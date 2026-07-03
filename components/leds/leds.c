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
#include "microphone.h"
#include "wled_sync.h"
#include "ntp_time.h"   /* ntp_is_night_window — shared LCD/LED night-brightness window */
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

static const char *TAG = "leds";

/* Night-brightness override for the accent LEDs — same shared auto_brightness
 * toggle and night window (night_start_hour/night_end_hour) the LCD backlight
 * uses (see display_task), just a separate target value since the LEDs and
 * the LCD backlight usually want different night dimming.  Must be called
 * with config_lock() held (reads multiple cfg fields via the shared pointer). */
static inline uint8_t night_adjusted_led_brightness(const nextube_config_t *cfg)
{
    if (cfg->auto_brightness &&
        ntp_is_night_window(cfg->night_start_hour, cfg->night_end_hour))
        return cfg->led_night_brightness;
    return cfg->led_brightness;
}

/* Set true by audio driver while a sound is playing.
 * The LED task skips ws2812_write() while this is set, stopping RMT
 * 10 MHz bursts that cause current spikes on the shared 3.3 V rail.
 * WS2812 LEDs latch their last colour and need no refresh to stay on. */
static volatile bool s_audio_active = false;
void leds_set_audio_active(bool active) { s_audio_active = active; }

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
    /* Use checked returns instead of ESP_ERROR_CHECK: a transient RMT error
     * (channel busy, 100 ms timeout) should log and return gracefully rather
     * than triggering an abort-and-reboot.  LEDs latch their last colour so
     * a skipped frame is invisible. */
    esp_err_t e = rmt_transmit(s_rmt_chan, s_bytes_enc, grb, sizeof(grb), &tx_cfg);
    if (e != ESP_OK) { ESP_LOGW(TAG, "LED transmit failed: %d", e); return; }
    e = rmt_tx_wait_all_done(s_rmt_chan, pdMS_TO_TICKS(100));
    if (e != ESP_OK) { ESP_LOGW(TAG, "LED wait failed: %d", e); }
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

/* Weather lightning override (see header).  s_leds_dirty forces the static
 * accent mode to repaint once the flash ends (it otherwise caches its frame). */
static volatile uint8_t s_weather_flash = 0;
static volatile bool    s_leds_dirty    = false;
void leds_weather_flash(uint8_t level)
{
    if (level == 0 && s_weather_flash > 0) s_leds_dirty = true;
    s_weather_flash = level;
}

void leds_off(void)
{
    memset(led_data, 0, sizeof(led_data));
    ws2812_write();
}

