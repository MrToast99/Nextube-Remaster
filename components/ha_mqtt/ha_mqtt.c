/**
 * @file ha_mqtt.c
 * @brief Home Assistant MQTT integration for Nextube-Remaster.
 *
 * Topics (using device hostname as unique ID):
 *
 * Published:
 *   nextube/<host>/sensor/temperature/state  {"temperature": 21.4}
 *   nextube/<host>/sensor/humidity/state     {"humidity": 55.1}
 *   nextube/<host>/mode/state                "Clock"
 *   nextube/<host>/display/state             "ON" or "OFF"
 *   nextube/<host>/brightness/state          "75"
 *   nextube/<host>/theme/state               "NixieOY"
 *   nextube/<host>/rotation/state            "ON" or "OFF"
 *   nextube/<host>/ticker/state              current ticker text, or "" when cleared
 *   nextube/<host>/ticker_speed/state        ticker scroll speed (px per 200 ms tick)
 *   nextube/<host>/ticker_sound/state        "ON" or "OFF" — chime on ticker text
 *   nextube/<host>/ntp/xtal_drift/state      signed ms; per NTP sync (retained)
 *   nextube/<host>/ntp/rtc_err/state         ms; PCF worst hold error (retained)
 *   nextube/<host>/health/state              {"rssi","heap","uptime_min"} / 60 s
 *                                            (optional group: mqtt_pub_health)
 *   nextube/<host>/button/state              "left"/"middle"/"right" per press
 *                                            (optional group: mqtt_pub_buttons)
 *
 * Subscribed:
 *   nextube/<host>/mode/set                  "Weather"
 *   nextube/<host>/display/set               "ON" or "OFF"
 *   nextube/<host>/brightness/set            "75"
 *   nextube/<host>/theme/set                 "DarkSlate"
 *   nextube/<host>/rotation/set              "ON" or "OFF"
 *   nextube/<host>/ticker/set                UTF-8 string ≤ 255 chars; empty = cancel
 *   nextube/<host>/ticker_speed/set          ticker scroll speed 1–20 (px per 200 ms tick)
 *   nextube/<host>/ticker_sound/set          "ON"/"OFF" — play cfg->ticker_file on ticker text
 *
 * HA auto-discovery:
 *   homeassistant/sensor/<host>_temp/config
 *   homeassistant/sensor/<host>_hum/config
 *   homeassistant/sensor/<host>_fw/config
 *   homeassistant/select/<host>_mode/config
 *   homeassistant/select/<host>_theme/config
 *   homeassistant/switch/<host>_display/config
 *   homeassistant/switch/<host>_rotation/config
 *   homeassistant/number/<host>_brightness/config
 *   homeassistant/text/<host>_ticker/config
 *   homeassistant/number/<host>_ticker_speed/config
 *   homeassistant/switch/<host>_ticker_sound/config
 *   homeassistant/sensor/<host>_xtal_drift/config
 *   homeassistant/sensor/<host>_rtc_err/config
 *
 * Firmware version:
 *   nextube/<host>/firmware/state              "1.10.0"  (retained)
 */

#include "ha_mqtt.h"
#include "config_mgr.h"
#include "fw_version.h"
#include "sht30.h"
#include "weather.h"     /* air-quality sensor (weather_get_aqi)        */
#include "wifi_manager.h"
#include "display.h"
#include "audio.h"       /* ticker notification chime (audio_play_file) */
#include "ntp_time.h"    /* NTP sync-stats listener → HA sensors        */
#include "esp_wifi.h"    /* RSSI for the optional health sensors        */
#include "esp_system.h"  /* esp_get_free_heap_size                      */
#include "esp_timer.h"   /* uptime for the health sensors               */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"

static const char *TAG = "ha_mqtt";

/* ── File-scope state ──────────────────────────────────────────────── */
static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool            s_connected = false;

/* ── Topic helpers ─────────────────────────────────────────────────── */
#define TOPIC_MAXLEN 96

/* ── Ticker state ──────────────────────────────────────────────────── */
#define TICKER_MAX_LEN 255
static char              s_ticker_text[TICKER_MAX_LEN + 1] = "";
static SemaphoreHandle_t s_ticker_mutex                    = NULL;
/* Precomputed topic strings (built once in ha_mqtt_start) */
static char s_topic_ticker_set  [TOPIC_MAXLEN] = "";
static char s_topic_ticker_state[TOPIC_MAXLEN] = "";

/* Cached config values read at start() — broker may not be reachable
 * immediately; these are used throughout the task lifetime. */
static char   s_hostname[32];
static char   s_broker[64];
static uint16_t s_port;
static char   s_user[32];
static char   s_pass[64];
static bool   s_discovery;

static void make_topic(char *buf, size_t n, const char *suffix)
{
    snprintf(buf, n, "nextube/%s/%s", s_hostname, suffix);
}

/* ── Publish helpers ───────────────────────────────────────────────── */
static void publish(const char *topic, const char *payload, int retain)
{
    if (!s_connected || !s_client) return;
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, retain);
}

