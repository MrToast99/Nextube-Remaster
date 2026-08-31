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
 *   nextube/<host>/update/state              "ON" or "OFF" — newer firmware release
 *                                            available (retained; from update_check task)
 *   nextube/<host>/update/latest_version/state "1.18.0" (retained)
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
 *   homeassistant/binary_sensor/<host>_update/config
 *   homeassistant/sensor/<host>_update_ver/config
 *
 * Firmware version:
 *   nextube/<host>/firmware/state              "1.10.0"  (retained)
 */

#include "ha_mqtt.h"
#include "config_mgr.h"
#include "fw_version.h"
#include "sht30.h"
#include "weather.h"     /* air-quality sensor (weather_get_aqi)        */
#include "display.h"
#include "update_check.h" /* autonomous GitHub release check → update binary sensor */
#include "audio.h"       /* ticker notification chime (audio_play_file) */
#include "ntp_time.h"    /* NTP sync-stats listener → HA sensors        */
#include "esp_wifi.h"    /* RSSI for the optional health sensors        */
#include "esp_system.h"  /* esp_get_free_heap_size                      */
#include "esp_timer.h"   /* uptime for the health sensors               */
#include "esp_heap_caps.h" /* heap_caps_get_largest_free_block - diagnostic on client-start failure */

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

/* Restrict a token to [A-Za-z0-9_-] in place, replacing everything else
 * with '_'. s_hostname is interpolated unescaped into both MQTT topic
 * strings and hand-built JSON discovery payloads — a hostname containing
 * '"' or '\' would corrupt the JSON, so it must never carry those chars. */
static void sanitize_mqtt_token(char *s)
{
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            *s = '_';
    }
}

/* ── Publish helpers ───────────────────────────────────────────────── */
static void publish(const char *topic, const char *payload, int retain)
{
    if (!s_connected || !s_client) return;
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, retain);
}

/* ── Discovery payloads ────────────────────────────────────────────── */
/* include_sensors: publish the SHT30-backed temperature/humidity sensors and
 * the outdoor AQI sensor.  Callers pass false when the SHT30 is not present
 * so HA doesn't get discovery configs for entities that will never publish
 * a state.  Every other entity (mode/display/brightness/theme/rotation/
 * firmware/update) is unconditional — those don't depend on SHT30 presence. */
static void publish_discovery(bool include_sensors)
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

    if (include_sensors) {
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
    }

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
               "\"Clock\","
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
               "\"DotMatrix\",\"Formula1\","
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

    /* ── Update available binary sensor ── */
    char upd_state[TOPIC_MAXLEN];
    make_topic(upd_state, sizeof(upd_state), "update/state");
    snprintf(topic, sizeof(topic),
             "homeassistant/binary_sensor/%s_update/config", s_hostname);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"Nextube Update Available\","
             "\"unique_id\":\"%s_update\","
             "\"state_topic\":\"%s\","
             "\"payload_on\":\"ON\","
             "\"payload_off\":\"OFF\","
             "\"device_class\":\"update\","
             "\"entity_category\":\"diagnostic\","
             "\"device\":{%s}"
             "}",
             s_hostname, upd_state, dev);
    publish(topic, payload, 1);

    /* ── Latest available version sensor (diagnostic) ── */
    char upd_ver_state[TOPIC_MAXLEN];
    make_topic(upd_ver_state, sizeof(upd_ver_state), "update/latest_version/state");
    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s_update_ver/config", s_hostname);
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"Nextube Latest Version\","
             "\"unique_id\":\"%s_update_ver\","
             "\"state_topic\":\"%s\","
             "\"entity_category\":\"diagnostic\","
             "\"icon\":\"mdi:cloud-download\","
             "\"device\":{%s}"
             "}",
             s_hostname, upd_ver_state, dev);
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

/* Retained — unlike the frequently-changing fields above, "an update is
 * pending" is a slow-changing status the user wants visible immediately on
 * HA reconnect, same treatment as firmware/state. */
static void publish_update_available(bool avail)
{
    char topic[TOPIC_MAXLEN];
    make_topic(topic, sizeof(topic), "update/state");
    publish(topic, avail ? "ON" : "OFF", 1);
}

