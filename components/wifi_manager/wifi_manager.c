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

/* ── AP auto-disable / re-enable timers ─────────────────────────────── */
/* esp_wifi_set_mode() must not be called from within a WiFi event handler
 * (it takes the WiFi mutex, causing deadlock).  We defer via esp_timer
 * callbacks which run in the esp_timer task, outside the WiFi event task. */
static esp_timer_handle_t s_ap_disable_timer = NULL;
static esp_timer_handle_t s_ap_enable_timer  = NULL;

static void ap_disable_cb(void *arg)
{
    /* Switch to STA-only – hides the "Nextube-Setup" AP after connecting */
    if (esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK)
        ESP_LOGI(TAG, "AP disabled – STA connected");
}

static void ap_enable_cb(void *arg)
{
    /* Switch back to AP+STA – restores setup AP on disconnect */
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK)
        ESP_LOGI(TAG, "AP re-enabled – STA disconnected");
}

static void init_ap_timers(void)
{
    esp_timer_create_args_t a = { .callback = ap_disable_cb, .name = "ap_off" };
    esp_timer_create(&a, &s_ap_disable_timer);
    a.callback = ap_enable_cb; a.name = "ap_on";
    esp_timer_create(&a, &s_ap_enable_timer);
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
            ESP_LOGW(TAG, "STA disconnected, retrying...");
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
            /* Re-enable AP so user can reconfigure; defer mode switch out of handler */
            if (s_ap_enable_timer) {
                esp_timer_stop(s_ap_enable_timer);
                esp_timer_start_once(s_ap_enable_timer, 2 * 1000000ULL); /* 2 s */
            }
            esp_wifi_connect();
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
        ESP_LOGI(TAG, "STA got IP: %s", s_ip_str);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        /* Disable setup AP 5 s after connecting – gives any open browser time to
         * receive the save-settings response before the AP disappears. */
        if (s_ap_disable_timer) {
            esp_timer_stop(s_ap_disable_timer);
            esp_timer_start_once(s_ap_disable_timer, 5 * 1000000ULL); /* 5 s */
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
    const nextube_config_t *cfg = config_get();
    s_wifi_events = xEventGroupCreate();

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    /* AP+STA mode: always provide a captive portal AP */
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

    /* Configure STA if credentials exist */
    if (strlen(cfg->ssid) > 0) {
        wifi_config_t sta_cfg = {0};
        strncpy((char *)sta_cfg.sta.ssid, cfg->ssid, sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char *)sta_cfg.sta.password, cfg->password, sizeof(sta_cfg.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        ESP_LOGI(TAG, "STA: connecting to \"%s\"", cfg->ssid);
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    init_ap_timers();
    start_mdns();
    ESP_LOGI(TAG, "WiFi AP+STA started. AP SSID: Nextube-Setup");
}

void wifi_manager_reconnect_sta(void)
{
    const nextube_config_t *cfg = config_get();
    if (strlen(cfg->ssid) == 0) return;

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid,     cfg->ssid,     sizeof(sta_cfg.sta.ssid)     - 1);
    strncpy((char *)sta_cfg.sta.password, cfg->password, sizeof(sta_cfg.sta.password) - 1);

    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "STA: reconnecting to \"%s\"", cfg->ssid);
}

bool wifi_manager_is_connected(void)
{
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

const char *wifi_manager_get_ip(void) { return s_ip_str; }

void wifi_manager_scan_start(void)
{
    wifi_scan_config_t scan = { .show_hidden = true };
    esp_wifi_scan_start(&scan, false);
}