/* leds_effect_breath – legacy keyframe-less breath using a fixed warm-blue
 * palette.  Kept for external callers; the LED task uses the config-aware
 * version below so per-tube colours are respected. */
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
        /* ── Weather lightning override ─────────────────────────────────────
         * A thunderstorm flash pre-empts every accent mode with a yellow-white
         * pulse.  Skipped during audio playback (RMT paused for noise). */
        {
            uint8_t lv = s_weather_flash;
            if (lv > 0 && !s_audio_active) {
                leds_set_brightness(100);
                leds_set_all(lv, (uint8_t)(248 * lv / 255), (uint8_t)(205 * lv / 255));
                leds_update();
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
        }

        /* ── WLED Sync override ─────────────────────────────────────────────
         * When backlight_mode == BL_MODE_WLED and a UDP Notifier packet has
         * been received, skip local effects and mirror the WLED primary colour.
         * wled_sync_get() returns false until at least one packet arrives, so
         * the fallthrough below keeps local effects active on first boot.      */
        {
            backlight_mode_t bl;
            uint8_t          bl_brightness;
            config_lock();
            bl            = config_get()->backlight_mode;
            bl_brightness = night_adjusted_led_brightness(config_get());
            config_unlock();

            if (bl == BL_MODE_WLED) {
                wled_sync_state_t ws;
                if (wled_sync_get(&ws)) {
                    leds_set_brightness(bl_brightness);
                    if (!ws.on) {
                        leds_off();
                    } else if (ws.fx != 0 && ws.r == 0 && ws.g == 0 && ws.b == 0) {
                        /* Palette-based animation effects (Rainbow, Fire, Ocean,
                         * Color Cycle, etc.) do not use col[0] for rendering.
                         * WLED sends col[0]=(0,0,0) in the notifier packet when
                         * the user hasn't set an explicit primary colour for that
                         * effect, causing the accent LEDs to go dark.
                         * Mirror with our local rainbow so the strip stays visually
                         * active while WLED is animating. */
                        leds_effect_rainbow();
                    } else {
                        /* Apply the device master brightness explicitly: this path
                         * continues before the leds_set_brightness() call in the
                         * main loop body, so without this the global brightness
                         * would keep whatever stale value it last had.
                         * The WLED colour is already scaled by WLED's own brightness. */
                        leds_set_all(ws.r, ws.g, ws.b);
                        leds_update();
                    }
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                }
                /* No packet yet — fall through to local effects (shows 'Off') */
            }
        }

        /* Skip all RMT transmissions while audio is playing.
         * WS2812 hold their last colour — no visual glitch, no rail noise. */
        if (s_audio_active) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* Copy all config fields needed for this iteration under the lock so
         * no task switch between struct member reads can produce a torn read. */
        uint8_t led_brightness;
        uint8_t led_effect_speed;
        app_mode_t current_mode;
        uint8_t spectrum_rgb[3];
        uint8_t spectrum_led_source;
        backlight_mode_t backlight_mode;
        uint8_t backlight_rgb[LED_COUNT][3];

        config_lock();
        const nextube_config_t *cfg = config_get();
        led_brightness      = night_adjusted_led_brightness(cfg);
        led_effect_speed    = cfg->led_effect_speed;
        if (led_effect_speed < 1)  led_effect_speed = 1;
        if (led_effect_speed > 10) led_effect_speed = 10;
        current_mode        = cfg->current_mode;
        memcpy(spectrum_rgb,  cfg->spectrum_rgb,  sizeof(spectrum_rgb));
        spectrum_led_source = cfg->spectrum_led_source;
        backlight_mode      = cfg->backlight_mode;
        memcpy(backlight_rgb, cfg->backlight_rgb, sizeof(backlight_rgb));
        config_unlock();

        /* led_brightness is 0=off, 100=full bright — use directly. */
        leds_set_brightness(led_brightness);

        /* ── Spectrum mode: drive each LED at per-band audio brightness ──
         * spectrum_led_source == 0: amplitude-modulate the custom glow colour.
         * spectrum_led_source == 1: fall through to the accent mode switch so
         *   the LEDs animate in Static/Breath/Rainbow/Off as configured — the
         *   LCD still shows spectrum bars; only the LED source differs. */
        if (current_mode == APP_MODE_SPECTRUM && spectrum_led_source == 0) {
            float bands[MIC_BAND_COUNT];
            mic_get_bands(bands);
            /* Average the 4 frequency bands assigned to each tube/LED */
            const int bpl = MIC_BAND_COUNT / LED_COUNT;
            for (int i = 0; i < LED_COUNT; i++) {
                float v = 0.0f;
                for (int b = 0; b < bpl; b++) v += bands[i * bpl + b];
                v /= (float)bpl;
                leds_set_color(i,
                    (uint8_t)(spectrum_rgb[0] * v),
                    (uint8_t)(spectrum_rgb[1] * v),
                    (uint8_t)(spectrum_rgb[2] * v));
            }
            leds_update();
            vTaskDelay(pdMS_TO_TICKS(50));  /* 20 Hz refresh in spectrum mode */
            continue;
        }

        switch (backlight_mode) {
        case BL_MODE_STATIC: {
            /* In static mode, transmit once when colour/brightness changes
             * then stop.  WS2812 latch their last colour indefinitely —
             * no periodic refresh needed.  Eliminating continuous RMT
             * bursts reduces periodic noise spikes on the 3.3 V rail. */
            static uint8_t last_rgb[LED_COUNT][3];
            static uint8_t last_brt = 0xFF;
            bool changed = (led_brightness != last_brt) || s_leds_dirty;
            if (!changed) {
                for (int i = 0; i < LED_COUNT && !changed; i++)
                    changed = memcmp(last_rgb[i], backlight_rgb[i], 3) != 0;
            }
            if (changed) {
                for (int i = 0; i < LED_COUNT; i++) {
                    leds_set_color(i,
                        backlight_rgb[i][0],
                        backlight_rgb[i][1],
                        backlight_rgb[i][2]);
                    memcpy(last_rgb[i], backlight_rgb[i], 3);
                }
                last_brt = led_brightness;
                s_leds_dirty = false;   /* repaint after a weather-flash override */
                leds_update();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }
        case BL_MODE_BREATH: {
            /* Modulate each tube's configured colour with a sine-wave envelope.
             * The old leds_effect_breath() used a hardcoded blue palette;
             * this version respects the per-tube backlight_RGB settings.
             *
             * Update rate is fixed at 10 Hz (100 ms) to keep WS2812 RMT
             * bursts below 20 Hz and reduce audible coupling into the DAC.
             * Speed is controlled by scaling the phase increment:
             *   phase_step = led_effect_speed × 0.02
             *   speed=1 → 0.02 rad/tick → ~31 s cycle  (very slow)
             *   speed=5 → 0.10 rad/tick → ~6.3 s cycle (default)
             *   speed=10→ 0.20 rad/tick → ~3.1 s cycle (fast) */
            static float breath_phase = 0.0f;
            breath_phase += 0.02f * (float)led_effect_speed;
            float val = (sinf(breath_phase) + 1.0f) / 2.0f;  /* 0.0 – 1.0 */
            for (int i = 0; i < LED_COUNT; i++) {
                leds_set_color(i,
                    (uint8_t)(backlight_rgb[i][0] * val),
                    (uint8_t)(backlight_rgb[i][1] * val),
                    (uint8_t)(backlight_rgb[i][2] * val));
            }
            leds_update();
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }
        case BL_MODE_RAINBOW: {
            /* Full-spectrum hue rotation across all 6 LEDs.
             * Inlined here (instead of delegating to leds_effect_rainbow())
             * so led_effect_speed can control the hue step directly.
             *   hue_step = led_effect_speed  (1–10 degrees per 50 ms tick)
             *   speed=1  →  1°/50 ms → ~18 s/revolution  (very slow)
             *   speed=5  →  5°/50 ms →  3.6 s/revolution (default)
             *   speed=10 → 10°/50 ms →  1.8 s/revolution (fast)  */
            static int rainbow_hue = 0;
            rainbow_hue = (rainbow_hue + (int)led_effect_speed) % 360;
            for (int i = 0; i < LED_COUNT; i++) {
                int hue    = (rainbow_hue + i * 60) % 360;
                int sector = hue / 60;
                int f      = (hue % 60) * 200 / 60;
                uint8_t r, g, b;
                switch (sector) {
                    case 0:  r = 200; g = f;       b = 0;       break;
                    case 1:  r = 200-f; g = 200;   b = 0;       break;
                    case 2:  r = 0;   g = 200;     b = f;       break;
                    case 3:  r = 0;   g = 200-f;   b = 200;     break;
                    case 4:  r = f;   g = 0;       b = 200;     break;
                    default: r = 200; g = 0;       b = 200-f;   break;
                }
                leds_set_color(i, r, g, b);
            }
            leds_update();
            vTaskDelay(pdMS_TO_TICKS(50));
            break;
        }
        case BL_MODE_WLED:
            /* Sync task not started or no packet yet — hold LEDs off while
             * waiting for the first WLED broadcast after boot.              */
            leds_off();
            vTaskDelay(pdMS_TO_TICKS(200));
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
    /* 4096: the task calls ESP_LOGW (printf formatting ~1-1.5 KB of stack),
     * sinf, config_lock, mic_get_bands — 2048 left no headroom for a warning
     * firing at max call depth (stack-canary abort). */
    if (xTaskCreatePinnedToCore(led_task, "leds", 4096, NULL, 4, NULL, 1) != pdPASS)
        ESP_LOGE(TAG, "led_task creation failed");
    else
        ESP_LOGI(TAG, "LED effect task started");
}