/* ── Discovery payloads ────────────────────────────────────────────── */
static void publish_discovery(void)
{
    char topic[TOPIC_MAXLEN];
    char payload[768];

    /* Device block — reused in every payload.
     * sw_version appears in HA → Devices → device card as "Firmware version". */
    char dev[192];
    snprintf(dev, sizeof(dev),
             "\"identifiers\":[\"%s\"],\"name\":\"Nextube\","
             "\"model\":\"Nextube-Remaster\","
             "\"manufacturer\":\"MrToast99\","
             "\"sw_version\":\"%s\"",
             s_hostname, FW_VERSION_STR);

    /* ── Temperature sensor ── */
    char state_t[TOPIC_MAXLEN];
    make_topic(state_t, sizeof(state_t), "sensor/temperature/state");
    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s_temp/config", s_hostname);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"Nextube Temperature\","
             "\"unique_id\":\"%s_temp\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.temperature }}\","
             "\"unit_of_measurement\":\"°C\","
             "\"device_class\":\"temperature\","
             "\"device\":{%s}"
             "}",
             s_hostname, state_t, dev);
    publish(topic, payload, 1);

    /* ── Humidity sensor ── */
    make_topic(state_t, sizeof(state_t), "sensor/humidity/state");
    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s_hum/config", s_hostname);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"Nextube Humidity\","
             "\"unique_id\":\"%s_hum\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.humidity }}\","
             "\"unit_of_measurement\":\"%%\","
             "\"device_class\":\"humidity\","
             "\"device\":{%s}"
             "}",
             s_hostname, state_t, dev);
    publish(topic, payload, 1);

    /* ── Air-quality sensor (outdoor AQI from Open-Meteo) ── */
    make_topic(state_t, sizeof(state_t), "sensor/aqi/state");
    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s_aqi/config", s_hostname);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"Nextube Air Quality\","
             "\"unique_id\":\"%s_aqi\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.aqi }}\","
             "\"device_class\":\"aqi\","
             "\"state_class\":\"measurement\","
             "\"device\":{%s}"
             "}",
             s_hostname, state_t, dev);
    publish(topic, payload, 1);

    /* ── Mode select ── */
    char mode_state[TOPIC_MAXLEN], mode_cmd[TOPIC_MAXLEN];
    make_topic(mode_state, sizeof(mode_state), "mode/state");
    make_topic(mode_cmd,   sizeof(mode_cmd),   "mode/set");
    snprintf(topic, sizeof(topic),
             "homeassistant/select/%s_mode/config", s_hostname);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"Nextube Mode\","
             "\"unique_id\":\"%s_mode\","
             "\"state_topic\":\"%s\","
             "\"command_topic\":\"%s\","
             "\"options\":["
               "\"Clock\",\"Countdown\",\"Pomodoro\","
               "\"YouTube\",\"Date\",\"Album\",\"Weather\","
               "\"Spectrum\",\"Instagram\",\"TikTok\",\"Mastodon\""
             "],"
             "\"device\":{%s}"
             "}",
             s_hostname, mode_state, mode_cmd, dev);
    publish(topic, payload, 1);

    /* ── Display switch (backlight on/off) ── */
    char disp_state[TOPIC_MAXLEN], disp_cmd[TOPIC_MAXLEN];
    make_topic(disp_state, sizeof(disp_state), "display/state");
    make_topic(disp_cmd,   sizeof(disp_cmd),   "display/set");
    snprintf(topic, sizeof(topic),
             "homeassistant/switch/%s_display/config", s_hostname);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"Nextube Display\","
             "\"unique_id\":\"%s_display\","
             "\"state_topic\":\"%s\","
             "\"command_topic\":\"%s\","
             "\"payload_on\":\"ON\","
             "\"payload_off\":\"OFF\","
             "\"device\":{%s}"
             "}",
             s_hostname, disp_state, disp_cmd, dev);
    publish(topic, payload, 1);

    /* ── Brightness number ── */
    char br_state[TOPIC_MAXLEN], br_cmd[TOPIC_MAXLEN];
    make_topic(br_state, sizeof(br_state), "brightness/state");
    make_topic(br_cmd,   sizeof(br_cmd),   "brightness/set");
    snprintf(topic, sizeof(topic),
             "homeassistant/number/%s_brightness/config", s_hostname);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"Nextube Brightness\","
             "\"unique_id\":\"%s_brightness\","
             "\"state_topic\":\"%s\","
             "\"command_topic\":\"%s\","
             "\"min\":0,\"max\":100,\"step\":1,"
             "\"mode\":\"slider\","
             "\"device\":{%s}"
             "}",
             s_hostname, br_state, br_cmd, dev);
    publish(topic, payload, 1);

    /* ── Theme select ── */
    char theme_state[TOPIC_MAXLEN], theme_cmd[TOPIC_MAXLEN];
    make_topic(theme_state, sizeof(theme_state), "theme/state");
    make_topic(theme_cmd,   sizeof(theme_cmd),   "theme/set");
    snprintf(topic, sizeof(topic),
             "homeassistant/select/%s_theme/config", s_hostname);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"Nextube Theme\","
             "\"unique_id\":\"%s_theme\","
             "\"state_topic\":\"%s\","
             "\"command_topic\":\"%s\","
             "\"options\":["
               "\"NixieOY\",\"FlipClock\",\"DarkSlate\","
               "\"DotMatrixRG\",\"DotMatrixY\",\"Formula1\","
               "\"GlitchGR\",\"LightFuture\",\"NotionRain\","
               "\"RedDigits\",\"RetroPaper\",\"WireMesh\","
               "\"WeatherLive\",\"WeatherLive Demo\""
             "],"
             "\"icon\":\"mdi:palette\","
             "\"device\":{%s}"
             "}",
             s_hostname, theme_state, theme_cmd, dev);
    publish(topic, payload, 1);

    /* ── Mode rotation switch ── */
    char rot_state[TOPIC_MAXLEN], rot_cmd[TOPIC_MAXLEN];
    make_topic(rot_state, sizeof(rot_state), "rotation/state");
    make_topic(rot_cmd,   sizeof(rot_cmd),   "rotation/set");
    snprintf(topic, sizeof(topic),
             "homeassistant/switch/%s_rotation/config", s_hostname);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"Nextube Mode Rotation\","
             "\"unique_id\":\"%s_rotation\","
             "\"state_topic\":\"%s\","
             "\"command_topic\":\"%s\","
             "\"payload_on\":\"ON\","
             "\"payload_off\":\"OFF\","
             "\"icon\":\"mdi:autorenew\","
             "\"device\":{%s}"
             "}",
             s_hostname, rot_state, rot_cmd, dev);
    publish(topic, payload, 1);

    /* ── Firmware version sensor (diagnostic) ── */
    char fw_state[TOPIC_MAXLEN];
    make_topic(fw_state, sizeof(fw_state), "firmware/state");
    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s_fw/config", s_hostname);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"Nextube Firmware\","
             "\"unique_id\":\"%s_fw\","
             "\"state_topic\":\"%s\","
             "\"entity_category\":\"diagnostic\","
             "\"icon\":\"mdi:chip\","
             "\"device\":{%s}"
             "}",
             s_hostname, fw_state, dev);
    publish(topic, payload, 1);

    ESP_LOGI(TAG, "HA auto-discovery payloads published");
}

