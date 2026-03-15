/**
 * @file touch_input.c
 * @brief Capacitive touch input – uses IDF 5.x touch_sens driver.
 *
 * Migrated from the legacy driver/touch_pad.h API to driver/touch_sens.h
 * (available for the original ESP32 since IDF 5.5).
 *
 * Three channels:
 *   TOUCH_LEFT   – GPIO 2  (touch pad channel 2)
 *   TOUCH_MIDDLE – GPIO 4  (touch pad channel 0)
 *   TOUCH_RIGHT  – GPIO 15 (touch pad channel 3)
 *
 * Strategy: continuous scanning is started at init time.  A polling task
 * reads the smooth data every 50 ms and fires the user callback on the
 * rising edge of a press.  Hardware thresholds are set to 0 (disabled)
 * so that no hardware interrupt fires; all detection is done in software
 * against a baseline calibrated at start-up (threshold = baseline × 2/3).
 */

#include "touch_input.h"
#include "board_pins.h"
#include "esp_log.h"
#include "driver/touch_sens.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

/* ── Touch pad channel numbers (match GPIO from board_pins.h) ───────── */
/*   PIN_TOUCH_LEFT=2→pad2, PIN_TOUCH_MIDDLE=4→pad0, PIN_TOUCH_RIGHT=15→pad3 */
static const int touch_channels[] = { 2, 0, 3 };  /* pad IDs, not GPIO numbers */

/* ── Driver state ───────────────────────────────────────────────────── */
static touch_sensor_handle_t  s_sens = NULL;
static touch_channel_handle_t s_chan[3] = { NULL, NULL, NULL };
static uint32_t               s_thresholds[3] = { 0 };

static touch_callback_t user_cb = NULL;
static TaskHandle_t touch_task_handle = NULL;

/* ── Poll task ──────────────────────────────────────────────────────── */
static void touch_poll_task(void *arg)
{
    bool was_pressed[3] = { false, false, false };

    while (1) {
        for (int i = 0; i < 3; i++) {
            uint32_t val[1] = { 0 };
            if (touch_channel_read_data(s_chan[i],
                                        TOUCH_CHAN_DATA_TYPE_SMOOTH,
                                        val) != ESP_OK) {
                continue;
            }
            bool pressed = (val[0] < s_thresholds[i]);
            if (pressed && !was_pressed[i] && user_cb) {
                user_cb((touch_pad_id_t)i);
            }
            was_pressed[i] = pressed;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ════════════════════════════════════════════════════════════════════ */
/*  Public API                                                          */
/* ════════════════════════════════════════════════════════════════════ */

void touch_input_init(void)
{
    ESP_LOGI(TAG, "Initialising touch pads: GPIO%d, GPIO%d, GPIO%d",
             PIN_TOUCH_LEFT, PIN_TOUCH_MIDDLE, PIN_TOUCH_RIGHT);

    /* ── Configure sensor controller ── */
    /* Voltage levels match original firmware:
     *   old: TOUCH_HVOLT_2V7 with TOUCH_HVOLT_ATTEN_1V → effective 1.7 V
     *   old: TOUCH_LVOLT_0V5 → 0.5 V
     * In the new API charge_volt_lim_h is absolute (no attenuation field). */
    touch_sensor_sample_config_t sample_cfg =
        TOUCH_SENSOR_V1_DEFAULT_SAMPLE_CONFIG(
            1.0f,
            TOUCH_VOLT_LIM_L_0V5,
            TOUCH_VOLT_LIM_H_1V7);

    touch_sensor_config_t sens_cfg =
        TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, &sample_cfg);

    ESP_ERROR_CHECK(touch_sensor_new_controller(&sens_cfg, &s_sens));

    /* ── Register channels ── */
    /* abs_active_thresh = 0: hardware interrupt never fires (polling only) */
    touch_channel_config_t chan_cfg = {
        .abs_active_thresh = { 0 },
        .charge_speed      = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt  = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
        .group             = TOUCH_CHAN_TRIG_GROUP_BOTH,
    };
    for (int i = 0; i < 3; i++) {
        ESP_ERROR_CHECK(touch_sensor_new_channel(s_sens, touch_channels[i],
                                                 &chan_cfg, &s_chan[i]));
    }

    /* ── Configure software (IIR) filter ── */
    /* Required before enable; SMOOTH data type won't work without it.
     * The default config uses a reasonable IIR weight for capacitive pads. */
    touch_filter_config_t filter_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    ESP_ERROR_CHECK(touch_sensor_config_filter(s_sens, &filter_cfg));

    /* ── Enable and start continuous scanning ── */
    ESP_ERROR_CHECK(touch_sensor_enable(s_sens));
    ESP_ERROR_CHECK(touch_sensor_start_continuous_scanning(s_sens));

    /* Allow sensor readings to settle */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* ── Calibrate thresholds at ~66 % of idle baseline ── */
    for (int i = 0; i < 3; i++) {
        uint32_t val[1] = { 0 };
        touch_channel_read_data(s_chan[i], TOUCH_CHAN_DATA_TYPE_SMOOTH, val);
        s_thresholds[i] = val[0] * 2 / 3;
        ESP_LOGI(TAG, "Touch%d (pad%d) baseline=%u threshold=%u",
                 i, touch_channels[i], (unsigned)val[0],
                 (unsigned)s_thresholds[i]);
    }

    xTaskCreatePinnedToCore(touch_poll_task, "touch", 2048, NULL,
                            5, &touch_task_handle, 1);
}

void touch_input_register_callback(touch_callback_t cb) { user_cb = cb; }
