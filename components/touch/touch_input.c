/**
 * @file touch_input.c
 * @brief Capacitive touch input – uses IDF 5.x touch_sens driver.
 *
 * Migrated from the legacy driver/touch_pad.h API to driver/touch_sens.h
 * (available for the original ESP32 since IDF 5.5).
 *
 * Three channels:
 *   TOUCH_LEFT   – GPIO 4  (touch pad channel 0)
 *   TOUCH_MIDDLE – GPIO 2  (touch pad channel 2)
 *   TOUCH_RIGHT  – GPIO 15 (touch pad channel 3)
 *
 * Strategy: continuous scanning with software IIR filter.  A poll task
 * (CPU 0, 50 ms) compares SMOOTH data against a software-tracked baseline
 * and fires a task-notification to a handler task on rising-edge press
 * (smooth < baseline × 80 %).  The handler calls the user callback, keeping
 * the poll loop unblocked during flash writes.
 */

#include "touch_input.h"
#include "board_pins.h"
#include "esp_log.h"
#include "driver/touch_sens.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "touch";

/* Number of consecutive 50 ms poll samples that must all read "pressed"
 * before a touch event fires.  Eliminates single-sample noise spikes that
 * the IIR filter hasn't fully attenuated yet.
 *   3 × 50 ms = 150 ms minimum hold – shorter than any intentional tap,
 *   longer than typical EMI glitches on the capacitive pads. */
#define PRESS_DEBOUNCE  3

/* ── Touch pad channel numbers (match GPIO from board_pins.h) ───────── */
/*   PIN_TOUCH_LEFT=4→pad0, PIN_TOUCH_MIDDLE=2→pad2, PIN_TOUCH_RIGHT=15→pad3 */
static const int touch_channels[] = { 0, 2, 3 };  /* pad IDs, not GPIO numbers */

/* ── Driver state ───────────────────────────────────────────────────── */
static touch_sensor_handle_t  s_sens = NULL;
static touch_channel_handle_t s_chan[3] = { NULL, NULL, NULL };

static touch_callback_t user_cb = NULL;
static TaskHandle_t touch_task_handle = NULL;

/* ── Poll task ──────────────────────────────────────────────────────── */
/*
 * Detection uses a software IIR baseline (s_baseline[]) seeded at boot from
 * settled SMOOTH data and slowly tracked toward idle readings at runtime.
 * A press fires when smooth < baseline × 80 % (20 % drop required).
 * ESP32 V1 touch_sens exposes only RAW and SMOOTH — no hardware BENCHMARK.
 *
 * Running on CPU 0 so JPEG decoding on CPU 1 (display task) cannot starve it.
 *
 * The user callback (which calls config_set_json / save_to_flash) is
 * dispatched via task-notification to a separate handler task, keeping
 * the 50 ms poll loop unblocked during slow flash writes.
 */

static TaskHandle_t  s_handler_task = NULL;
static QueueHandle_t s_touch_queue  = NULL;  /* depth-8 queue: survives burst presses */

/* Software baseline: IIR-tracked idle level per channel.
 * ESP32 V1 touch_sens only exposes RAW and SMOOTH (no hardware BENCHMARK),
 * so we maintain our own slow-moving baseline in software.
 * Updated only when the pad is not currently pressed, so a held finger
 * does not corrupt the baseline.  IIR weight = 1/64 per 50 ms tick. */
static uint32_t s_baseline[3] = { 0 };

static void touch_handler_task(void *arg)
{
    touch_pad_id_t id;
    while (1) {
        if (xQueueReceive(s_touch_queue, &id, portMAX_DELAY) == pdTRUE) {
            if (user_cb) user_cb(id);
        }
    }
}