/* ── State publishers ─────────────────────────────────────────────── */
static void publish_mode(app_mode_t mode)
{
    char topic[TOPIC_MAXLEN];
    make_topic(topic, sizeof(topic), "mode/state");
    publish(topic, app_mode_name(mode), 0);
}

static void publish_display(bool on)
{
    char topic[TOPIC_MAXLEN];
    make_topic(topic, sizeof(topic), "display/state");
    publish(topic, on ? "ON" : "OFF", 0);
}

static void publish_brightness(uint8_t val)
{
    char topic[TOPIC_MAXLEN];
    char buf[8];
    make_topic(topic, sizeof(topic), "brightness/state");
    snprintf(buf, sizeof(buf), "%u", val);
    publish(topic, buf, 0);
}

static void publish_theme(const char *theme)
{
    char topic[TOPIC_MAXLEN];
    make_topic(topic, sizeof(topic), "theme/state");
    publish(topic, theme, 0);
}

static void publish_rotation(bool enabled)
{
    char topic[TOPIC_MAXLEN];
    make_topic(topic, sizeof(topic), "rotation/state");
    publish(topic, enabled ? "ON" : "OFF", 0);
}

static void publish_ticker_speed(int px)
{
    char topic[TOPIC_MAXLEN];
    char buf[8];
    make_topic(topic, sizeof(topic), "ticker_speed/state");
    snprintf(buf, sizeof(buf), "%d", px);
    publish(topic, buf, 0);
}

static void publish_ticker_sound(bool enabled)
{
    char topic[TOPIC_MAXLEN];
    make_topic(topic, sizeof(topic), "ticker_sound/state");
    publish(topic, enabled ? "ON" : "OFF", 0);
}

/* ── NTP sync-stats → HA sensors ─────────────────────────────────────
 * The listener runs in the SNTP callback context — which is lwIP's tcpip
 * thread (tiT).  It MUST NOT take any lock or call esp_mqtt_client_publish:
 * the publish acquires the MQTT client mutex, and if the MQTT task holds it
 * while blocked in a socket call that needs tiT to make progress, the two
 * deadlock and ALL networking freezes silently (observed: web UI, weather
 * and MQTT all dead from the first steady-state sync, no errors, while
 * non-network tasks kept running).  So the listener only stashes the values;
 * the MQTT task's 60 s loop publishes them from a safe context. */
static volatile int32_t s_ntp_pend_drift = 0;
static volatile float   s_ntp_pend_pcf   = -1.0f;
static volatile bool    s_ntp_pend       = false;

static void on_ntp_sync_stats(int32_t xtal_drift_ms, float pcf_max_err_ms,
                              int discipline_mode)
{
    (void)discipline_mode;
    s_ntp_pend_drift = xtal_drift_ms;
    s_ntp_pend_pcf   = pcf_max_err_ms;
    s_ntp_pend       = true;        /* written last — flag publishes the pair */
}

/* Called from the MQTT task's publish loop.  Retained messages so HA shows
 * the last sync even after a broker restart. */
static void publish_ntp_stats(void)
{
    char topic[TOPIC_MAXLEN];
    char val[16];

    make_topic(topic, sizeof(topic), "ntp/xtal_drift/state");
    snprintf(val, sizeof(val), "%d", (int)s_ntp_pend_drift);
    esp_mqtt_client_publish(s_client, topic, val, 0, 0, /*retain=*/1);

    float pcf = s_ntp_pend_pcf;
    if (pcf >= 0.0f) {
        make_topic(topic, sizeof(topic), "ntp/rtc_err/state");
        snprintf(val, sizeof(val), "%.1f", (double)pcf);
        esp_mqtt_client_publish(s_client, topic, val, 0, 0, /*retain=*/1);
    }
}

/* ── Optional health sensors: WiFi RSSI / free heap / uptime ─────────
 * One JSON payload on health/state, split into HA sensors by
 * value_template in the discovery configs.  Published from the 60 s
 * loop when cfg->mqtt_pub_health is set. */
static void publish_health(void)
{
    wifi_ap_record_t ap;
    int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;

    char topic[TOPIC_MAXLEN];
    char payload[96];
    make_topic(topic, sizeof(topic), "health/state");
    snprintf(payload, sizeof(payload),
             "{\"rssi\":%d,\"heap\":%u,\"uptime_min\":%llu}",
             rssi,
             (unsigned)esp_get_free_heap_size(),
             (unsigned long long)(esp_timer_get_time() / 60000000LL));
    publish(topic, payload, 0);
}

/* ── Optional button events: touch presses as HA device triggers ─────
 * Called from the touch handler (main.c).  Safe before MQTT is up and
 * when the group is disabled — both no-op. */
void ha_mqtt_publish_button(const char *btn)
{
    if (!s_connected || !s_client || !btn) return;
    config_lock();
    bool en = config_get()->mqtt_pub_buttons;
    config_unlock();
    if (!en) return;

    char topic[TOPIC_MAXLEN];
    make_topic(topic, sizeof(topic), "button/state");
    esp_mqtt_client_publish(s_client, topic, btn, 0, 0, 0);
}

static void publish_sensors(void)
{
    char topic[TOPIC_MAXLEN];
    char payload[64];

    /* Outdoor air quality — independent of the SHT30, so publish before the
     * sensor-validity gate below.  weather_get_aqi() returns -1 until the first
     * successful fetch. */
    int aqi = weather_get_aqi(NULL);
    if (aqi >= 0) {
        make_topic(topic, sizeof(topic), "sensor/aqi/state");
        snprintf(payload, sizeof(payload), "{\"aqi\":%d}", aqi);
        publish(topic, payload, 0);
    }

    const sht30_reading_t *s = sht30_get();
    if (!s || !s->valid) return;

    make_topic(topic, sizeof(topic), "sensor/temperature/state");
    snprintf(payload, sizeof(payload), "{\"temperature\":%.1f}", (double)s->temp_c);
    publish(topic, payload, 0);

    make_topic(topic, sizeof(topic), "sensor/humidity/state");
    snprintf(payload, sizeof(payload), "{\"humidity\":%.1f}", (double)s->humidity);
    publish(topic, payload, 0);
}

