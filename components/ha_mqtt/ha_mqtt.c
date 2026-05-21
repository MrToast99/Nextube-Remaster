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
 *
 * Subscribed:
 *   nextube/<host>/mode/set                  "Weather"
 *   nextube/<host>/display/set               "ON" or "OFF"
 *   nextube/<host>/brightness/set            "75"
 *
 * HA auto-discovery:
 *   homeassistant/sensor/<host>_temp/config
 *   homeassistant/sensor/<host>_hum/config
 *   homeassistant/sensor/<host>_fw/config
 *   homeassistant/select/<host>_mode/config
 *   homeassistant/switch/<host>_display/config
 *   homeassistant/number/<host>_brightness/config
 *
 * Firmware version:
 *   nextube/<host>/firmware/state              "1.10.0"  (retained)
 */

#include "ha_mqtt.h"
#include "config_mgr.h"
#include "fw_version.h"
#include "sht30.h"
#include "wifi_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"

static const char *TAG = "ha_mqtt";

/* ── File-scope state ──────────────────────────────────────────────── */
static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool            s_connected = false;

/* Cached config values read at start() — broker may not be reachable
 * immediately; these are used throughout the task lifetime. */
static char   s_hostname[32];
static char   s_broker[64];
static uint16_t s_port;
static char   s_user[32];
static char   s_pass[64];
static bool   s_discovery;

/* ── Topic helpers ─────────────────────────────────────────────────── */
#define TOPIC_MAXLEN 96

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
               "\"Clock\",\"Countdown\",\"Scoreboard\",\"Pomodoro\","
               "\"YouTube\",\"Custom Clock\",\"Album\",\"Weather\","
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

static void publish_sensors(void)
{
    const sht30_reading_t *s = sht30_get();
    if (!s || !s->valid) return;

    char topic[TOPIC_MAXLEN];
    char payload[64];

    make_topic(topic, sizeof(topic), "sensor/temperature/state");
    snprintf(payload, sizeof(payload), "{\"temperature\":%.1f}", (double)s->temp_c);
    publish(topic, payload, 0);

    make_topic(topic, sizeof(topic), "sensor/humidity/state");
    snprintf(payload, sizeof(payload), "{\"humidity\":%.1f}", (double)s->humidity);
    publish(topic, payload, 0);
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
                           "\"Clock\",\"Countdown\",\"Scoreboard\",\"Pomodoro\","
                           "\"YouTube\",\"Custom Clock\",\"Album\",\"Weather\","
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
        }

        /* Publish current state immediately after (re-)connect */
        {
            config_lock();
            const nextube_config_t *cfg = config_get();
            app_mode_t  cur_mode = cfg->current_mode;
            bool        cur_on   = cfg->backlight_on;
            uint8_t     cur_br   = cfg->lcd_brightness;
            config_unlock();

            publish_mode(cur_mode);
            publish_display(cur_on);
            publish_brightness(cur_br);
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

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));

        if (!s_connected) continue;

        /* Read current state */
        config_lock();
        const nextube_config_t *cfg = config_get();
        app_mode_t  cur_mode = cfg->current_mode;
        bool        cur_on   = cfg->backlight_on;
        uint8_t     cur_br   = cfg->lcd_brightness;
        config_unlock();

        /* Sensor readings */
        if (sht30_is_present()) {
            publish_sensors();
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

    xTaskCreatePinnedToCore(ha_mqtt_task, "ha_mqtt",
                            4096, NULL, 3, NULL, 0);
    ESP_LOGI(TAG, "MQTT task started (broker: %s:%u)", s_broker, (unsigned)s_port);
}