static void publish_update_version(const char *ver)
{
    char topic[TOPIC_MAXLEN];
    make_topic(topic, sizeof(topic), "update/latest_version/state");
    publish(topic, ver, 1);
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

    sht30_reading_t s;
    if (!sht30_get(&s)) return;

    make_topic(topic, sizeof(topic), "sensor/temperature/state");
    snprintf(payload, sizeof(payload), "{\"temperature\":%.1f}", (double)s.temp_c);
    publish(topic, payload, 0);

    make_topic(topic, sizeof(topic), "sensor/humidity/state");
    snprintf(payload, sizeof(payload), "{\"humidity\":%.1f}", (double)s.humidity);
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
            publish_discovery(sht30_is_present());
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
        /* esp-mqtt splits a publish across multiple MQTT_EVENT_DATA callbacks
         * whenever the payload is larger than its internal buffer.  Only the
         * FIRST fragment of such a message carries event->topic —
         * continuation fragments have topic_len == 0.  Without tracking the
         * topic across fragments, every strcmp() below silently fails on a
         * continuation fragment and the message is dropped with no error.
         *
         * s_frag_topic persists the topic for the duration of one logical
         * message (current_data_offset == 0 through the fragment that
         * reaches total_data_len), then is cleared so any later event that
         * unexpectedly carries no topic doesn't silently reuse a stale one. */
        static char s_frag_topic[TOPIC_MAXLEN] = "";

        if (event->current_data_offset == 0 && event->topic_len > 0) {
            int save_len = event->topic_len < (int)(sizeof(s_frag_topic) - 1)
                         ? event->topic_len : (int)(sizeof(s_frag_topic) - 1);
            memcpy(s_frag_topic, event->topic, save_len);
            s_frag_topic[save_len] = '\0';
        }

        /* Build null-terminated copies of topic and payload */
        char t[TOPIC_MAXLEN];
        if (event->topic_len > 0) {
            int tlen = event->topic_len < (int)(sizeof(t) - 1)
                      ? event->topic_len : (int)(sizeof(t) - 1);
            memcpy(t, event->topic, tlen);
            t[tlen] = '\0';
        } else {
            /* Continuation fragment — use the topic saved from fragment 0. */
            strncpy(t, s_frag_topic, sizeof(t) - 1);
            t[sizeof(t) - 1] = '\0';
        }

        if (event->current_data_offset + event->data_len >= event->total_data_len) {
            s_frag_topic[0] = '\0';   /* message complete — don't leak topic to next message */
        }

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
    /* Deliberately does NOT wait for WiFi to connect before creating
     * esp-mqtt's client (and its internal ~10 KB task) below. Every other
     * task in this firmware gets its stack carved out during app_main()'s
     * early xTaskCreate batch, while the internal heap is still relatively
     * unfragmented; this one used to be the sole exception, gated behind
     * wifi_manager_is_connected() and so deferred until well after WiFi
     * association, mDNS probing, and the first weather/update_check/
     * subscribers TLS handshakes had already churned the heap - which is
     * the likely reason "Error create mqtt task" kept recurring regardless
     * of a startup delay (see the task.stack_size comment below; a fixed
     * post-connect delay was tried and field-tested, and did not help).
     *
     * esp_mqtt_client_start()'s return value only reflects whether the
     * task itself was created - not whether the broker is reachable yet.
     * If WiFi isn't up when this runs, the transport connect attempt fails
     * asynchronously and is handled by the same "will reconnect
     * automatically" path already used for any later disconnect - no
     * different from starting MQTT on a laptop before Ethernet is plugged
     * in. Task creation itself doesn't need the network, only a place to
     * put the stack. */

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
         * 10240 gives comfortable headroom for future additions.
         *
         * DO NOT shrink this without measuring real peak usage under a run
         * with every feature active first. 8192 was tried (based on a single
         * earlier ~5.7 KB peak reading that didn't reflect this task's full
         * discovery payload under heavier entity counts) and produced a
         * silent stack overflow once the task actually ran — not a clean
         * failure, but heap corruption that showed up minutes later as
         * unrelated allocation failures across mDNS, esp-tls, and HTTP client
         * (confirmed by reverting only this value and watching the entire
         * cascade disappear, leaving just the task-creation retry below).
         *
         * The *creation-time* failure ("Error create mqtt task") is a
         * separate, real, and still-open issue: xTaskCreate() needs a single
         * contiguous internal-RAM block of this size.
         *
         * NOT a transient startup-burst thing: a 5 s post-connect delay was
         * tried and did not help - failures still occur many minutes into
         * uptime. largest_internal has read exactly 9216 B in every field
         * reading taken this whole investigation, across wildly different
         * uptimes and total-free values, including one where an unrelated
         * task's stack was cut by 4096 B (freeing that many bytes of total
         * internal RAM moved this number by exactly zero). That means this
         * is a fixed structural boundary between two permanently-resident
         * allocations, not a "not enough total free RAM" or "hasn't settled
         * yet" problem - freeing bytes elsewhere or waiting longer won't
         * touch it. heap_caps_print_heap_info(MALLOC_CAP_INTERNAL) on the
         * failure path below dumps the actual region/block layout so the
         * next failure shows what's boxing in that 9216 B gap, instead of
         * just the one number. Don't guess another stack-size or timing fix
         * without that. */
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

    esp_err_t start_err = ESP_FAIL;
    for (int attempt = 1; attempt <= 5; attempt++) {
        start_err = esp_mqtt_client_start(s_client);
        if (start_err == ESP_OK) break;
        /* largest_internal: the actual contiguous-block ceiling right now -
         * the number that decides whether task.stack_size above will fit,
         * not the total free figure in the periodic heap log.
         * MALLOC_CAP_8BIT matters here: MALLOC_CAP_INTERNAL alone also
         * counts reclaimed IRAM, which isn't byte-addressable and can never
         * actually hold a task stack - a field reading showed a stable
         * "9216 B" that turned out to be exactly that: an IRAM region
         * heap_caps_print_heap_info below confirmed was nearly empty and
         * irrelevant, while the real (fragmentable, byte-addressable)
         * ceiling sat lower, in a heavily-used D/IRAM region. Querying
         * without MALLOC_CAP_8BIT would report the unusable number again. */
        ESP_LOGE(TAG, "esp_mqtt_client_start failed (%s), attempt %d/5 (largest_internal=%u B)",
                 esp_err_to_name(start_err), attempt,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    if (start_err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT client failed to start — MQTT disabled for this boot");
        /* All 5 retries exhausted — see the stack_size comment above for the
         * 9216 B investigation this is part of. Dump the actual region/block
         * breakdown rather than guessing another stack-size or timing fix. */
        heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    }

    /* ── Publish loop (60 s tick) ── */
    app_mode_t last_mode       = (app_mode_t)-1;
    bool       last_on         = true;
    uint8_t    last_brightness = 255;   /* sentinel — forces publish on first tick */
    bool       last_rotation   = false;
    char       last_theme[32]  = {0};   /* empty = sentinel, forces publish on first tick */
    int        last_update_avail = -1;  /* impossible bool value — forces publish on first tick,
                                            same trick as last_mode above (0/1 are both real values) */
    char       last_update_ver[16] = {0};

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

        /* Update available — publish when changed (fed by update_check task) */
        char cur_update_ver[16] = {0};
        bool cur_update_avail = update_check_get_status(cur_update_ver, sizeof(cur_update_ver));
        if (cur_update_avail != last_update_avail) {
            publish_update_available(cur_update_avail);
            last_update_avail = cur_update_avail;
        }
        /* Publish whenever we have a known latest tag at all — not gated on
         * cur_update_avail, so the diagnostic sensor reads a real value even
         * when already up to date (or ahead of the latest release), instead
         * of staying "unknown" until an update happens to be pending. */
        if (cur_update_ver[0] && strcmp(cur_update_ver, last_update_ver) != 0) {
            publish_update_version(cur_update_ver);
            strncpy(last_update_ver, cur_update_ver, sizeof(last_update_ver) - 1);
        }
    }
}

