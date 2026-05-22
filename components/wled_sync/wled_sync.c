/**
 * @file wled_sync.c
 * @brief WLED UDP Notifier v2 receiver implementation.
 *
 * Listens for WLED UDP Notifier v2 broadcasts on the configured port
 * (default 21324) and stores the primary colour + brightness so the LED
 * task can mirror WLED-controlled strips in real time.
 */

#include "wled_sync.h"
#include "config_mgr.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "wled_sync";

/* ── Shared state ───────────────────────────────────────────────────── */
static wled_sync_state_t s_state      = {0};
static bool              s_have_state = false;
static SemaphoreHandle_t s_mutex      = NULL;
static bool              s_started    = false;

/* ── Public API ─────────────────────────────────────────────────────── */

bool wled_sync_get(wled_sync_state_t *out)
{
    if (!s_mutex || !s_have_state) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_mutex);
    return true;
}

/* ── Listener task ──────────────────────────────────────────────────── */

static void wled_sync_task(void *arg)
{
    /* Wait for WiFi before binding — the network stack needs to be up */
    ESP_LOGI(TAG, "Waiting for WiFi before binding...");
    while (!wifi_manager_is_connected())
        vTaskDelay(pdMS_TO_TICKS(1000));

    /* Read port under the config lock */
    config_lock();
    uint16_t port = config_get()->wled_sync_port;
    config_unlock();
    if (port == 0) port = 21324;

    /* Create a UDP socket */
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    /* SO_BROADCAST and SO_REUSEADDR allow receiving broadcast packets
     * and allow the port to be rebound quickly after a restart. */
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,  &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed on port %u: errno %d", port, errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Listening for WLED Notifier v2 on UDP port %u", port);

    /* Minimum WLED Notifier v2 packet is 12 bytes; allocate 32 to be safe */
    uint8_t buf[32];
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);

    while (1) {
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&src, &src_len);
        if (len < 12) continue;       /* too short to be a valid v2 packet */
        if (buf[0] != 9) continue;    /* not a WLED Notifier v2 packet     */

        /* byte[11] = brightness 0–255; bytes[1-3] = primary R, G, B */
        uint8_t bri = buf[11];
        uint8_t r   = (uint8_t)((buf[1] * (uint16_t)bri) / 255u);
        uint8_t g   = (uint8_t)((buf[2] * (uint16_t)bri) / 255u);
        uint8_t b   = (uint8_t)((buf[3] * (uint16_t)bri) / 255u);

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state.r    = r;
        s_state.g    = g;
        s_state.b    = b;
        s_state.on   = (bri > 0);
        s_have_state = true;
        xSemaphoreGive(s_mutex);

        ESP_LOGD(TAG, "Sync → #%02X%02X%02X  bri=%u  fx=%u",
                 r, g, b, bri, buf[4]);
    }
}

/* ── wled_sync_start ────────────────────────────────────────────────── */

void wled_sync_start(void)
{
    if (s_started) return;   /* idempotent — safe to call multiple times */
    s_started = true;
    s_mutex   = xSemaphoreCreateMutex();
    /* Core 0, priority 3 — below led_task (priority 4 on Core 1).
     * recvfrom() blocks so no CPU is wasted when no packets arrive. */
    xTaskCreatePinnedToCore(wled_sync_task, "wled_sync", 3072, NULL, 3, NULL, 0);
    ESP_LOGI(TAG, "WLED sync task started");
}