static void touch_poll_task(void *arg)
{
    bool was_pressed[3]    = { false, false, false };
    int  stuck_count[3]    = { 0, 0, 0 };   /* detect stuck-pressed           */
    int  debounce_count[3] = { 0, 0, 0 };   /* consecutive pressed samples     */
    bool armed[3]          = { true, true, true }; /* false after stuck recovery
                                                     * until pad fully releases  */

    while (1) {
        for (int i = 0; i < 3; i++) {
            uint32_t smooth[1] = { 0 };

            if (touch_channel_read_data(s_chan[i], TOUCH_CHAN_DATA_TYPE_SMOOTH,
                                        smooth) != ESP_OK) {
                continue;
            }

            /* Dynamic threshold: 20 % drop below software baseline */
            uint32_t threshold = s_baseline[i] * 80 / 100;
            bool raw = (smooth[0] < threshold);

            /* Slowly track baseline toward current smooth only when NOT pressed.
             * IIR: baseline = baseline - baseline/64 + smooth/64 */
            if (!raw && s_baseline[i] > 0) {
                s_baseline[i] = s_baseline[i] - (s_baseline[i] / 64) + (smooth[0] / 64);
            }

            /* Debounce: accumulate consecutive pressed samples; clear on release.
             * A single-sample noise spike resets to 0 on the next idle reading
             * so it can never reach PRESS_DEBOUNCE and fire a spurious event. */
            if (raw) {
                if (debounce_count[i] < PRESS_DEBOUNCE)
                    debounce_count[i]++;
            } else {
                debounce_count[i] = 0;
            }
            bool pressed = (debounce_count[i] >= PRESS_DEBOUNCE);

            /* Stuck-press recovery: force-release after 1 s (20 × 50 ms).
             *
             * After recovery, set armed=false to prevent the debounce counter
             * from immediately re-firing while the finger is still down.
             * armed is restored to true only when the pad fully releases
             * (raw=false), guaranteeing exactly one event per physical press
             * regardless of how long the pad is held. */
            if (raw) {
                stuck_count[i]++;
                if (stuck_count[i] > 20) {
                    was_pressed[i]    = false;
                    stuck_count[i]    = 0;
                    debounce_count[i] = 0;
                    armed[i]          = false;  /* disarm until pad releases */
                    ESP_LOGW(TAG, "Touch%d stuck-press recovery — disarmed", i);
                }
            } else {
                stuck_count[i] = 0;
                if (!armed[i]) {
                    armed[i] = true;   /* pad released: re-arm for next press */
                    ESP_LOGI(TAG, "Touch%d re-armed after release", i);
                }
            }

            /* Fire on debounced rising edge — only when armed */
            if (pressed && !was_pressed[i] && armed[i]) {
                touch_pad_id_t id = (touch_pad_id_t)i;
                ESP_LOGI(TAG, "Touch%d pressed (smooth=%u baseline=%u)",
                         i, (unsigned)smooth[0], (unsigned)s_baseline[i]);
                if (s_touch_queue)
                    xQueueSend(s_touch_queue, &id, 0);  /* non-blocking; drop if full */
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
    touch_sensor_filter_config_t filter_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    ESP_ERROR_CHECK(touch_sensor_config_filter(s_sens, &filter_cfg));

    /* ── Enable and start continuous scanning ── */
    ESP_ERROR_CHECK(touch_sensor_enable(s_sens));
    ESP_ERROR_CHECK(touch_sensor_start_continuous_scanning(s_sens));

    /* Allow sensor readings and IIR filter to fully settle.
     * 500 ms was sometimes insufficient on cold boot — the IIR filter
     * needs ~20 × its time-constant to converge.  1 000 ms is reliable. */
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* Seed software baselines from settled smooth readings */
    for (int i = 0; i < 3; i++) {
        uint32_t val[1] = { 0 };
        touch_channel_read_data(s_chan[i], TOUCH_CHAN_DATA_TYPE_SMOOTH, val);
        s_baseline[i] = val[0];
        ESP_LOGI(TAG, "Touch%d (pad%d) baseline=%u threshold(80%%)=%u",
                 i, touch_channels[i], (unsigned)s_baseline[i],
                 (unsigned)(s_baseline[i] * 80 / 100));
    }

    /* Handler task: drains the touch queue and calls user_cb.
     * Keeps the 50 ms poll loop unblocked during slow flash writes.
     * Pinned to CPU 0 where other app tasks live. */
    s_touch_queue = xQueueCreate(8, sizeof(touch_pad_id_t));
    xTaskCreatePinnedToCore(touch_handler_task, "touch_hdl", 3072, NULL,
                            4, &s_handler_task, 0);

    /* Poll task on CPU 0 – away from the display task's JPEG decoding on CPU 1 */
    xTaskCreatePinnedToCore(touch_poll_task, "touch", 3072, NULL,
                            5, &touch_task_handle, 0);
}

void touch_input_register_callback(touch_callback_t cb) { user_cb = cb; }
