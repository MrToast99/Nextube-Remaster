/**
 * @file leds.c
 * @brief WS2812 LED driver – uses IDF 5.x RMT TX + bytes encoder API.
 *
 * Replaces the legacy driver/rmt.h API with driver/rmt_tx.h +
 * driver/rmt_encoder.h (new in IDF 5.0).
 *
 * Timing at 10 MHz resolution (100 ns / tick):
 *   bit-0: T0H = 4 ticks (400 ns), T0L = 9 ticks (900 ns)
 *   bit-1: T1H = 8 ticks (800 ns), T1L = 5 ticks (500 ns)
 * WS2812 bit order: MSB first, colour order: G R B per pixel.
 */

#include "leds.h"
#include "board_pins.h"
#include "config_mgr.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

static const char *TAG = "leds";

/* GRB pixel buffer (WS2812 colour order) */
static uint8_t led_data[LED_COUNT][3];  /* [G, R, B] */
static uint8_t brightness = 60;

/* ── RMT handles ────────────────────────────────────────────────────── */
#define RMT_LED_RESOLUTION_HZ  10000000   /* 10 MHz → 100 ns / tick */

static rmt_channel_handle_t s_rmt_chan  = NULL;
static rmt_encoder_handle_t s_bytes_enc = NULL;

/* ── Internal transmit ──────────────────────────────────────────────── */
static void ws2812_write(void)
{
    /* Build flat GRB byte array with brightness scaling */
    uint8_t grb[LED_COUNT * 3];
    for (int i = 0; i < LED_COUNT; i++) {
        grb[i * 3 + 0] = (led_data[i][0] * brightness) / 100;  /* G */
        grb[i * 3 + 1] = (led_data[i][1] * brightness) / 100;  /* R */
        grb[i * 3 + 2] = (led_data[i][2] * brightness) / 100;  /* B */
    }

    rmt_transmit_config_t tx_cfg = {
        .loop_count      = 0,          /* transmit once */
        .flags.eot_level = 0,          /* line held low after tx = WS2812 reset */
    };
    ESP_ERROR_CHECK(rmt_transmit(s_rmt_chan, s_bytes_enc,
                                 grb, sizeof(grb), &tx_cfg));
    /* Block until the frame is fully clocked out */
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(s_rmt_chan, pdMS_TO_TICKS(100)));
}

/* ════════════════════════════════════════════════════════════════════ */
/*  Public API                                                          */
/* ════════════════════════════════════════════════════════════════════ */

void leds_init(void)
{
    ESP_LOGI(TAG, "Initialising WS2812 LEDs on GPIO%d (RMT TX driver)",
             PIN_LED_DATA);

    /* ── Create RMT TX channel ── */
    rmt_tx_channel_config_t tx_chan_cfg = {
        .gpio_num          = PIN_LED_DATA,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = RMT_LED_RESOLUTION_HZ,
        .mem_block_symbols = 192,  /* LED_COUNT(6)×24 bits = 144 symbols; 3×64 blocks */
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_cfg, &s_rmt_chan));

    /* ── Create bytes encoder with WS2812 bit timing ── */
    rmt_bytes_encoder_config_t enc_cfg = {
        /* bit-0: 400 ns high, 900 ns low */
        .bit0 = {
            .level0 = 1, .duration0 = 4,
            .level1 = 0, .duration1 = 9,
        },
        /* bit-1: 800 ns high, 500 ns low */
        .bit1 = {
            .level0 = 1, .duration0 = 8,
            .level1 = 0, .duration1 = 5,
        },
        .flags.msb_first = 1,   /* WS2812 sends MSB first */
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc_cfg, &s_bytes_enc));

    /* ── Enable channel and blank the strip ── */
    ESP_ERROR_CHECK(rmt_enable(s_rmt_chan));

    memset(led_data, 0, sizeof(led_data));
    ws2812_write();
}

void leds_set_color(int idx, uint8_t r, uint8_t g, uint8_t b)
{
    if (idx < 0 || idx >= LED_COUNT) return;
    led_data[idx][0] = g;
    led_data[idx][1] = r;
    led_data[idx][2] = b;
}

void leds_set_all(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < LED_COUNT; i++)
        leds_set_color(i, r, g, b);
}

void leds_set_brightness(uint8_t pct)
{
    brightness = pct > 100 ? 100 : pct;
}

void leds_update(void) { ws2812_write(); }

void leds_off(void)
{
    memset(led_data, 0, sizeof(led_data));
    ws2812_write();
}

void leds_effect_breath(void)
{
    static float phase = 0;
    phase += 0.05f;
    float val = (sinf(phase) + 1.0f) / 2.0f;
    uint8_t v = (uint8_t)(val * 200);
    leds_set_all(v / 3, v / 2, v);
    leds_update();
}

void leds_effect_rainbow(void)
{
    static int hue_offset = 0;
    hue_offset = (hue_offset + 4) % 360;
    for (int i = 0; i < LED_COUNT; i++) {
        int hue = (hue_offset + i * 60) % 360;
        /* Simple HSV→RGB at S=1, V=200 */
        int sector = hue / 60;
        int f = (hue % 60) * 200 / 60;
        uint8_t r, g, b;
        switch (sector) {
            case 0:  r = 200; g = f;       b = 0;       break;
            case 1:  r = 200 - f; g = 200; b = 0;       break;
            case 2:  r = 0;   g = 200;     b = f;       break;
            case 3:  r = 0;   g = 200 - f; b = 200;     break;
            case 4:  r = f;   g = 0;       b = 200;     break;
            default: r = 200; g = 0;       b = 200 - f; break;
        }
        leds_set_color(i, r, g, b);
    }
    leds_update();
}

/* ── Effect task ────────────────────────────────────────────────────── */
static void led_task(void *arg)
{
    while (1) {
        const nextube_config_t *cfg = config_get();
        /* led_brightness is 0=off, 100=full bright — use directly. */
        leds_set_brightness(cfg->led_brightness);

        switch (cfg->backlight_mode) {
        case BL_MODE_STATIC:
            for (int i = 0; i < LED_COUNT; i++) {
                leds_set_color(i,
                    cfg->backlight_rgb[i][0],
                    cfg->backlight_rgb[i][1],
                    cfg->backlight_rgb[i][2]);
            }
            leds_update();
            vTaskDelay(pdMS_TO_TICKS(100));   /* static: update slowly */
            break;
        case BL_MODE_BREATH:
            leds_effect_breath();
            vTaskDelay(pdMS_TO_TICKS(50));
            break;
        case BL_MODE_RAINBOW:
            leds_effect_rainbow();
            vTaskDelay(pdMS_TO_TICKS(50));
            break;
        case BL_MODE_OFF:
        default:
            leds_off();
            vTaskDelay(pdMS_TO_TICKS(200));
            break;
        }
    }
}

void leds_task_start(void)
{
    xTaskCreatePinnedToCore(led_task, "leds", 2048, NULL, 4, NULL, 1);
    ESP_LOGI(TAG, "LED effect task started");
}
