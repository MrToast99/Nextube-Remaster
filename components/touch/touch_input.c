#include "touch_input.h"
#include "board_pins.h"
#include "esp_log.h"
#include "driver/touch_pad.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";
static touch_callback_t user_cb = NULL;
static TaskHandle_t touch_task_handle = NULL;

/* touch_gpios array removed to fix unused-const-variable compiler error */
static const touch_pad_t touch_channels[] = { TOUCH_PAD_NUM2, TOUCH_PAD_NUM0, TOUCH_PAD_NUM3 };
static uint16_t thresholds[3] = {0};

static void touch_poll_task(void *arg)
{
    uint16_t val;
    bool was_pressed[3] = {false};

    while (1) {
        for (int i = 0; i < 3; i++) {
            touch_pad_read_filtered(touch_channels[i], &val);
            bool pressed = (val < thresholds[i]);
            if (pressed && !was_pressed[i] && user_cb) {
                user_cb((touch_pad_id_t)i);
            }
            was_pressed[i] = pressed;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void touch_input_init(void)
{
    ESP_LOGI(TAG, "Initialising touch pads: GPIO%d, GPIO%d, GPIO%d",
             PIN_TOUCH_LEFT, PIN_TOUCH_MIDDLE, PIN_TOUCH_RIGHT);

    touch_pad_init();
    touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);

    for (int i = 0; i < 3; i++) {
        touch_pad_config(touch_channels[i], 0);
    }

    touch_pad_filter_start(10);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Calibrate thresholds at ~66% of idle value */
    for (int i = 0; i < 3; i++) {
        uint16_t val;
        touch_pad_read_filtered(touch_channels[i], &val);
        thresholds[i] = val * 2 / 3;
        ESP_LOGI(TAG, "Touch%d baseline=%u threshold=%u", i, val, thresholds[i]);
    }

    xTaskCreatePinnedToCore(touch_poll_task, "touch", 2048, NULL, 5, &touch_task_handle, 1);
}

void touch_input_register_callback(touch_callback_t cb) { user_cb = cb; }