/* ── Ticker discovery ─────────────────────────────────────────────── */
static void publish_ticker_discovery(void)
{
    char topic[TOPIC_MAXLEN];
    char payload[768];
    char dev[192];
    snprintf(dev, sizeof(dev),
             "\"identifiers\":[\"%s\"],\"name\":\"Nextube\","
             "\"model\":\"Nextube-Remaster\","
             "\"manufacturer\":\"MrToast99\","
             "\"sw_version\":\"%s\"",
             s_hostname, FW_VERSION_STR);

    snprintf(topic, sizeof(topic),
             "homeassistant/text/%s_ticker/config", s_hostname);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"Nextube Ticker\","
             "\"unique_id\":\"%s_ticker\","
             "\"state_topic\":\"%s\","
             "\"command_topic\":\"%s\","
             "\"max\":255,"
             "\"icon\":\"mdi:message-text\","
             "\"device\":{%s}"
             "}",
             s_hostname, s_topic_ticker_state, s_topic_ticker_set, dev);
    publish(topic, payload, 1);

    /* ── Ticker speed number (px per 200 ms tick; higher = faster) ── */
    char ts_state[TOPIC_MAXLEN], ts_cmd[TOPIC_MAXLEN];
    make_topic(ts_state, sizeof(ts_state), "ticker_speed/state");
    make_topic(ts_cmd,   sizeof(ts_cmd),   "ticker_speed/set");
    snprintf(topic, sizeof(topic),
             "homeassistant/number/%s_ticker_speed/config", s_hostname);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"Nextube Ticker Speed\","
             "\"unique_id\":\"%s_ticker_speed\","
             "\"state_topic\":\"%s\","
             "\"command_topic\":\"%s\","
             "\"min\":1,\"max\":20,\"step\":1,"
             "\"mode\":\"slider\","
             "\"icon\":\"mdi:speedometer\","
             "\"device\":{%s}"
             "}",
             s_hostname, ts_state, ts_cmd, dev);
    publish(topic, payload, 1);

    /* ── Ticker sound switch (chime when ticker text arrives) ──
     * Plays cfg->ticker_file through the speaker on every non-empty
     * ticker/set.  Requires audio output to be enabled in device settings —
     * audio_play_file() is a no-op otherwise. */
    char sn_state[TOPIC_MAXLEN], sn_cmd[TOPIC_MAXLEN];
    make_topic(sn_state, sizeof(sn_state), "ticker_sound/state");
    make_topic(sn_cmd,   sizeof(sn_cmd),   "ticker_sound/set");
    snprintf(topic, sizeof(topic),
             "homeassistant/switch/%s_ticker_sound/config", s_hostname);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"Nextube Ticker Sound\","
             "\"unique_id\":\"%s_ticker_sound\","
             "\"state_topic\":\"%s\","
             "\"command_topic\":\"%s\","
             "\"icon\":\"mdi:bell-ring\","
             "\"device\":{%s}"
             "}",
             s_hostname, sn_state, sn_cmd, dev);
    publish(topic, payload, 1);

    /* ── NTP timekeeping sensors (clock-discipline telemetry) ──
     * Published after every steady-state NTP sync (see on_ntp_sync_stats).
     * state_class "measurement" makes HA record long-term statistics, so
     * crystal drift and RTC hold accuracy become graphable history. */
    char nx_state[TOPIC_MAXLEN], nr_state[TOPIC_MAXLEN];
    make_topic(nx_state, sizeof(nx_state), "ntp/xtal_drift/state");
    make_topic(nr_state, sizeof(nr_state), "ntp/rtc_err/state");

    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s_xtal_drift/config", s_hostname);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"Nextube XTAL Drift\","
             "\"unique_id\":\"%s_xtal_drift\","
             "\"state_topic\":\"%s\","
             "\"unit_of_measurement\":\"ms\","
             "\"state_class\":\"measurement\","
             "\"icon\":\"mdi:sine-wave\","
             "\"device\":{%s}"
             "}",
             s_hostname, nx_state, dev);
    publish(topic, payload, 1);

    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s_rtc_err/config", s_hostname);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"Nextube RTC Max Error\","
             "\"unique_id\":\"%s_rtc_err\","
             "\"state_topic\":\"%s\","
             "\"unit_of_measurement\":\"ms\","
             "\"state_class\":\"measurement\","
             "\"icon\":\"mdi:clock-check-outline\","
             "\"device\":{%s}"
             "}",
             s_hostname, nr_state, dev);
    publish(topic, payload, 1);

    /* ── Optional groups (web-UI checkboxes) ──
     * Discovery for these is published only when their group is enabled at
     * (re)connect time; enabling a checkbox takes effect for HA entities on
     * the next broker reconnect or reboot. */
    config_lock();
    bool pub_health  = config_get()->mqtt_pub_health;
    bool pub_buttons = config_get()->mqtt_pub_buttons;
    config_unlock();

    if (pub_health) {
        char h_state[TOPIC_MAXLEN];
        make_topic(h_state, sizeof(h_state), "health/state");
        static const struct { const char *id, *name, *tmpl, *unit, *icon, *devclass; } k_h[] = {
            { "rssi",   "Nextube WiFi RSSI", "{{ value_json.rssi }}",
              "dBm", "mdi:wifi",        ",\"device_class\":\"signal_strength\"" },
            { "heap",   "Nextube Free Heap", "{{ value_json.heap }}",
              "B",   "mdi:memory",      "" },
            { "uptime", "Nextube Uptime",    "{{ value_json.uptime_min }}",
              "min", "mdi:timer-outline", "" },
        };
        for (size_t i = 0; i < sizeof(k_h)/sizeof(k_h[0]); i++) {
            snprintf(topic, sizeof(topic),
                     "homeassistant/sensor/%s_%s/config", s_hostname, k_h[i].id);
            snprintf(payload, sizeof(payload),
                     "{"
                     "\"name\":\"%s\","
                     "\"unique_id\":\"%s_%s\","
                     "\"state_topic\":\"%s\","
                     "\"value_template\":\"%s\","
                     "\"unit_of_measurement\":\"%s\","
                     "\"state_class\":\"measurement\","
                     "\"icon\":\"%s\""
                     "%s,"
                     "\"device\":{%s}"
                     "}",
                     k_h[i].name, s_hostname, k_h[i].id, h_state, k_h[i].tmpl,
                     k_h[i].unit, k_h[i].icon, k_h[i].devclass, dev);
            publish(topic, payload, 1);
        }
    }

    if (pub_buttons) {
        char b_state[TOPIC_MAXLEN];
        make_topic(b_state, sizeof(b_state), "button/state");
        static const char *const k_btns[] = { "left", "middle", "right" };
        for (size_t i = 0; i < 3; i++) {
            snprintf(topic, sizeof(topic),
                     "homeassistant/device_automation/%s_btn_%s/config",
                     s_hostname, k_btns[i]);
            snprintf(payload, sizeof(payload),
                     "{"
                     "\"automation_type\":\"trigger\","
                     "\"type\":\"button_short_press\","
                     "\"subtype\":\"%s\","
                     "\"topic\":\"%s\","
                     "\"payload\":\"%s\","
                     "\"device\":{%s}"
                     "}",
                     k_btns[i], b_state, k_btns[i], dev);
            publish(topic, payload, 1);
        }
    }
}