/* ── Public entry point ────────────────────────────────────────────── */
void ha_mqtt_start(void)
{
    /* No teardown path exists for s_client/the task below — calling this
     * twice would spawn a second ha_mqtt_task racing the first over the
     * shared s_client/s_connected globals. */
    static bool s_started = false;
    if (s_started) {
        ESP_LOGW(TAG, "ha_mqtt_start() called again — ignoring (already running)");
        return;
    }
    s_started = true;

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

    /* Hostname flows unescaped into JSON discovery payloads (see
     * publish_discovery) — keep it to a safe charset. */
    sanitize_mqtt_token(s_hostname);

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

/* Pause/resume the underlying esp-mqtt client — for callers (web_server.c's
 * OTA/webUI/stock-repair paths) that need MQTT fully quiet for a while, not
 * just our own wrapper task.  vTaskSuspend()-ing "ha_mqtt" (the task these
 * two don't touch) only freezes THIS task's 60 s publish loop and connect-
 * retry logic; esp-mqtt's own client owns a separate internal task for the
 * actual TCP/reconnect/keepalive work once esp_mqtt_client_start() has
 * succeeded, and that keeps running regardless — observed in the field
 * reconnecting and publishing a full discovery burst several seconds into
 * an OTA/webUI pull's "suspended" window despite "ha_mqtt" already being on
 * the suspend list. esp_mqtt_client_stop()/_start() operate on the client
 * itself, so they reach that internal task too.
 *
 * s_connected is cleared synchronously in ha_mqtt_pause(), before
 * esp_mqtt_client_stop() returns — closes the tiny window where a caller on
 * another task (e.g. a touch-button press publish, or the 60 s loop if it
 * happens to be mid-tick right as this runs) could still see s_connected
 * true and attempt a publish against a client that's mid-stop. */
void ha_mqtt_pause(void)
{
    if (!s_client) return;   /* never started, or esp_mqtt_client_init() failed */
    s_connected = false;
    ESP_LOGI(TAG, "MQTT paused");
    esp_mqtt_client_stop(s_client);   /* blocks until the client's own task has stopped */
}

/* Resume a client previously paused with ha_mqtt_pause().  s_connected
 * flips back to true via the normal MQTT_EVENT_CONNECTED handler once the
 * reconnect actually completes — not set here, since esp_mqtt_client_start()
 * only kicks off the connection attempt asynchronously. No-op if the client
 * was never started (ha_mqtt_pause() would also have no-op'd in that case,
 * so there's nothing to resume). */
void ha_mqtt_resume(void)
{
    if (!s_client) return;
    esp_err_t e = esp_mqtt_client_start(s_client);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "MQTT resume: esp_mqtt_client_start failed (%s)", esp_err_to_name(e));
    else
        ESP_LOGI(TAG, "MQTT resumed");
}
