#include "wifi_manager.h"
#include "config_mgr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT BIT0
static EventGroupHandle_t s_wifi_events;
static char s_ip_str[20] = "0.0.0.0";
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;

/* AP is disabled 60 seconds after STA gets an IP.  60 s gives the browser
 * enough time to finish loading the web UI before the AP disappears.
 * If STA later disconnects the AP is re-enabled so the device remains
 * accessible at 192.168.4.1 for re-configuration. */
#define AP_DISABLE_DELAY_US   (60LL  * 1000 * 1000)   /* 60 seconds  */

/* If credentials are saved but the STA never connects, close the setup AP
 * after this window.  Prevents "Nextube-Setup" broadcasting forever when
 * the device is deployed away from the configured network. */
#define AP_BOOT_TIMEOUT_US    (3LL * 60 * 1000 * 1000) /* 3 minutes   */

static esp_timer_handle_t s_ap_disable_timer   = NULL;
static esp_timer_handle_t s_ap_boot_timer      = NULL;

/* Set once the boot-window timer fires without a successful STA connection.
 * While true, WIFI_EVENT_STA_DISCONNECTED will NOT re-enable the AP —
 * the device keeps retrying STA silently.  Reset whenever new credentials
 * are saved so the AP comes back up for the new connection attempt. */
static bool s_ap_boot_expired  = false;

/* Set by wifi_manager_reconnect_sta() before it calls esp_wifi_connect()
 * directly.  Tells WIFI_EVENT_STA_DISCONNECTED NOT to fire a second
 * esp_wifi_connect() for that same reconnect attempt, which would create
 * two concurrent association requests and leave the TCP/IP stack in an
 * indeterminate state. */
static bool s_manual_reconnect = false;

static void ap_disable_cb(void *arg)
{
    ESP_LOGI(TAG, "STA connected – disabling setup AP");
    esp_wifi_set_mode(WIFI_MODE_STA);
}

static void ap_boot_timeout_cb(void *arg)
{
    if (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) return; /* connected in time */
    ESP_LOGI(TAG, "Boot AP timeout: STA not connected – closing setup AP");
    s_ap_boot_expired = true;
    esp_wifi_set_mode(WIFI_MODE_STA);
}

static void init_ap_timers(void)
{
    esp_timer_create_args_t a = { .callback = ap_disable_cb,     .name = "ap_disable" };
    esp_timer_create(&a, &s_ap_disable_timer);
    esp_timer_create_args_t b = { .callback = ap_boot_timeout_cb, .name = "ap_boot"    };
    esp_timer_create(&b, &s_ap_boot_timer);
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
            if (s_ap_disable_timer) esp_timer_stop(s_ap_disable_timer);
            if (!s_ap_boot_expired) {
                esp_wifi_set_mode(WIFI_MODE_APSTA);
            }
            if (s_manual_reconnect) {
                /* wifi_manager_reconnect_sta() already called esp_wifi_connect()
                 * directly — skip the auto-reconnect here to avoid a second
                 * concurrent association attempt. */
                ESP_LOGI(TAG, "STA disconnected (manual reconnect in progress)");
                s_manual_reconnect = false;
            } else {
                ESP_LOGW(TAG, "STA disconnected, retrying...");
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev = data;
            ESP_LOGI(TAG, "AP: client connected (AID=%d)", ev->aid);
            break;
        }
        default: break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "STA got IP: %s – AP will stop in 60 s", s_ip_str);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        /* Connected — cancel the boot-window timeout (no longer needed). */
        if (s_ap_boot_timer) esp_timer_stop(s_ap_boot_timer);
        /* Schedule graceful AP shutdown 60 s from now. */
        if (s_ap_disable_timer) {
            esp_timer_stop(s_ap_disable_timer);
            esp_timer_start_once(s_ap_disable_timer, AP_DISABLE_DELAY_US);
        }
    }
}

static void start_mdns(void)
{
    mdns_init();
    mdns_hostname_set("nextube-remaster");
    mdns_instance_name_set("Nextube Remaster");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://nextube-remaster.local");
}