/* ── Ticker public API ────────────────────────────────────────────── */
bool ha_mqtt_ticker_active(char *out, size_t len)
{
    if (!s_ticker_mutex) return false;
    xSemaphoreTake(s_ticker_mutex, portMAX_DELAY);
    bool active = (s_ticker_text[0] != '\0');
    if (active && out && len > 0) {
        size_t n = strlen(s_ticker_text);
        if (n >= len) n = len - 1;
        memcpy(out, s_ticker_text, n);
        out[n] = '\0';
    }
    xSemaphoreGive(s_ticker_mutex);
    return active;
}

void ha_mqtt_ticker_clear(void)
{
    if (!s_ticker_mutex) return;
    xSemaphoreTake(s_ticker_mutex, portMAX_DELAY);
    s_ticker_text[0] = '\0';
    xSemaphoreGive(s_ticker_mutex);
    /* Publish empty state so HA reflects the cleared ticker */
    publish(s_topic_ticker_state, "", 0);
    ESP_LOGI(TAG, "Ticker cleared");
}

/* ── MQTT event handler ────────────────────────────────────────────── */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to mqtt://%s:%u", s_broker, s_port);
        s_connected = true;

        /* Publish HA discovery payloads (only if SHT30 is present for sensors) */
        if (s_discovery) {
            if (!sht30_is_present()) {
                /* Skip temperature/humidity discovery — publish mode/display/brightness only */
                char topic[TOPIC_MAXLEN];
                char payload[768];   /* mode-select payload alone can reach ~580 bytes */
                char dev[192];
                snprintf(dev, sizeof(dev),
                         "\"identifiers\":[\"%s\"],\"name\":\"Nextube\","
                         "\"model\":\"Nextube-Remaster\","
                         "\"manufacturer\":\"MrToast99\","
                         "\"sw_version\":\"%s\"",
                         s_hostname, FW_VERSION_STR);

                char mode_state[TOPIC_MAXLEN], mode_cmd[TOPIC_MAXLEN];
                make_topic(mode_state, sizeof(mode_state), "mode/state");
                make_topic(mode_cmd,   sizeof(mode_cmd),   "mode/set");
                snprintf(topic, sizeof(topic),
                         "homeassistant/select/%s_mode/config", s_hostname);
                snprintf(payload, sizeof(payload),
                         "{"
                         "\"name\":\"Nextube Mode\","
                         "\"unique_id\":\"%s_mode\","
                         "\"state_topic\":\"%s\","
                         "\"command_topic\":\"%s\","
                         "\"options\":["
                           "\"Clock\",\"Countdown\",\"Pomodoro\","
                           "\"YouTube\",\"Date\",\"Album\",\"Weather\","
                           "\"Spectrum\",\"Instagram\",\"TikTok\",\"Mastodon\""
                         "],"
                         "\"device\":{%s}"
                         "}",
                         s_hostname, mode_state, mode_cmd, dev);
                publish(topic, payload, 1);

                char disp_state[TOPIC_MAXLEN], disp_cmd[TOPIC_MAXLEN];
                make_topic(disp_state, sizeof(disp_state), "display/state");
                make_topic(disp_cmd,   sizeof(disp_cmd),   "display/set");
                snprintf(topic, sizeof(topic),
                         "homeassistant/switch/%s_display/config", s_hostname);
                snprintf(payload, sizeof(payload),
                         "{"
                         "\"name\":\"Nextube Display\","
                         "\"unique_id\":\"%s_display\","
                         "\"state_topic\":\"%s\","
                         "\"command_topic\":\"%s\","
                         "\"payload_on\":\"ON\","
                         "\"payload_off\":\"OFF\","
                         "\"device\":{%s}"
                         "}",
                         s_hostname, disp_state, disp_cmd, dev);
                publish(topic, payload, 1);

                char br_state[TOPIC_MAXLEN], br_cmd[TOPIC_MAXLEN];
                make_topic(br_state, sizeof(br_state), "brightness/state");
                make_topic(br_cmd,   sizeof(br_cmd),   "brightness/set");
                snprintf(topic, sizeof(topic),
                         "homeassistant/number/%s_brightness/config", s_hostname);
                snprintf(payload, sizeof(payload),
                         "{"
                         "\"name\":\"Nextube Brightness\","
                         "\"unique_id\":\"%s_brightness\","
                         "\"state_topic\":\"%s\","
                         "\"command_topic\":\"%s\","
                         "\"min\":0,\"max\":100,\"step\":1,"
                         "\"mode\":\"slider\","
                         "\"device\":{%s}"
                         "}",
                         s_hostname, br_state, br_cmd, dev);
                publish(topic, payload, 1);

                /* Theme select */
                char theme_state_ns[TOPIC_MAXLEN], theme_cmd_ns[TOPIC_MAXLEN];
                make_topic(theme_state_ns, sizeof(theme_state_ns), "theme/state");
                make_topic(theme_cmd_ns,   sizeof(theme_cmd_ns),   "theme/set");
                snprintf(topic, sizeof(topic),
                         "homeassistant/select/%s_theme/config", s_hostname);
                snprintf(payload, sizeof(payload),
                         "{"
                         "\"name\":\"Nextube Theme\","
                         "\"unique_id\":\"%s_theme\","
                         "\"state_topic\":\"%s\","
                         "\"command_topic\":\"%s\","
                         "\"options\":["
                           "\"NixieOY\",\"FlipClock\",\"DarkSlate\","
                           "\"DotMatrixRG\",\"DotMatrixY\",\"Formula1\","
                           "\"GlitchGR\",\"LightFuture\",\"NotionRain\","
                           "\"RedDigits\",\"RetroPaper\",\"WireMesh\","
                           "\"WeatherLive\",\"WeatherLive Demo\""
                         "],"
                         "\"icon\":\"mdi:palette\","
                         "\"device\":{%s}"
                         "}",
                         s_hostname, theme_state_ns, theme_cmd_ns, dev);
                publish(topic, payload, 1);

                /* Rotation switch */
                char rot_state_ns[TOPIC_MAXLEN], rot_cmd_ns[TOPIC_MAXLEN];
                make_topic(rot_state_ns, sizeof(rot_state_ns), "rotation/state");
                make_topic(rot_cmd_ns,   sizeof(rot_cmd_ns),   "rotation/set");
                snprintf(topic, sizeof(topic),
                         "homeassistant/switch/%s_rotation/config", s_hostname);
                snprintf(payload, sizeof(payload),
                         "{"
                         "\"name\":\"Nextube Mode Rotation\","
                         "\"unique_id\":\"%s_rotation\","
                         "\"state_topic\":\"%s\","
                         "\"command_topic\":\"%s\","
                         "\"payload_on\":\"ON\","
                         "\"payload_off\":\"OFF\","
                         "\"icon\":\"mdi:autorenew\","
                         "\"device\":{%s}"
                         "}",
                         s_hostname, rot_state_ns, rot_cmd_ns, dev);
                publish(topic, payload, 1);

                /* Firmware version sensor (same whether SHT30 present or not) */
                char fw_state[TOPIC_MAXLEN];
                make_topic(fw_state, sizeof(fw_state), "firmware/state");
                snprintf(topic, sizeof(topic),
                         "homeassistant/sensor/%s_fw/config", s_hostname);
                snprintf(payload, sizeof(payload),
                         "{"
                         "\"name\":\"Nextube Firmware\","
                         "\"unique_id\":\"%s_fw\","
                         "\"state_topic\":\"%s\","
                         "\"entity_category\":\"diagnostic\","
                         "\"icon\":\"mdi:chip\","
                         "\"device\":{%s}"
                         "}",
                         s_hostname, fw_state, dev);
                publish(topic, payload, 1);
            } else {
                publish_discovery();   /* full discovery including sensors */
            }
        }

        /* Subscribe to command topics */
        {
            char topic[TOPIC_MAXLEN];
            make_topic(topic, sizeof(topic), "mode/set");
            esp_mqtt_client_subscribe(s_client, topic, 1);
            make_topic(topic, sizeof(topic), "display/set");
            esp_mqtt_client_subscribe(s_client, topic, 1);
            make_topic(topic, sizeof(topic), "brightness/set");
            esp_mqtt_client_subscribe(s_client, topic, 1);
            make_topic(topic, sizeof(topic), "theme/set");
            esp_mqtt_client_subscribe(s_client, topic, 1);
            make_topic(topic, sizeof(topic), "rotation/set");
            esp_mqtt_client_subscribe(s_client, topic, 1);
            make_topic(topic, sizeof(topic), "ticker_speed/set");
            esp_mqtt_client_subscribe(s_client, topic, 1);
            make_topic(topic, sizeof(topic), "ticker_sound/set");
            esp_mqtt_client_subscribe(s_client, topic, 1);
            esp_mqtt_client_subscribe(s_client, s_topic_ticker_set, 1);
        }

        /* HA auto-discovery: ticker entity — added after both SHT30 branches */
        if (s_discovery) {
            publish_ticker_discovery();
        }

        /* Publish current state immediately after (re-)connect */
        {
            config_lock();
            const nextube_config_t *cfg = config_get();
            app_mode_t  cur_mode     = cfg->current_mode;
            bool        cur_on       = cfg->backlight_on;
            uint8_t     cur_br       = cfg->lcd_brightness;
            bool        cur_rot      = cfg->rotation_enabled;
            bool        cur_tsnd     = cfg->ticker_sound;
            char        cur_theme[32];
            strncpy(cur_theme, cfg->theme, sizeof(cur_theme) - 1);
            cur_theme[sizeof(cur_theme) - 1] = '\0';
            config_unlock();

            publish_mode(cur_mode);
            publish_display(cur_on);
            publish_brightness(cur_br);
            publish_theme(cur_theme);
            publish_rotation(cur_rot);
            publish_ticker_speed(display_get_ticker_speed());
            publish_ticker_sound(cur_tsnd);
            if (sht30_is_present()) publish_sensors();

            /* Firmware version — retained so HA has it after broker restart */
            {
                char fw_topic[TOPIC_MAXLEN];
                make_topic(fw_topic, sizeof(fw_topic), "firmware/state");
                publish(fw_topic, FW_VERSION_STR, /*retain=*/1);
            }
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Disconnected from broker — will reconnect automatically");
        s_connected = false;
        break;

    case MQTT_EVENT_DATA: {
        /* Build null-terminated copies of topic and payload */
        char t[TOPIC_MAXLEN];
        int  tlen = event->topic_len < (int)(sizeof(t) - 1)
                  ? event->topic_len : (int)(sizeof(t) - 1);
        memcpy(t, event->topic, tlen);
        t[tlen] = '\0';

        char p[128];
        int  plen = event->data_len < (int)(sizeof(p) - 1)
                  ? event->data_len : (int)(sizeof(p) - 1);
        memcpy(p, event->data, plen);
        p[plen] = '\0';

        ESP_LOGI(TAG, "Received: %s = %s", t, p);

        /* ── mode/set ── */
        char mode_cmd_topic[TOPIC_MAXLEN];
        make_topic(mode_cmd_topic, sizeof(mode_cmd_topic), "mode/set");
        if (strcmp(t, mode_cmd_topic) == 0) {
            for (int i = 0; i < APP_MODE_MAX; i++) {
                if (strcmp(p, app_mode_name((app_mode_t)i)) == 0) {
                    config_set_mode((app_mode_t)i);
                    ESP_LOGI(TAG, "Mode set to %s via MQTT", p);
                    break;
                }
            }
            break;
        }

        /* ── display/set ── */
        char disp_cmd_topic[TOPIC_MAXLEN];
        make_topic(disp_cmd_topic, sizeof(disp_cmd_topic), "display/set");
        if (strcmp(t, disp_cmd_topic) == 0) {
            if (strcmp(p, "ON") == 0) {
                config_set_json("{\"backlight_onoff\":\"ON\"}", 24);
                ESP_LOGI(TAG, "Display ON via MQTT");
            } else if (strcmp(p, "OFF") == 0) {
                config_set_json("{\"backlight_onoff\":\"OFF\"}", 25);
                ESP_LOGI(TAG, "Display OFF via MQTT");
            }
            break;
        }

        /* ── brightness/set ── */
        char br_cmd_topic[TOPIC_MAXLEN];
        make_topic(br_cmd_topic, sizeof(br_cmd_topic), "brightness/set");
        if (strcmp(t, br_cmd_topic) == 0) {
            int val = atoi(p);
            if (val < 0)   val = 0;
            if (val > 100) val = 100;
            char json[48];
            snprintf(json, sizeof(json), "{\"lcd_brightness\":%d}", val);
            config_set_json(json, strlen(json));
            ESP_LOGI(TAG, "Brightness set to %d via MQTT", val);
            break;
        }

        /* ── theme/set ── */
        char theme_cmd_topic[TOPIC_MAXLEN];
        make_topic(theme_cmd_topic, sizeof(theme_cmd_topic), "theme/set");
        if (strcmp(t, theme_cmd_topic) == 0) {
            config_set_theme(p);
            publish_theme(p);
            ESP_LOGI(TAG, "Theme set to %s via MQTT", p);
            break;
        }

        /* ── rotation/set ── */
        char rot_cmd_topic[TOPIC_MAXLEN];
        make_topic(rot_cmd_topic, sizeof(rot_cmd_topic), "rotation/set");
        if (strcmp(t, rot_cmd_topic) == 0) {
            bool enable = (strcmp(p, "ON") == 0);
            char json[48];
            snprintf(json, sizeof(json), "{\"rotation_enabled\":%s}",
                     enable ? "true" : "false");
            config_set_json(json, strlen(json));
            publish_rotation(enable);
            ESP_LOGI(TAG, "Mode rotation %s via MQTT", enable ? "ON" : "OFF");
            break;
        }

        /* ── ticker_speed/set ── */
        char tspeed_cmd_topic[TOPIC_MAXLEN];
        make_topic(tspeed_cmd_topic, sizeof(tspeed_cmd_topic), "ticker_speed/set");
        if (strcmp(t, tspeed_cmd_topic) == 0) {
            display_set_ticker_speed(atoi(p));        /* clamps to 1–20 internally */
            publish_ticker_speed(display_get_ticker_speed());  /* echo clamped value */
            ESP_LOGI(TAG, "Ticker speed set to %d via MQTT", display_get_ticker_speed());
            break;
        }

        /* ── ticker_sound/set ── */
        char tsnd_cmd_topic[TOPIC_MAXLEN];
        make_topic(tsnd_cmd_topic, sizeof(tsnd_cmd_topic), "ticker_sound/set");
        if (strcmp(t, tsnd_cmd_topic) == 0) {
            bool on = (strcmp(p, "ON") == 0);
            if (on || strcmp(p, "OFF") == 0) {
                const char *json = on ? "{\"ticker_sound\":true}"
                                      : "{\"ticker_sound\":false}";
                config_set_json(json, strlen(json));
                publish_ticker_sound(on);
                ESP_LOGI(TAG, "Ticker sound %s via MQTT", on ? "ON" : "OFF");
            }
            break;
        }

        /* ── ticker/set ── */
        if (strcmp(t, s_topic_ticker_set) == 0) {
            /* Use event->data directly — up to TICKER_MAX_LEN chars, bypassing
             * the 128-byte p[] buffer used for other (shorter) payloads. */
            int ticker_n = event->data_len < TICKER_MAX_LEN
                         ? event->data_len : TICKER_MAX_LEN;
            if (ticker_n == 0) {
                ha_mqtt_ticker_clear();   /* empty payload = cancel ticker */
                ESP_LOGI(TAG, "Ticker cancelled via MQTT");
            } else {
                xSemaphoreTake(s_ticker_mutex, portMAX_DELAY);
                memcpy(s_ticker_text, event->data, ticker_n);
                s_ticker_text[ticker_n] = '\0';
                xSemaphoreGive(s_ticker_mutex);
                publish(s_topic_ticker_state, s_ticker_text, 0);
                ESP_LOGI(TAG, "Ticker set: \"%.*s\"", ticker_n, event->data);

                /* Optional notification chime.  audio_play_file() returns
                 * immediately (playback runs in its own task) and is a no-op
                 * when audio output is disabled in settings. */
                bool snd_en;
                char snd_file[64];
                config_lock();
                snd_en = config_get()->ticker_sound;
                strncpy(snd_file, config_get()->ticker_file, sizeof(snd_file) - 1);
                snd_file[sizeof(snd_file) - 1] = '\0';
                config_unlock();
                if (snd_en && snd_file[0] != '\0')
                    audio_play_file(snd_file);
            }
            break;
        }

        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT error");
        break;

    default:
        break;
    }
}

/* ── Main task ─────────────────────────────────────────────────────── */
static void ha_mqtt_task(void *arg)
{
    /* Wait for WiFi station to connect */
    while (!wifi_manager_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    /* Gate MQTT client allocation until no HTTPS connection is in progress.
     *
     * Weather and social-counter tasks call tls_sem_take() before every
     * mbedTLS handshake and tls_sem_give() after cleanup.  The esp_mqtt
     * client_init() + start() calls allocate internal buffers from the heap;
     * doing this mid-handshake can fragment internal RAM enough that the
     * mbedTLS BigNum scratch allocations fail, causing RSA signature
     * verification to report "PK verify failed" instead of an explicit OOM.
     *
     * Taking + immediately releasing the semaphore here ensures we wait for
     * any in-flight TLS session to finish before we commit MQTT's heap
     * footprint.  Once init/start are done the client holds a plain TCP
     * socket (no TLS context), so subsequent weather fetches are unaffected. */
    tls_sem_take();
    tls_sem_give();

    /* Build broker URI */
    char uri[96];
    snprintf(uri, sizeof(uri), "mqtt://%s:%u", s_broker, (unsigned)s_port);
    ESP_LOGI(TAG, "Connecting to %s", uri);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .session.keepalive  = 30,
        .network.reconnect_timeout_ms = 5000,
        /* Default stack (6144) is too small once discovery payloads are generated.
         * publish_discovery() alone allocates ~1.7 KB of locals (payload[768] +
         * dev[192] + 8 topic strings) on top of ~1-2 KB of MQTT library frames.
         * 10240 gives comfortable headroom for future additions. */
        .task.stack_size = 10240,
    };

    /* Set credentials only if username is provided */
    if (s_user[0] != '\0') {
        mqtt_cfg.credentials.username = s_user;
        mqtt_cfg.credentials.authentication.password = s_pass;
    }

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to create MQTT client — task exiting");
        vTaskDelete(NULL);
        return;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);

    /* ── Publish loop (60 s tick) ── */
    app_mode_t last_mode       = (app_mode_t)-1;
    bool       last_on         = true;
    uint8_t    last_brightness = 255;   /* sentinel — forces publish on first tick */
    bool       last_rotation   = false;
    char       last_theme[32]  = {0};   /* empty = sentinel, forces publish on first tick */

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));

        if (!s_connected) continue;

        /* Read current state */
        config_lock();
        const nextube_config_t *cfg = config_get();
        app_mode_t  cur_mode   = cfg->current_mode;
        bool        cur_on     = cfg->backlight_on;
        uint8_t     cur_br     = cfg->lcd_brightness;
        bool        cur_rot    = cfg->rotation_enabled;
        char        cur_theme[32];
        strncpy(cur_theme, cfg->theme, sizeof(cur_theme) - 1);
        cur_theme[sizeof(cur_theme) - 1] = '\0';
        config_unlock();

        /* Sensor readings */
        if (sht30_is_present()) {
            publish_sensors();
        }

        /* Optional health telemetry (RSSI / heap / uptime) + deferred NTP
         * sync stats (stashed by the SNTP-context listener — see
         * on_ntp_sync_stats for why it cannot publish directly). */
        config_lock();
        bool pub_health = config_get()->mqtt_pub_health;
        bool pub_ntp    = config_get()->mqtt_pub_ntp;
        config_unlock();
        if (pub_health) publish_health();
        if (s_ntp_pend) {
            if (pub_ntp) publish_ntp_stats();
            s_ntp_pend = false;
        }

        /* Mode — publish when changed */
        if (cur_mode != last_mode) {
            publish_mode(cur_mode);
            last_mode = cur_mode;
        }

        /* Display state — publish when changed */
        if (cur_on != last_on) {
            publish_display(cur_on);
            last_on = cur_on;
        }

        /* Brightness — publish when changed */
        if (cur_br != last_brightness) {
            publish_brightness(cur_br);
            last_brightness = cur_br;
        }

        /* Theme — publish when changed */
        if (strcmp(cur_theme, last_theme) != 0) {
            publish_theme(cur_theme);
            strncpy(last_theme, cur_theme, sizeof(last_theme) - 1);
        }

        /* Rotation — publish when changed */
        if (cur_rot != last_rotation) {
            publish_rotation(cur_rot);
            last_rotation = cur_rot;
        }
    }
}