void wifi_manager_start(void)
{
    char ssid[64], password[64];
    config_lock();
    const nextube_config_t *cfg = config_get();
    strncpy(ssid,     cfg->ssid,     sizeof(ssid)     - 1); ssid[sizeof(ssid)         - 1] = '\0';
    strncpy(password, cfg->password, sizeof(password) - 1); password[sizeof(password) - 1] = '\0';
    config_unlock();

    s_wifi_events = xEventGroupCreate();

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    init_ap_timers();

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    /* Start in APSTA so the setup AP is reachable immediately */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* Configure AP */
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = "Nextube-Setup",
            .password       = "",
            .max_connection = 4,
            .authmode       = WIFI_AUTH_OPEN,
            .channel        = 1,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    /* Always push an STA config — even when SSID is empty — to keep the
     * WiFi driver's NVS copy in sync with the application's config.json.
     * This ensures credentials written by wifi_manager_reconnect_sta() are
     * immediately active and avoids stale NVS data on first boot after a
     * SPIFFS-only update that wiped config.json. */
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    if (strlen(ssid) > 0) {
        ESP_LOGI(TAG, "STA: connecting to \"%s\"", ssid);
        /* Start the boot-window timer.  If STA does not obtain an IP within
         * AP_BOOT_TIMEOUT_US the setup AP is closed to avoid broadcasting
         * "Nextube-Setup" indefinitely on a deployed device. */
        esp_timer_start_once(s_ap_boot_timer, AP_BOOT_TIMEOUT_US);
        ESP_LOGI(TAG, "AP boot window: %.0f min", AP_BOOT_TIMEOUT_US / 60e6);
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    start_mdns();
    ESP_LOGI(TAG, "WiFi AP+STA started. AP SSID: Nextube-Setup");
}

void wifi_manager_reconnect_sta(void)
{
    char ssid[64], password[64];
    config_lock();
    const nextube_config_t *cfg = config_get();
    strncpy(ssid,     cfg->ssid,     sizeof(ssid)     - 1); ssid[sizeof(ssid)         - 1] = '\0';
    strncpy(password, cfg->password, sizeof(password) - 1); password[sizeof(password) - 1] = '\0';
    config_unlock();

    if (strlen(ssid) == 0) return;

    /* New credentials were saved — reset the boot-timeout state so the AP
     * stays visible while the user waits for the new credentials to work.
     * Re-arm the boot timer for another full window from now. */
    s_ap_boot_expired = false;
    if (s_ap_boot_timer) {
        esp_timer_stop(s_ap_boot_timer);
        esp_timer_start_once(s_ap_boot_timer, AP_BOOT_TIMEOUT_US);
    }
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid,     ssid,     sizeof(sta_cfg.sta.ssid)     - 1);
    strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    /* Set the flag BEFORE calling esp_wifi_disconnect() so that if the
     * WIFI_EVENT_STA_DISCONNECTED event fires synchronously (or very quickly)
     * it sees s_manual_reconnect=true and skips its own esp_wifi_connect(). */
    s_manual_reconnect = true;

    /* esp_wifi_disconnect() stops any in-progress connection attempt.
     * On first-time credential save the STA was never connected and this
     * call returns ESP_ERR_WIFI_NOT_CONNECT without firing a disconnect
     * event — which is exactly why we call esp_wifi_connect() ourselves
     * rather than relying on the event handler to do it. */
    esp_wifi_disconnect();
    esp_wifi_connect();
    ESP_LOGI(TAG, "STA: connecting to \"%s\"", ssid);
}

bool wifi_manager_is_connected(void)
{
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

const char *wifi_manager_get_ip(void) { return s_ip_str; }

void wifi_manager_apply_sta_credentials(void)
{
    char ssid[64], password[64];
    config_lock();
    const nextube_config_t *cfg = config_get();
    strncpy(ssid,     cfg->ssid,     sizeof(ssid)     - 1); ssid[sizeof(ssid)         - 1] = '\0';
    strncpy(password, cfg->password, sizeof(password) - 1); password[sizeof(password) - 1] = '\0';
    config_unlock();

    if (strlen(ssid) == 0) return;
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid,     ssid,     sizeof(sta_cfg.sta.ssid)     - 1);
    strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    ESP_LOGI(TAG, "STA credentials updated (no reconnect)");
}

void wifi_manager_scan_start(void)
{
    wifi_scan_config_t scan = { .show_hidden = true };
    esp_wifi_scan_start(&scan, false);
}