/* ── Public entry point ────────────────────────────────────────────── */
void ha_mqtt_start(void)
{
    /* Snapshot the config fields we need at task creation time.
     * The task references these static copies for the rest of its lifetime. */
    config_lock();
    const nextube_config_t *cfg = config_get();
    strncpy(s_hostname, cfg->hostname,    sizeof(s_hostname) - 1);
    strncpy(s_broker,   cfg->mqtt_broker, sizeof(s_broker)   - 1);
    s_port      = cfg->mqtt_port;
    strncpy(s_user, cfg->mqtt_user,     sizeof(s_user) - 1);
    strncpy(s_pass, cfg->mqtt_password, sizeof(s_pass) - 1);
    s_discovery = cfg->mqtt_ha_discovery;
    config_unlock();

    /* Precompute ticker topic strings (hostname is now set above) */
    make_topic(s_topic_ticker_set,   sizeof(s_topic_ticker_set),   "ticker/set");
    make_topic(s_topic_ticker_state, sizeof(s_topic_ticker_state), "ticker/state");

    /* Create ticker mutex — must be done before the MQTT task starts so any
     * early ha_mqtt_ticker_* calls from other tasks are safe. */
    if (!s_ticker_mutex) s_ticker_mutex = xSemaphoreCreateMutex();

    /* Publish clock-discipline telemetry to HA after each NTP sync.  The
     * handler no-ops until the broker connection is up. */
    ntp_register_sync_listener(on_ntp_sync_stats);

    if (xTaskCreatePinnedToCore(ha_mqtt_task, "ha_mqtt",
                               4096, NULL, 3, NULL, 0) != pdPASS)
        ESP_LOGE(TAG, "ha_mqtt_task creation failed");
    else
        ESP_LOGI(TAG, "MQTT task started (broker: %s:%u)", s_broker, (unsigned)s_port);
}
