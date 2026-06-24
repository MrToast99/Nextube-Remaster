#include "display.h"
#include "lodepng.h"
#include "board_pins.h"
#include "u8g2.h"
#include "font_render.h"
#include "esp_log.h"
#include "esp_timer.h"          /* esp_timer_get_time — AP PIN phase clock */
#include "esp_heap_caps.h"      /* PSRAM_MALLOC / heap_caps_malloc */
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "weather.h"
#include "leds.h"               /* leds_weather_flash() — WeatherLive lightning */
#include "sht30.h"              /* sht30_get() — indoor H/T for 24H_CX panel */
#include "wifi_manager.h"       /* AP PIN visibility (S1) */
#include "wled_sync.h"          /* Spectrum LCD bars can follow WLED primary */
#include "ha_mqtt.h"            /* ticker overlay (ha_mqtt_ticker_active / _clear) */
#include "jpeg_decoder.h"       /* espressif/esp_jpeg v1.x managed component */
#include "esp_random.h"         /* esp_fill_random — static-snow burn-in */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

static const char *TAG = "display";
static const int cs_pins[LCD_COUNT] = {
    PIN_LCD1_CS, PIN_LCD2_CS, PIN_LCD3_CS,
    PIN_LCD4_CS, PIN_LCD5_CS, PIN_LCD6_CS
};
static spi_device_handle_t spi_dev;

/* Display task handle — stored so display_apply_init_profiles() can suspend
 * the task for the duration of a per-tube SWRESET + reinit sequence, giving
 * the init code exclusive ownership of the SPI bus and CS lines.
 * NULL until display_task_start() is called; checked before use. */
static TaskHandle_t s_display_task_handle = NULL;

/* Cooperative park handshake for OTA (see display_show_wait): the display
 * task checks s_park_req at its loop boundary — never mid-render, never with
 * an SPI transaction in flight — sets s_parked, and suspends itself. */
static volatile bool s_park_req = false;
static volatile bool s_parked   = false;

/* Short-lived "busy" backoff (display_busy_hint): a wall-clock deadline, in
 * esp_timer microseconds, until which the heavy WeatherLive realtime animation
 * skips its frame so a CPU/flash-bound web operation (config save, etc.) on the
 * lower-priority httpd task isn't starved by the display task.  Auto-expires —
 * unlike the OTA park, it never suspends the task. */
static volatile int64_t s_busy_until_us = 0;

/* ── U8g2 virtual display for 24H-CX H/T panel text rendering ────────────────
 * We configure U8g2 for a 128×64 "nodisp" (no hardware) display.  The 128-px
 * width gives ample room for our 80-px tube; a 28-px font centred in 80 rows
 * produces a baseline at row ≈53, keeping all glyph pixels within the 64-row
 * buffer.  The 16 physical rows below (64–79 of each half) are left as the
 * black fill laid down by display_fill() at the start of every kind==2 render.
 * Buffer: 128 cols × 8 tile-rows = 1024 bytes, allocated internally by
 * u8g2_Setup_sh1106_128x64_noname_f (via u8g2_m_16_8_f).  Access via
 * u8g2_GetBufferPtr(&s_u8g2) after any draw call.
 * Callbacks are no-ops — all rendering stays in RAM. */
static u8g2_t s_u8g2;

static uint8_t ht_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
    { (void)u8x8; (void)msg; (void)arg_int; (void)arg_ptr; return 1; }
static uint8_t ht_gpio_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
    { (void)u8x8; (void)msg; (void)arg_int; (void)arg_ptr; return 1; }

/* Update indicator flag — set true to overlay 4 red rows at the physical
 * bottom of tube 5 on every frame.  Declared here (before display_show_digit)
 * so the function can read it.  Implementation: display_set_update_indicator(). */
static volatile bool s_update_indicator = false;

/* ── 24H Custom clock — tube 6 panel rotation state ─────────────────────────
 * Tracks which info panel is currently on tube 6 and when it started.
 * Both variables are only ever read/written from the display task, so no
 * additional mutex is required. */
static uint8_t  s_cx_panel        = 0;   /* tube 6: index into its enabled panel list */
static int64_t  s_cx_panel_start  = 0;   /* esp_timer_get_time() µs when panel began (shared) */
static struct tm s_cx_last_t;            /* last struct tm at which the panels were rendered */
static int8_t   s_cx_last_kind    = -1;  /* tube 6: panel kind last drawn; -1 = none yet */
/* Dual-panel mode: tube 5 (LCD index 4) rotates through its own enabled set,
 * with its own index and last-kind so each panel clears/animates independently. */
static uint8_t  s_cx_panel5       = 0;   /* tube 5: index into its enabled panel list */
static int8_t   s_cx_last_kind5   = -1;  /* tube 5: panel kind last drawn; -1 = none yet */

/* ── 24H_CX externally-pushed image panels (asset themes) ───────────────────
 * An external script POSTs a JPG (exactly 80×160) to /api/cx_image?tube=5|6.
 * It is decoded to RGB565 here (in the httpd task) into a per-tube PSRAM blob;
 * render_cx_panel() blits it when the tube's "Pushed image" panel is enabled.
 * Index 0 = tube 5 (LCD 4), 1 = tube 6 (LCD 5).  s_cx_push_mutex guards the
 * decode-write vs. the display-task blit; s_cx_push_seq bumps on each push so
 * the display task re-renders promptly. */
static uint8_t         *s_cx_push_buf[2]   = { NULL, NULL };
static volatile bool    s_cx_push_valid[2] = { false, false };
static volatile uint32_t s_cx_push_seq     = 0;
static SemaphoreHandle_t s_cx_push_mutex   = NULL;

/* ── Timer / burn-in mutex ───────────────────────────────────────────────────
 * Declared here (before the burn-in setter functions) so the setters can use
 * it at any point in the file.  Initialised in display_task() before the first
 * render tick.  Guards both the countdown/pomodoro timer fields and the paired
 * burn-in/snow mask+end_time writes so they are never observed in a torn state. */
static SemaphoreHandle_t s_timer_mutex = NULL;

/* ── Anti burn-in ────────────────────────────────────────────────────────── */
/* Hourly pixel shift: the CASET window is offset by s_burnin_shift_x pixels
 * so the same physical columns are not driven by static content indefinitely.
 * Value cycles through {-2,-1,0,+1,+2} as hour%5-2 (self-correcting from NTP).
 * Applied to every CASET command in display_fill, display_show_digit, and
 * render_spectrum — nowhere else needs it since all rendering goes through one
 * of those three entry points.
 *
 * Safety: LCD_OFFSET_X=24 so shift -2 → col 22 (still on-panel).
 *         LCD_WIDTH=80 so shift +2 → right edge col 105 (panel is ≥128 wide). */
static int8_t s_burnin_shift_x = 0;

/* Burn-in colour-cycle mask: each bit = one tube (bit 0 = tube 0 … bit 5 =
 * tube 5).  When a bit is set, that tube shows a cycling colour sequence
 * (red → green → blue → white → black, 30 s per step) to exercise all
 * sub-pixels at both voltage extremes.  Normal rendering runs for unmasked
 * tubes.  Set/cleared via display_set_burnin_mask().                          */
static volatile uint8_t  s_burnin_mask     = 0;
static volatile time_t   s_burnin_end_time = 0;  /* 0 = no timer; else epoch s */

/* RGB565 colour cycle — drives each sub-pixel high and low in turn. */
static const uint16_t s_burnin_colors[] = {
    0xF800,   /* red   — R max, G=0, B=0 */
    0x07E0,   /* green — R=0, G max, B=0 */
    0x001F,   /* blue  — R=0, G=0, B max */
    0xFFFF,   /* white — all max          */
    0x0000,   /* black — all min          */
};
#define BURNIN_COLOR_COUNT  ((int)(sizeof(s_burnin_colors)/sizeof(s_burnin_colors[0])))
#define BURNIN_COLOR_SECS   30   /* seconds per colour step in the cycle */

/* SPI pixel transfer chunk height — 8 rows × 80 px × 2 B = 1280 B on the
 * stack (SRAM).  Used by display_show_image() and display_show_image_region().
 * Defined at file scope so both functions can share the same constant. */
#define DISP_CHUNK_ROWS 8

/* ── MQTT Ticker overlay ─────────────────────────────────────────────────────
 * When the MQTT ticker is active the display task calls render_ticker() each
 * 200 ms tick and skips normal mode rendering.  The text scrolls from right to
 * left across all 6 tubes using the logisoso28 font at 4 px/tick (20 px/s).
 * render_ticker() clears the ticker and blanks all tubes once the text has
 * fully scrolled off the left edge of tube 0.
 *
 * U8G2_16BIT is defined globally (see components/u8g2/CMakeLists.txt) so
 * u8g2_uint_t = uint16_t and u8g2_int_t = int16_t.  Drawing at a negative x
 * (e.g. text already half-scrolled off left) works correctly: the cast
 * (u8g2_uint_t)negative_int wraps to the two's-complement uint16 value, and
 * U8g2's internal clip uses (u8g2_int_t) to restore the signed value, safely
 * discarding any glyph whose right edge is ≤ 0.                               */
#define TICKER_MAX_LEN     255
#define TICKER_SCROLL_PX     4   /* default px per 200 ms tick → 20 px/s */
#define TICKER_SCROLL_MIN    1   /* slowest (≈ 5 px/s)  */
#define TICKER_SCROLL_MAX   20   /* fastest (≈ 100 px/s) */
/* Runtime scroll speed in on-screen pixels per 200 ms tick.  Adjustable at
 * runtime via display_set_ticker_speed() (driven by the Home Assistant MQTT
 * "Ticker Speed" number entity).  RAM-only — resets to the default on reboot. */
static int s_ticker_scroll_px = TICKER_SCROLL_PX;
/* The ticker text is rendered into the 128×64 U8g2 buffer at native logisoso28
 * size, then blitted with 2× pixel scaling (both axes) so it appears double
 * size (~56 px) — the same technique as the big clock digits (pin_draw_tube).
 * x_start / text_px_w stay in on-screen pixels; only the per-tube draw offset
 * is halved into buffer space.  Output band = 64×2 = 128 rows, centred with a
 * 16-row top/bottom margin in the 160-row tube. */
#define TICKER_SCALE       2
#define TICKER_OUT_H       (64 * TICKER_SCALE)                 /* 128 */
#define TICKER_Y_MARGIN    ((LCD_HEIGHT - TICKER_OUT_H) / 2)   /* 16  */

static struct {
    char  text[TICKER_MAX_LEN + 1]; /* current message being scrolled */
    int   x_start;                  /* left edge of text in global 6×80 px canvas */
    int   text_px_w;                /* measured pixel width of text */
    bool  running;                  /* true while a scroll is in progress */
} s_ticker_state;

/* Static-snow burn-in: each frame writes truly random RGB565 pixels to every
 * tube in the mask, exercising each sub-pixel independently rather than as a
 * uniform colour block.  State mirrors the colour-cycle variables above. */
static volatile uint8_t s_snow_mask     = 0;
static volatile time_t  s_snow_end_time = 0;

/* ── Panel profiles (VCOM + gamma) ──────────────────────────────────────────
 * The ST7735 "Green Tab" (original Nextube panels) and ST7735S (replacement
 * panels such as LH096NT-IF09W) require different VCOM voltages and gamma
 * curves to achieve proper contrast and colour saturation.
 *
 *  Profile 0 "Standard" — VCOM 0x0E.  Tuned for the original Green Tab panels
 *     shipped with the Nextube.  Colours are accurate and saturated on those.
 *
 *  Profile 1 "Vivid"    — VCOM 0x3C.  For ST7735S replacement panels that
 *     look washed/low-contrast at Standard VCOM.  The higher VCOM raises the
 *     AC driving voltage, restoring contrast and colour depth.  Gamma is also
 *     recalibrated to match the ST7735S response curve.
 *
 * Only VCOM (0xC5) and the two gamma registers (0xE0 / 0xE1) differ between
 * profiles.  All power-control registers (0xC0-0xC4) are identical and are
 * not included here — they are sent once during st7735_init_one().
 *
 * Source: Adafruit ST7735R Red-Tab (VCOM 0x3C) vs Green-Tab (VCOM 0x0E);
 *   TFT_eSPI ST7735S_80x160 gamma table.                                      */
typedef struct {
    uint8_t vcom;       /* VMCTR1 (0xC5) — AC driving voltage for contrast     */
    uint8_t gmp[16];    /* GMCTRP1 (0xE0) — positive gamma correction          */
    uint8_t gmn[16];    /* GMCTRN1 (0xE1) — negative gamma correction          */
} panel_profile_t;

static const panel_profile_t s_panel_profiles[] = {
    /* 0: Standard — original Nextube Green-Tab panels */
    {
        .vcom = 0x0E,
        .gmp  = {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
                 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10},
        .gmn  = {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10},
    },
    /* 1: Vivid — ST7735S replacement panels (e.g. LH096NT-IF09W).
     * VCOM raised from 0x0E → 0x3C restores contrast lost to a mismatched
     * AC voltage.  Gamma recalibrated for the ST7735S transfer curve. */
    {
        .vcom = 0x3C,
        .gmp  = {0x0F, 0x1A, 0x0F, 0x18, 0x2F, 0x28, 0x20, 0x22,
                 0x1F, 0x1B, 0x23, 0x37, 0x00, 0x07, 0x02, 0x10},
        .gmn  = {0x0F, 0x1B, 0x0F, 0x17, 0x33, 0x2C, 0x29, 0x2E,
                 0x30, 0x30, 0x39, 0x3F, 0x00, 0x07, 0x03, 0x10},
    },
};
#define PANEL_PROFILE_COUNT  ((int)(sizeof(s_panel_profiles) / sizeof(s_panel_profiles[0])))

/* Active per-tube profile index.  Zero-init = Profile 0 (Standard) at boot. */
static uint8_t s_init_profiles[LCD_COUNT];

/* ── Per-tube panel fine-tuning ──────────────────────────────────────────── */
/* Window offset overrides — added to LCD_OFFSET_X/Y in every CASET/RASET.
 * Replacement panels based on ST7735S variants typically need col +2, row +1
 * to prevent 1px of uninitialized frame-buffer from appearing at the right/bottom
 * edge.  Updated by display_apply_tube_offsets(); safe to read from display task. */
static int8_t  s_col_offsets[LCD_COUNT];   /* default 0 — zero-init by linker */
static int8_t  s_row_offsets[LCD_COUNT];   /* default 0 — zero-init by linker */

/* Set by display_invalidate() (and internally by display_apply_tube_offsets) to
 * signal the display task to force-repaint every tube on its next tick.
 * Without this, a tube whose content hasn't changed (e.g. the hours digit in
 * no-seconds mode) will not pick up a new col/row offset until its digit finally
 * ticks over — leaving the right-edge static artifact visible for up to an hour. */
static volatile bool s_full_repaint_request = false;
/* Set by display_config_changed() after a settings save.  Forces the display
 * task to treat the next tick as a mode-change (re-render everything) without
 * blanking the tubes, so live config edits (shadow colour, etc.) appear within
 * one tick even if change-detection tracking is momentarily stale. */
static volatile bool s_settings_saved = false;

/* Per-tube software brightness scale (0-100).  Must be initialised to 100 in
 * display_init() because the linker zero-initialises static arrays, which would
 * render every pixel black.  Updated by display_apply_tube_brightness(). */
static uint8_t s_tube_brightness[LCD_COUNT];

/* Per-tube VMCTR1 VCOM value (0x00–0x3F).  Must be initialised to 0x0E in
 * display_init().  Decoupled from the panel profile so users can fine-tune
 * contrast for their specific replacement panel batch without changing the
 * gamma curve.  Updated by display_apply_tube_vcom(). */
static uint8_t s_tube_vcom[LCD_COUNT];

/* ── Software gamma correction ───────────────────────────────────────────────
 * Pre-computed lookup tables that map each possible R5/B5 (0–31) and G6 (0–63)
 * channel value through out = in^gamma before the pixel is sent to the display.
 *
 * Gamma > 1.0 darkens midtones (fixes washed / low-contrast panels whose native
 * response curve is flatter than expected).  Gamma = 1.0 is identity — the
 * s_gamma_lut_active flag short-circuits the table entirely so there is no
 * per-pixel overhead when gamma correction is off.
 *
 * The tables are rebuilt by rebuild_gamma_lut() whenever the gamma value
 * changes.  The display task is suspended during the rebuild to avoid reading
 * a partially-updated table.                                                   */
static float   s_gamma[LCD_COUNT];                  /* per-tube exponent; 1.0 = identity */
static bool    s_gamma_lut_active[LCD_COUNT];        /* false = LUT skipped (identity)    */
static uint8_t s_gamma_lut_5bit[LCD_COUNT][32];      /* R and B channels (5-bit, 0–31)    */
static uint8_t s_gamma_lut_6bit[LCD_COUNT][64];      /* G channel        (6-bit, 0–63)    */

static void rebuild_gamma_lut(int tube)
{
    float g = s_gamma[tube];
    if (fabsf(g - 1.0f) < 0.005f) {
        /* Identity — fill with pass-through values and disable the LUT path. */
        for (int i = 0; i < 32; i++) s_gamma_lut_5bit[tube][i] = (uint8_t)i;
        for (int i = 0; i < 64; i++) s_gamma_lut_6bit[tube][i] = (uint8_t)i;
        s_gamma_lut_active[tube] = false;
        return;
    }
    for (int i = 0; i < 32; i++) {
        float v = powf((float)i / 31.0f, g) * 31.0f + 0.5f;
        s_gamma_lut_5bit[tube][i] = (v >= 31.0f) ? 31u : (uint8_t)v;
    }
    for (int i = 0; i < 64; i++) {
        float v = powf((float)i / 63.0f, g) * 63.0f + 0.5f;
        s_gamma_lut_6bit[tube][i] = (v >= 63.0f) ? 63u : (uint8_t)v;
    }
    s_gamma_lut_active[tube] = true;
}

static void lcd_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_LCD_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_polling_transmit(spi_dev, &t);   /* 1 byte — polling cheaper than DMA setup */
}

static void lcd_data(const uint8_t *data, int len)
{
    if (len <= 0) return;
    gpio_set_level(PIN_LCD_DC, 1);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    spi_device_polling_transmit(spi_dev, &t);   /* ≤8 bytes — polling cheaper than DMA setup */
}

static void lcd_data_byte(uint8_t val) { lcd_data(&val, 1); }

static void select_tube(int i)
{
    for (int n = 0; n < LCD_COUNT; n++)
        gpio_set_level(cs_pins[n], (n == i) ? 0 : 1);
}

static void deselect_all(void)
{
    for (int i = 0; i < LCD_COUNT; i++) gpio_set_level(cs_pins[i], 1);
}

/* RST is shared across all 6 displays — call display_reset_all() ONCE
 * before looping over tubes.  This function only sends the init sequence
 * to the already-selected tube; it does NOT toggle RST. */
static void st7735_init_one(int tube)
{
    select_tube(tube);
    lcd_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150)); /* SWRESET */
    lcd_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120)); /* SLPOUT  */

    /* Power Control – common to all profiles */
    lcd_cmd(0xC0); uint8_t pc1[] = {0xA2, 0x02, 0x84}; lcd_data(pc1, 3);
    lcd_cmd(0xC1); uint8_t pc2[] = {0xC5};             lcd_data(pc2, 1);
    lcd_cmd(0xC2); uint8_t pc3[] = {0x0A, 0x00};       lcd_data(pc3, 2);
    lcd_cmd(0xC3); uint8_t pc4[] = {0x8A, 0x2A};       lcd_data(pc4, 2);
    lcd_cmd(0xC4); uint8_t pc5[] = {0x8A, 0xEE};       lcd_data(pc5, 2);

    /* VCOM + Gamma — VCOM from per-tube s_tube_vcom (user-tunable), gamma from
     * per-tube profile.  Keeping them separate lets the user dial contrast via
     * VCOM without having to change the gamma curve. */
    {
        uint8_t pidx = s_init_profiles[tube];
        if (pidx >= (uint8_t)PANEL_PROFILE_COUNT) pidx = 0;
        const panel_profile_t *p = &s_panel_profiles[pidx];
        lcd_cmd(0xC5); lcd_data(&s_tube_vcom[tube], 1);  /* VMCTR1 — per-tube VCOM */
        lcd_cmd(0xE0); lcd_data(p->gmp, 16);
        lcd_cmd(0xE1); lcd_data(p->gmn, 16);
    }

    lcd_cmd(0x3A); lcd_data_byte(0x05);            /* COLMOD  RGB565 */
    lcd_cmd(0x36); lcd_data_byte(0xC8);            /* MADCTL  MY|MX|BGR = 180° rotation */
    uint8_t fr[] = {0x01, 0x2C, 0x2D};
    lcd_cmd(0xB1); lcd_data(fr, 3);                /* FRMCTR1 */
    lcd_cmd(0x13); vTaskDelay(pdMS_TO_TICKS(10));  /* NORON   Normal Display Mode On
                                                     * Clears partial-display mode that
                                                     * can leave border pixels undriven
                                                     * on ST7735S variants. */
    lcd_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(50));  /* DISPON  */
    deselect_all();
}

void display_init(void)
{
    ESP_LOGI(TAG, "Initialising 6x ST7735 displays");
    gpio_config_t io = { .mode = GPIO_MODE_OUTPUT };
    for (int i = 0; i < LCD_COUNT; i++) {
        io.pin_bit_mask = 1ULL << cs_pins[i]; gpio_config(&io); gpio_set_level(cs_pins[i], 1);
    }
    io.pin_bit_mask = (1ULL << PIN_LCD_DC);
#if PIN_LCD_RST >= 0
    io.pin_bit_mask |= (1ULL << PIN_LCD_RST);
#endif
    gpio_config(&io);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_LCD_MOSI, .miso_io_num = -1, .sclk_io_num = PIN_LCD_SCK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = LCD_WIDTH*LCD_HEIGHT*2,
    };
    spi_bus_initialize(HSPI_HOST, &bus, SPI_DMA_CH_AUTO);
    spi_device_interface_config_t dev = {
        /* 26 MHz: max safe SPI speed when PSRAM is active on ESP32 (APB/3 = 26.666 MHz ceiling) */
        .clock_speed_hz = 26*1000*1000, .mode = 0, .spics_io_num = -1, .queue_size = 7,
    };
    spi_bus_add_device(HSPI_HOST, &dev, &spi_dev);

    ledc_timer_config_t tmr = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 50000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tmr);
    ledc_channel_config_t ch = {
        .gpio_num = PIN_LCD_BACKLIGHT, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 128,
    };
    ledc_channel_config(&ch);

    /* Initialise per-tube brightness to 100 (full) — static arrays are
     * zero-initialised by the linker, which would otherwise make every tube black. */
    for (int i = 0; i < LCD_COUNT; i++) s_tube_brightness[i] = 100;
    /* Initialise per-tube VCOM to 0x0E (Standard / original panel value).
     * display_apply_tube_vcom() will override before the task starts if the
     * saved config has different values. */
    for (int i = 0; i < LCD_COUNT; i++) s_tube_vcom[i] = 0x0E;
    /* Initialise per-tube gamma LUT to identity (gamma = 1.0, no correction). */
    for (int i = 0; i < LCD_COUNT; i++) { s_gamma[i] = 1.0f; rebuild_gamma_lut(i); }

    /* Hardware reset: if RST is wired to a GPIO, pulse it once to reset all
     * 6 displays together.  If PIN_LCD_RST == -1 the RST line is tied to
     * 3.3 V on the PCB — ST7735 comes up in reset-released state and the
     * init sequence below is sufficient to bring every panel up cleanly. */
#if PIN_LCD_RST >= 0
    gpio_set_level(PIN_LCD_RST, 0); vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));
#endif
    for (int i = 0; i < LCD_COUNT; i++) { st7735_init_one(i); display_fill(i, 0x0000); }
    /* display_apply_invert_mask(), display_apply_tube_offsets(), and
     * display_apply_tube_brightness() are called by app_main() after display_init()
     * once config_mgr has loaded the saved values. */

    /* Initialise U8g2 virtual display for H/T panel text rendering.
     * 128×64 full-framebuffer, no hardware I/O.  Buffer allocated internally
     * by U8g2 (static array inside u8g2_m_16_8_f).  Access via
     * u8g2_GetBufferPtr(&s_u8g2) after any draw call. */
    /* u8g2_Setup_bitmap_128x64_nodisp_f is not compiled into every U8g2 build.
     * Use the SH1106 128×64 variant instead — same full-framebuffer geometry,
     * identical tile-buffer layout; the SH1106 hardware callbacks are never
     * reached because ht_byte_cb / ht_gpio_cb are no-ops. */
    u8g2_Setup_sh1106_128x64_noname_f(&s_u8g2, U8G2_R0, ht_byte_cb, ht_gpio_cb);
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);

    ESP_LOGI(TAG, "Displays ready");
}

void display_set_brightness(uint8_t pct)
{
    /* Backlight control is active-LOW: duty 0 = full bright, 255 = off.
     * Invert so pct=100 → full bright, pct=0 → off. */
    if (pct > 100) pct = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, ((100 - pct) * 255) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* Cached invert mask — persisted here so display_apply_init_profiles() can
 * re-apply INVON/INVOFF after a per-tube SWRESET without needing the config. */
static uint8_t s_invert_mask = 0;

void display_apply_invert_mask(uint8_t mask)
{
    s_invert_mask = mask & 0x3F;
    for (int i = 0; i < LCD_COUNT; i++) {
        select_tube(i);
        lcd_cmd((s_invert_mask & (1u << i)) ? 0x21 : 0x20);  /* INVON : INVOFF */
    }
    deselect_all();
    ESP_LOGI(TAG, "lcd_invert_mask 0x%02X applied", s_invert_mask);
}

void display_wake(void)
{
    if (s_display_task_handle)
        xTaskAbortDelay(s_display_task_handle);
}

void display_invalidate(void)
{
    s_full_repaint_request = true;
    display_wake();
}

void display_apply_tube_offsets(const int8_t col_off[6], const int8_t row_off[6])
{
    for (int i = 0; i < LCD_COUNT; i++) {
        s_col_offsets[i] = col_off[i];
        s_row_offsets[i] = row_off[i];
    }
    ESP_LOGI(TAG, "tube col_offsets [%d,%d,%d,%d,%d,%d]  row_offsets [%d,%d,%d,%d,%d,%d]",
             col_off[0], col_off[1], col_off[2], col_off[3], col_off[4], col_off[5],
             row_off[0], row_off[1], row_off[2], row_off[3], row_off[4], row_off[5]);
    /* Signal the display task to force-repaint all tubes on the next tick so
     * the corrected window position takes effect immediately regardless of
     * whether tube content has changed (see s_full_repaint_request comment). */
    display_invalidate();
}

void display_apply_tube_brightness(const uint8_t br[6])
{
    for (int i = 0; i < LCD_COUNT; i++)
        s_tube_brightness[i] = (br[i] > 100) ? 100 : br[i];
    ESP_LOGI(TAG, "tube brightness [%u,%u,%u,%u,%u,%u]",
             s_tube_brightness[0], s_tube_brightness[1], s_tube_brightness[2],
             s_tube_brightness[3], s_tube_brightness[4], s_tube_brightness[5]);
}

void display_apply_tube_vcom(const uint8_t vcom[6])
{
    bool tube_changed[LCD_COUNT] = {false};
    bool any = false;
    for (int i = 0; i < LCD_COUNT; i++) {
        uint8_t v = vcom[i] & 0x3F;   /* clamp to valid VMCTR1 range */
        if (s_tube_vcom[i] != v) {
            s_tube_vcom[i]  = v;
            tube_changed[i] = true;
            any             = true;
        }
    }
    if (!any) return;

    /* VMCTR1 is only latched during the SLPOUT→DISPON window — same constraint
     * as profile gamma registers.  Suspend the display task and do a per-tube
     * SWRESET + full reinit for each tube whose VCOM changed. */
    if (s_display_task_handle) {
        vTaskSuspend(s_display_task_handle);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    for (int i = 0; i < LCD_COUNT; i++) {
        if (!tube_changed[i]) continue;
        ESP_LOGI(TAG, "tube %d: SWRESET + reinit (vcom 0x%02X)", i, s_tube_vcom[i]);
        st7735_init_one(i);
        select_tube(i);
        lcd_cmd((s_invert_mask & (1u << i)) ? 0x21 : 0x20);
        deselect_all();
        display_fill(i, 0x0000);
    }
    if (s_display_task_handle) vTaskResume(s_display_task_handle);

    ESP_LOGI(TAG, "tube VCOM [0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X]",
             s_tube_vcom[0], s_tube_vcom[1], s_tube_vcom[2],
             s_tube_vcom[3], s_tube_vcom[4], s_tube_vcom[5]);
}

void display_apply_tube_gamma(const float gamma[6])
{
    /* First pass: clamp, detect changes, update s_gamma[]. */
    bool tube_changed[LCD_COUNT] = {false};
    bool any = false;
    for (int i = 0; i < LCD_COUNT; i++) {
        float g = gamma[i];
        if (g < 0.5f) g = 0.5f;
        if (g > 3.0f) g = 3.0f;
        if (fabsf(g - s_gamma[i]) > 0.005f) {
            s_gamma[i]      = g;
            tube_changed[i] = true;
            any             = true;
        }
    }
    if (!any) return;

    /* Suspend the display task so it cannot read a partially-updated LUT while
     * rebuild_gamma_lut() is writing table entries.  Each rebuild takes only a
     * few µs (96 powf calls on Xtensa LX6 at 240 MHz) so the task is paused
     * for well under one frame period even when all 6 tubes change at once. */
    if (s_display_task_handle) {
        vTaskSuspend(s_display_task_handle);
    }
    /* Second pass: rebuild only the tubes that changed. */
    for (int i = 0; i < LCD_COUNT; i++) {
        if (tube_changed[i]) rebuild_gamma_lut(i);
    }
    if (s_display_task_handle) {
        vTaskResume(s_display_task_handle);
    }
    ESP_LOGI(TAG, "tube gamma [%.2f,%.2f,%.2f,%.2f,%.2f,%.2f]",
             s_gamma[0], s_gamma[1], s_gamma[2],
             s_gamma[3], s_gamma[4], s_gamma[5]);
}

void display_apply_init_profiles(const uint8_t profiles[6])
{
    /* Determine which tubes actually changed profile. */
    bool tube_changed[LCD_COUNT] = {false};
    bool any = false;
    for (int i = 0; i < LCD_COUNT; i++) {
        uint8_t p = (profiles[i] < (uint8_t)PANEL_PROFILE_COUNT) ? profiles[i] : 0;
        if (s_init_profiles[i] != p) {
            s_init_profiles[i] = p;
            tube_changed[i]    = true;
            any                = true;
        }
    }
    if (!any) return;

    /* VCOM (0xC5) and gamma registers (0xE0/0xE1) are only latched by ST7735S
     * variants during the SLPOUT → DISPON initialisation window; writes issued
     * during normal display-on mode are silently ignored.  The only reliable
     * way to change these registers at runtime is a per-tube software reset
     * (SWRESET, command 0x01) followed by the full init sequence.
     *
     * SWRESET is CS-gated — sending it to one tube's CS does not affect the
     * other five tubes.  st7735_init_one() performs:
     *   SWRESET (150 ms) → SLPOUT (120 ms) → power-ctrl → VCOM → gamma →
     *   COLMOD → MADCTL → FRMCTR1 → NORON (10 ms) → DISPON (50 ms)
     * for the already-selected tube and then deselects all.
     *
     * After reinit, s_invert_mask must be re-applied because SWRESET clears
     * every register — including the INVON/INVOFF state — back to POR defaults.
     *
     * ── SPI bus ownership ───────────────────────────────────────────────
     * st7735_init_one() calls select_tube() once at the top, then blocks for
     * 150 ms (SWRESET) and 120 ms (SLPOUT) via vTaskDelay.  Without
     * protection, the display task (Core 1) wakes during those delays, calls
     * select_tube(j) for a different tube, and later deselect_all() — which
     * drives tube i CS HIGH.  Every register write after the initial SWRESET
     * (SLPOUT, VCOM, gamma, DISPON) arrives on a deselected bus and is
     * silently ignored by the panel.  The panel wakes from reset but never
     * exits sleep mode, leaving the old VCOM and gamma untouched.
     *
     * Fix: suspend the display task for the entire reinit loop so we have
     * exclusive ownership of the SPI bus and all CS lines.
     * s_display_task_handle is NULL at boot (display_task_start() not yet
     * called), so the guard is safe for the boot-time apply path too. */
    if (s_display_task_handle) {
        vTaskSuspend(s_display_task_handle);
        /* Wait 2 ms after suspend to let any in-flight SPI transaction the
         * display task started complete in hardware before we touch the CS
         * lines.  A single 8-row chunk at 26 MHz takes ≈ 0.4 ms worst-case. */
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    for (int i = 0; i < LCD_COUNT; i++) {
        if (!tube_changed[i]) continue;

        ESP_LOGI(TAG, "tube %d: SWRESET + reinit (profile %u)", i, s_init_profiles[i]);
        st7735_init_one(i);   /* SWRESET + full register reload; deselects_all on return */

        /* Re-apply colour inversion — cleared by SWRESET. */
        select_tube(i);
        lcd_cmd((s_invert_mask & (1u << i)) ? 0x21 : 0x20);  /* INVON : INVOFF */
        deselect_all();

        /* Clear framebuffer; display task re-renders within one 200 ms tick. */
        display_fill(i, 0x0000);
    }

    if (s_display_task_handle) {
        vTaskResume(s_display_task_handle);
    }

    ESP_LOGI(TAG, "panel profiles [%u,%u,%u,%u,%u,%u] applied (per-tube reinit)",
             s_init_profiles[0], s_init_profiles[1], s_init_profiles[2],
             s_init_profiles[3], s_init_profiles[4], s_init_profiles[5]);
}

void display_debug_set_pwm(uint32_t freq_hz, uint8_t duty_pct)
{
    /* Clamp to safe LEDC range for 8-bit resolution.
     * LEDC_AUTO_CLK (80 MHz APB) minimum at 8-bit = 80e6/255 ≈ 314 kHz → cap at 80 kHz. */
    if (freq_hz < 1)      freq_hz = 1;
    if (freq_hz > 80000)  freq_hz = 80000;
    if (duty_pct > 100)   duty_pct = 100;

    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, freq_hz);
    /* active-LOW: invert pct so 100 = full bright (duty=0), 0 = off (duty=255) */
    uint32_t duty = ((uint32_t)(100 - duty_pct) * 255) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ESP_LOGI(TAG, "debug PWM: %lu Hz  duty_pct=%u  register=%lu",
             (unsigned long)freq_hz, (unsigned)duty_pct, (unsigned long)duty);
}

void display_debug_restore_pwm(void)
{
    /* Restore timer frequency; the display task re-applies the correct duty
     * on its next render tick (≤200 ms) via display_set_brightness(). */
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, 50000);
    ESP_LOGI(TAG, "debug PWM: restored to 50 kHz (duty restored by display task)");
}

/* Open an ST7735 pixel-write window (CASET + RASET + RAMWR) and leave the
 * data/command line high so the caller can stream pixel bytes immediately.
 * ox/oy are the physical column/row start addresses in the panel's frame buffer
 * (LCD_OFFSET_X + per-tube corrections already applied by the caller). */
static inline void open_lcd_window(uint8_t ox, uint8_t oy, uint8_t w, uint8_t h)
{
    lcd_cmd(0x2A);
    uint8_t ca[] = {0, ox, 0, (uint8_t)(ox + w - 1)};
    lcd_data(ca, 4);
    lcd_cmd(0x2B);
    uint8_t ra[] = {0, oy, 0, (uint8_t)(oy + h - 1)};
    lcd_data(ra, 4);
    lcd_cmd(0x2C);
    gpio_set_level(PIN_LCD_DC, 1);
}

void display_fill(int tube, uint16_t color)
{
    if (tube < 0 || tube >= LCD_COUNT) return;
    select_tube(tube);
    uint8_t ox = (uint8_t)((int)LCD_OFFSET_X + (int)s_burnin_shift_x + (int)s_col_offsets[tube]);
    uint8_t oy = (uint8_t)((int)LCD_OFFSET_Y                          + (int)s_row_offsets[tube]);
    open_lcd_window(ox, oy, LCD_WIDTH, LCD_HEIGHT);
    uint8_t line[LCD_WIDTH * 2];
    for (int x = 0; x < LCD_WIDTH; x++) { line[x*2] = color>>8; line[x*2+1] = color&0xFF; }
    for (int y = 0; y < LCD_HEIGHT; y++) {
        spi_transaction_t t = { .length = sizeof(line)*8, .tx_buffer = line };
        spi_device_polling_transmit(spi_dev, &t);
    }
    /* Also clear the |shift|-px column the burn-in shift leaves uncovered at the
     * panel edge, so a fill (e.g. a transition clear / blanked tube) doesn't
     * leave a stale sliver there.  Same colour, narrow window at the fixed edge. */
    int shift = (int)s_burnin_shift_x;
    int marg  = (shift >= 0) ? shift : -shift;
    if (marg > 0) {
        uint8_t marg_ox = (uint8_t)((int)LCD_OFFSET_X + (int)s_col_offsets[tube]
                                    + ((shift > 0) ? 0 : (LCD_WIDTH - marg)));
        open_lcd_window(marg_ox, oy, (uint8_t)marg, LCD_HEIGHT);
        for (int y = 0; y < LCD_HEIGHT; y++) {
            spi_transaction_t t = { .length = (size_t)(marg * 2) * 8, .tx_buffer = line };
            spi_device_polling_transmit(spi_dev, &t);
        }
    }
    deselect_all();
}

void display_show_digit(int tube, const uint8_t *data, int w, int h)
{
    if (tube < 0 || tube >= LCD_COUNT || !data) return;
    select_tube(tube);
    uint8_t ox = (uint8_t)((int)LCD_OFFSET_X + (int)s_burnin_shift_x + (int)s_col_offsets[tube]);
    uint8_t oy = (uint8_t)((int)LCD_OFFSET_Y                          + (int)s_row_offsets[tube]);
    open_lcd_window(ox, oy, (uint8_t)w, (uint8_t)h);
    /* `data` may be in PSRAM; ESP32 SPI DMA cannot access PSRAM directly.
     * Copy DISP_CHUNK_ROWS rows at a time into a stack SRAM chunk buffer,
     * optionally scale brightness per-pixel (integer arithmetic, negligible
     * overhead at 5 Hz), then send via polling transmit.  8 rows reduces
     * transaction count 160→20 per tube, cutting per-transaction
     * GPIO/controller overhead by ~8×. */
    uint8_t chunk[LCD_WIDTH * 2 * DISP_CHUNK_ROWS];  /* 1280 B — always SRAM */
    uint8_t br        = s_tube_brightness[tube];
    bool    do_br     = (br < 100);
    bool    do_gamma  = s_gamma_lut_active[tube];
    bool    do_px     = do_br || do_gamma;

    /* Burn-in column shift exposes |shift| visible columns at one edge that the
     * shifted write window never covers — they would otherwise retain stale
     * content (the previous mode / wait.jpg / older WeatherLive frame) as a
     * 1–2 px sliver.  Capture the adjacent edge column of THIS frame and paint
     * it into that margin after the main blit, so the content (e.g. the sky)
     * extends seamlessly to the panel edge.  shift>0 → left margin uses col 0;
     * shift<0 → right margin uses col w-1.  Only meaningful for full-width
     * (LCD_WIDTH) blits, which is every full-tube draw. */
    int     shift    = (int)s_burnin_shift_x;
    int     marg     = (shift >= 0) ? shift : -shift;
    bool    want_marg = (marg > 0 && w == LCD_WIDTH && h <= LCD_HEIGHT);
    int     edge_col = (shift > 0) ? 0 : (w - 1);
    uint8_t marg_ox  = (uint8_t)((int)LCD_OFFSET_X + (int)s_col_offsets[tube]
                                 + ((shift > 0) ? 0 : (LCD_WIDTH - marg)));
    uint8_t margbuf[2 * LCD_HEIGHT * 2];   /* ≤ 2 cols × 160 rows */

    for (int y = 0; y < h; y += DISP_CHUNK_ROWS) {
        int rows = (y + DISP_CHUNK_ROWS <= h) ? DISP_CHUNK_ROWS : h - y;
        memcpy(chunk, data + y * w * 2, (size_t)(rows * w * 2));
        if (do_px) {
            /* Single-pass brightness scale + gamma LUT.
             * Brightness runs first (linear channel scale), gamma LUT second
             * (maps the already-scaled value through out = in^γ).  Both are
             * pure integer arithmetic — no float ops inside the pixel loop. */
            int npx = rows * w;
            for (int j = 0; j < npx; j++) {
                uint16_t px = ((uint16_t)chunk[j * 2] << 8) | chunk[j * 2 + 1];
                uint32_t r = (px >> 11) & 0x1Fu;
                uint32_t g = (px >>  5) & 0x3Fu;
                uint32_t b =  px        & 0x1Fu;
                if (do_br) {
                    r = r * br / 100u;
                    g = g * br / 100u;
                    b = b * br / 100u;
                }
                if (do_gamma) {
                    r = s_gamma_lut_5bit[tube][r];
                    g = s_gamma_lut_6bit[tube][g];
                    b = s_gamma_lut_5bit[tube][b];
                }
                px = (uint16_t)((r << 11) | (g << 5) | b);
                chunk[j * 2]     = (uint8_t)(px >> 8);
                chunk[j * 2 + 1] = (uint8_t)(px & 0xFF);
            }
        }
        /* Stash the (transformed) edge column for the burn-in margin strip. */
        if (want_marg) {
            for (int rr = 0; rr < rows; rr++) {
                const uint8_t *src = chunk + (rr * w + edge_col) * 2;
                uint8_t       *dst = margbuf + ((y + rr) * marg) * 2;
                for (int c = 0; c < marg; c++) { dst[c*2] = src[0]; dst[c*2+1] = src[1]; }
            }
        }
        spi_transaction_t t = { .length = (size_t)(rows * w * 2) * 8, .tx_buffer = chunk };
        spi_device_polling_transmit(spi_dev, &t);
    }

    /* Paint the exposed burn-in margin with the captured edge column so the
     * panel edge shows this frame's content instead of a stale sliver. */
    if (want_marg) {
        open_lcd_window(marg_ox, oy, (uint8_t)marg, (uint8_t)h);
        spi_transaction_t tm = { .length = (size_t)(marg * h * 2) * 8, .tx_buffer = margbuf };
        spi_device_polling_transmit(spi_dev, &tm);
    }

    /* ── Update indicator overlay ──────────────────────────────────────── */
    /* When s_update_indicator is set, paint 4 rows of solid red at the
     * physical bottom of tube 5.  Physical bottom = the last 4 rows of the
     * address window (oy + h - 4 .. oy + h - 1).  Uses the already-computed
     * ox/oy so the overlay tracks per-tube window offset corrections.
     * Red in RGB565 big-endian = 0xF800 → bytes {0xF8, 0x00}.
     * The SPI device is still selected (cs low) so no extra select call is
     * needed — we simply reissue CASET/RASET for the 4-row window. */
    if (tube == LCD_COUNT - 1 && s_update_indicator) {
        uint8_t ca2[] = {0, ox, 0, (uint8_t)(ox + w - 1)};
        lcd_cmd(0x2A); lcd_data(ca2, 4);
        uint8_t ra2[] = {0, (uint8_t)(oy + h - 4), 0, (uint8_t)(oy + h - 1)};
        lcd_cmd(0x2B); lcd_data(ra2, 4);
        lcd_cmd(0x2C);
        gpio_set_level(PIN_LCD_DC, 1);
        uint8_t redline[LCD_WIDTH * 2];
        for (int x = 0; x < LCD_WIDTH; x++) { redline[x*2] = 0xF8; redline[x*2+1] = 0x00; }
        for (int row = 0; row < 4; row++) {
            spi_transaction_t tr = { .length = sizeof(redline) * 8, .tx_buffer = redline };
            spi_device_polling_transmit(spi_dev, &tr);
        }
    }

    deselect_all();
}

/* ── Update indicator API ────────────────────────────────────────────
 * Called by the web-server task via POST /api/update_notify.
 * s_update_indicator is declared near the top of this file so that
 * display_show_digit() (which runs in the display task) can read it. */
void display_set_update_indicator(bool active)
{
    s_update_indicator = active;
}

/* ── Anti burn-in API ────────────────────────────────────────────────
 * display_set_burnin_mask() — select which tubes enter colour-cycle mode.
 * mask bit N = tube N.  0x3F = all six tubes.  0x00 = restore all.
 * duration_s: 0 = run until manually stopped; otherwise auto-clears after
 *             that many seconds (use 3600/7200/10800/14400 for 1–4 h).     */
void display_set_burnin_mask(uint8_t mask, uint32_t duration_s)
{
    /* Take mutex so the two-field compound write is never observed as torn
     * by the display task's expiry check running on the other core. */
    if (s_timer_mutex) xSemaphoreTake(s_timer_mutex, portMAX_DELAY);
    s_burnin_mask     = mask & 0x3F;
    s_burnin_end_time = (mask && duration_s) ? time(NULL) + (time_t)duration_s : 0;
    if (s_timer_mutex) xSemaphoreGive(s_timer_mutex);
    ESP_LOGI(TAG, "burn-in mask: 0x%02X  duration: %s",
             (unsigned)s_burnin_mask,
             duration_s ? "timed" : "manual");
}

/* Fill one tube with a single frame of random RGB565 pixels — the core of
 * the static-snow burn-in.  One scan-line of random bytes is generated at a
 * time using the ESP32 hardware RNG (esp_fill_random) to keep stack usage
 * minimal (160 B) while ensuring every pixel is independently randomised.
 * The same s_burnin_shift_x hourly pixel-shift is applied to the CASET
 * window so snow co-operates with the column-drift anti-burn-in. */
static void display_fill_snow(int tube)
{
    if (tube < 0 || tube >= LCD_COUNT) return;
    select_tube(tube);
    uint8_t ox = (uint8_t)((int)LCD_OFFSET_X + (int)s_burnin_shift_x + (int)s_col_offsets[tube]);
    uint8_t oy = (uint8_t)((int)LCD_OFFSET_Y                          + (int)s_row_offsets[tube]);
    open_lcd_window(ox, oy, LCD_WIDTH, LCD_HEIGHT);
    uint8_t line[LCD_WIDTH * 2];   /* 160 B — always SRAM, within stack budget */
    for (int y = 0; y < LCD_HEIGHT; y++) {
        esp_fill_random(line, sizeof(line));
        spi_transaction_t t = {.length = sizeof(line) * 8, .tx_buffer = line};
        spi_device_polling_transmit(spi_dev, &t);
    }
    deselect_all();
}

void display_set_snow_mask(uint8_t mask, uint32_t duration_s)
{
    if (s_timer_mutex) xSemaphoreTake(s_timer_mutex, portMAX_DELAY);
    s_snow_mask     = mask & 0x3F;
    s_snow_end_time = (mask && duration_s) ? time(NULL) + (time_t)duration_s : 0;
    if (s_timer_mutex) xSemaphoreGive(s_timer_mutex);
    ESP_LOGI(TAG, "snow mask: 0x%02X  duration: %s",
             (unsigned)s_snow_mask,
             duration_s ? "timed" : "manual");
}

/* ════════════════════════════════════════════════════════════════════
 *  JPEG asset loader
 * ════════════════════════════════════════════════════════════════════ */
/* Forward declaration — flip_to_image() is defined after display_show_image()
 * but called from within it for FlipClock theme paths. */
static void flip_to_image(int tube, const uint8_t *new_buf, const char *path);

/* Allocate decode buffer from PSRAM so we don't exhaust DRAM. */
#define PSRAM_MALLOC(sz)  heap_caps_malloc((sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/* TJpgDec workspace: ~3100 bytes for JD_FASTDECODE=0 – round up with margin. */
#define JPEG_WORK_BUF_SIZE   3200

/* Upper bound on a single JPEG asset read from LittleFS.  Files larger than
 * this are almost certainly corrupt or wrongly-placed; reject before malloc. */
#define MAX_JPEG_FILE_SIZE  200000

/* Display task tick periods and stack size. */
#define DISPLAY_TICK_MS_FAST    50   /* spectrum mode — 20 Hz to match LED task */
#define DISPLAY_TICK_MS_MED    100   /* WeatherLive sky w/o precip — 10 Hz */
#define DISPLAY_TICK_MS_SLOW   200   /* all other modes — 5 Hz */

/* Weather panel indices — weather_panel local in display_task. */
#define WEATHER_PANEL_TEMP  0   /* temperature + icon */
#define WEATHER_PANEL_HUM   1   /* humidity */
#define WEATHER_PANEL_SUN   2   /* sunrise + sunset times */
#define WEATHER_PANEL_WIND  3   /* wind speed — procedural glyph + digits + unit */
#define WEATHER_PANEL_HILO  4   /* daily Hi / Lo — internal HI→LO sub-rotation */
/* Stack: config snapshot (~1900 B) + JPEG decode call chain (~3-4 KB).
 * 8 KB was too tight — panic handler couldn't print a backtrace. */
/* 12288 was borderline: cfg_snap (~1.9 KB) had to be moved to BSS because the
 * JPEG-decode call chain alone approached the limit (see the static cfg_snap
 * comment in display_task).  The AP-PIN digit renderer (pin_draw_tube) stacks
 * a 1280 B SPI chunk buffer plus the U8g2 draw chain on top of the loop frame
 * — the deepest single path in the task — so give it real headroom. */
#define DISPLAY_STACK_SIZE   16384

/* ── Theme error tracking ────────────────────────────────────────────────
 * Holds the path of the last image that failed to decode (e.g. wrong size,
 * truncated JPEG).  Exposed via display_get_theme_error() → api/status →
 * web UI banner.  Cleared automatically when the theme changes. */
/* Buffer: IMG_CACHE_PATH_MAX (320) + " (65535x65535 decoded, need 80x160)" (~36) + NUL */
static char s_theme_error[384] = {0};

const char *display_get_theme_error(void)
{
    return s_theme_error[0] ? s_theme_error : NULL;
}

/* ── Image decode cache ─────────────────────────────────────────────────
 * LRU cache of decoded RGB565 frames in PSRAM.  Eliminates re-reading from
 * SPIFFS and re-decoding JPEG for images that have not changed since the
 * previous render tick (e.g. clock digits that stay the same, weather icon).
 *
 * Each slot permanently owns a 25 600-byte PSRAM output buffer — no malloc
 * on cache hit, no fragmentation from eviction.  Total PSRAM for the cache:
 *   IMG_CACHE_ENTRIES × 25 600 B = 512 KB  (affordable on 8 MB PSRAM)
 *
 * Access is single-threaded (display task only) so no mutex is needed. */
#define IMG_CACHE_ENTRIES  20
#define IMG_CACHE_PATH_MAX 320

typedef struct {
    char     path[IMG_CACHE_PATH_MAX]; /* SPIFFS path key (no "/spiffs" prefix) */
    uint8_t *data;      /* permanently owned PSRAM RGB565 buffer, or NULL       */
    int      w, h;      /* decoded image dimensions                              */
    uint32_t used_at;   /* monotonic stamp; higher = more recently used         */
} img_cache_entry_t;

static img_cache_entry_t s_img_cache[IMG_CACHE_ENTRIES]; /* zero-init at startup */
static uint32_t          s_cache_clock = 0;

/* Pre-allocated buffers reused across every JPEG decode (avoids per-call
 * malloc/free churn on the PSRAM allocator). */
static uint8_t *s_jpeg_work_buf  = NULL;   /* JPEG_WORK_BUF_SIZE bytes (PSRAM) */
static uint8_t *s_flip_frame_buf = NULL;   /* FLIP_FRAME_BYTES bytes (PSRAM)   */

/* Forward-declared here so img_cache_flush() (below) can invalidate the memo
 * without requiring the full definition to appear first.  Defined near
 * ht_sample_theme_color() further down in this file. */
static char     s_theme_color_memo_theme[32];
static uint16_t s_theme_color_memo_color;

/* Evict all cache entries (called on theme change so stale paths age out). */
static void img_cache_flush(void)
{
    for (int i = 0; i < IMG_CACHE_ENTRIES; i++) {
        s_img_cache[i].path[0] = '\0';
        s_img_cache[i].used_at = 0;
        /* Retain the data allocation — it will be overwritten on next use. */
    }
    s_cache_clock = 0;
    /* Invalidate the theme-color memo: '1.jpg' has been evicted so the
     * cached colour no longer matches the new theme. */
    s_theme_color_memo_theme[0] = '\0';
    ESP_LOGI(TAG, "Image cache flushed");
}

/* ── img_cache_get ──────────────────────────────────────────────────────
 * Return a pointer to the decoded RGB565 frame for `path`.
 * On cache hit : stamp the entry as most-recently used and return immediately.
 * On cache miss: decode from SPIFFS into the LRU slot and return the pointer.
 * Returns NULL on decode failure; caller should display_fill(tube, 0x0000).
 * The returned pointer is valid until the next img_cache_flush() call or
 * until the same slot is evicted.  Callers must NOT free() the pointer. */
static const uint8_t *img_cache_get(const char *path, int *w_out, int *h_out)
{
    /* WeatherLive ships no JPEG assets.  display_show_image() already renders
     * those procedurally, but several callers hit the cache DIRECTLY for theme
     * metadata (theme-colour sampling, blank.jpg backgrounds, FlipClock priming,
     * the colon diff box).  Return "absent" silently for any WeatherLive theme
     * path so those callers take their NULL/black fallback without flooding the
     * log with "Image not found" for every #.jpg / blank.jpg every frame. */
    if (strstr(path, "/themes/WeatherLive")) {
        if (w_out) *w_out = 0;
        if (h_out) *h_out = 0;
        return NULL;
    }

    /* ── Cache lookup ── */
    for (int i = 0; i < IMG_CACHE_ENTRIES; i++) {
        if (s_img_cache[i].data &&
            strncmp(s_img_cache[i].path, path, IMG_CACHE_PATH_MAX - 1) == 0) {
            s_img_cache[i].used_at = ++s_cache_clock;
            if (w_out) *w_out = s_img_cache[i].w;
            if (h_out) *h_out = s_img_cache[i].h;
            return s_img_cache[i].data;
        }
    }

    /* ── Cache miss: choose slot (prefer empty, then LRU) ── */
    int slot = 0;
    for (int i = 0; i < IMG_CACHE_ENTRIES; i++) {
        if (!s_img_cache[i].data) { slot = i; break; }
        if (s_img_cache[i].used_at < s_img_cache[slot].used_at) slot = i;
    }

    /* ── Decode from SPIFFS ── */
    char full[IMG_CACHE_PATH_MAX + 7];
    snprintf(full, sizeof(full), "/spiffs%s", path);
    FILE *f = fopen(full, "rb");
    if (!f) { ESP_LOGW(TAG, "Image not found: %s", full); return NULL; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > MAX_JPEG_FILE_SIZE) { fclose(f); return NULL; }

    uint8_t *jpeg_buf = PSRAM_MALLOC(sz);
    if (!jpeg_buf) { fclose(f); return NULL; }
    if (fread(jpeg_buf, 1, (size_t)sz, f) != (size_t)sz) {
        ESP_LOGW(TAG, "Truncated read: %s", full);
        fclose(f); free(jpeg_buf); return NULL;
    }
    fclose(f);

    /* Allocate output buffer on first use of this slot (kept forever). */
    if (!s_img_cache[slot].data) {
        s_img_cache[slot].data = PSRAM_MALLOC(LCD_WIDTH * LCD_HEIGHT * 2);
        if (!s_img_cache[slot].data) { free(jpeg_buf); return NULL; }
    }

    /* Ensure the work buffer is available (pre-allocated at init; lazy fallback). */
    if (!s_jpeg_work_buf)
        s_jpeg_work_buf = PSRAM_MALLOC(JPEG_WORK_BUF_SIZE);
    if (!s_jpeg_work_buf) { free(jpeg_buf); return NULL; }

    esp_jpeg_image_cfg_t dec_cfg = {0};
    dec_cfg.indata                       = jpeg_buf;
    dec_cfg.indata_size                  = (uint32_t)sz;
    dec_cfg.outbuf                       = s_img_cache[slot].data;
    dec_cfg.outbuf_size                  = LCD_WIDTH * LCD_HEIGHT * 2;
    dec_cfg.out_format                   = JPEG_IMAGE_FORMAT_RGB565;
    dec_cfg.out_scale                    = JPEG_IMAGE_SCALE_0;
    dec_cfg.flags.swap_color_bytes       = 1;   /* big-endian bytes for ST7735 */
    dec_cfg.advanced.working_buffer      = s_jpeg_work_buf;
    dec_cfg.advanced.working_buffer_size = JPEG_WORK_BUF_SIZE;

    esp_jpeg_image_output_t out_img = {0};
    esp_err_t err = esp_jpeg_decode(&dec_cfg, &out_img);
    free(jpeg_buf);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "JPEG decode failed (err=%d) decoded=%ux%u buf=%u: %s",
                 err, out_img.width, out_img.height, LCD_WIDTH * LCD_HEIGHT * 2, full);
        /* Record for the web UI banner — strip the /spiffs prefix for brevity */
        const char *rel = (strncmp(full, "/spiffs", 7) == 0) ? full + 7 : full;
        snprintf(s_theme_error, sizeof(s_theme_error),
                 "%.320s (%ux%u decoded, need %ux%u)",
                 rel, out_img.width, out_img.height, LCD_WIDTH, LCD_HEIGHT);
        s_img_cache[slot].path[0] = '\0';   /* slot reusable; data buffer kept for next decode */
        return NULL;
    }

    strncpy(s_img_cache[slot].path, path, IMG_CACHE_PATH_MAX - 1);
    s_img_cache[slot].path[IMG_CACHE_PATH_MAX - 1] = '\0';
    s_img_cache[slot].w       = (int)out_img.width;
    s_img_cache[slot].h       = (int)out_img.height;
    s_img_cache[slot].used_at = ++s_cache_clock;
    if (w_out) *w_out = s_img_cache[slot].w;
    if (h_out) *h_out = s_img_cache[slot].h;
    return s_img_cache[slot].data;
}

/* Defined later (with the WeatherLive renderer): draws a theme asset
 * procedurally over black when the active theme ships no JPEG assets. */
static void wl_render_asset(int tube, const char *path);
/* Defined later: lazy-allocated PSRAM scratch buffer shared by WL renderers. */
static uint8_t *wl_fb(void);
/* Defined later: WeatherLive helpers used by early render functions.
 * cx_is_wl_sky / wl_ensure_scene are declared after the mid-file includes
 * where nextube_config_t first becomes visible (see below). */
static void wl_tube_str(int tube, const uint8_t *font, const char *str, int by);

/* When set by display_show_ampm / display_show_number, used as a fallback
 * background for PNG compositing when same-directory blank.jpg is absent
 * (e.g. system-fallback PNGs, Numbers-directory PNGs). Cleared after use. */
static char s_png_bg_override[270] = "";

void display_show_image(int tube, const char *path)
{
    if (tube < 0 || tube >= LCD_COUNT || !path) return;
    /* WeatherLive themes have no JPEG assets — render any requested theme image
     * (digits, colon, °, %, AM/PM, K/M, …) as a procedural glyph over black so
     * every non-clock mode still works.  Clock mode never reaches here (it draws
     * its own animated tubes via render_weatherlive). */
    if (strstr(path, "/themes/WeatherLive")) {
        if ((s_burnin_mask | s_snow_mask) & (1u << tube)) return;
        wl_render_asset(tube, path);
        return;
    }
    /* Skip any tube that is currently held by the colour-cycle or snow burn-in.
     * Both modes overwrite the tube in the display task after normal rendering
     * completes, so writing a JPEG here would be immediately discarded.
     * Skipping avoids the image-cache lookup, JPEG decode (on miss), and the
     * full ~8 ms SPI frame write — for no visible benefit on the masked tube. */
    if ((s_burnin_mask | s_snow_mask) & (1u << tube)) return;

    /* PNG: composite over blank.jpg background (same dir), alpha-blend into fb. */
    {
        size_t plen = strlen(path);
        if (plen > 4 && strcmp(path + plen - 4, ".png") == 0) {
            uint8_t *fb = wl_fb();
            if (!fb) { display_fill(tube, 0x0000); return; }

            /* Seed fb with blank.jpg: prefer same directory, then the caller-supplied
             * override (e.g. theme AMPM/blank.jpg for system-path or Numbers PNGs). */
            const char *sl = strrchr(path, '/');
            if (sl) {
                char blank[270];
                snprintf(blank, sizeof(blank), "%.*sblank.jpg", (int)(sl - path + 1), path);
                int bw = 0, bh = 0;
                const uint8_t *bg = img_cache_get(blank, &bw, &bh);
                if ((!bg || bw != LCD_WIDTH || bh != LCD_HEIGHT) && s_png_bg_override[0]) {
                    bw = 0; bh = 0;
                    bg = img_cache_get(s_png_bg_override, &bw, &bh);
                }
                if (bg && bw == LCD_WIDTH && bh == LCD_HEIGHT)
                    memcpy(fb, bg, LCD_WIDTH * LCD_HEIGHT * 2);
                else
                    memset(fb, 0, LCD_WIDTH * LCD_HEIGHT * 2);
            } else {
                memset(fb, 0, LCD_WIDTH * LCD_HEIGHT * 2);
            }

            unsigned char *rgba = NULL;
            unsigned pw = 0, ph = 0;
            char _spng[270]; snprintf(_spng, sizeof(_spng), "/spiffs%s", path);
            bool ok = (lodepng_decode32_file(&rgba, &pw, &ph, _spng) == 0
                       && pw == (unsigned)LCD_WIDTH && ph == (unsigned)LCD_HEIGHT);
            if (ok) {
                uint8_t *dst8 = fb;
                for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
                    const unsigned char *s = rgba + i * 4;
                    uint8_t a = s[3];
                    if (a == 0) continue;
                    int idx = i * 2;
                    uint16_t px;
                    if (a == 255) {
                        px = ((uint16_t)(s[0] >> 3) << 11)
                           | ((uint16_t)(s[1] >> 2) << 5)
                           |  (uint16_t)(s[2] >> 3);
                    } else {
                        uint16_t bg16 = ((uint16_t)dst8[idx] << 8) | dst8[idx + 1];
                        int bg_r = ((bg16 >> 11) & 31) * 255 / 31;
                        int bg_g = ((bg16 >>  5) & 63) * 255 / 63;
                        int bg_b = ( bg16        & 31) * 255 / 31;
                        int out_r = (s[0] * a + bg_r * (255 - a)) / 255;
                        int out_g = (s[1] * a + bg_g * (255 - a)) / 255;
                        int out_b = (s[2] * a + bg_b * (255 - a)) / 255;
                        px = ((uint16_t)(out_r >> 3) << 11)
                           | ((uint16_t)(out_g >> 2) << 5)
                           |  (uint16_t)(out_b >> 3);
                    }
                    dst8[idx]     = (uint8_t)(px >> 8);
                    dst8[idx + 1] = (uint8_t)(px & 0xFF);
                }
            } else {
                ESP_LOGE(TAG, "PNG load failed: %s (%ux%u)", path, pw, ph);
            }
            free(rgba);
            display_show_digit(tube, fb, LCD_WIDTH, LCD_HEIGHT);
            return;
        }
    }

    int w = 0, h = 0;
    const uint8_t *rgb_buf = img_cache_get(path, &w, &h);
    if (!rgb_buf) { display_fill(tube, 0x0000); return; }

    /* Push RGB565 frame to LCD (with FlipClock animation) */
    if (w == LCD_WIDTH && h == LCD_HEIGHT &&
        strstr(path, "/FlipClock/") != NULL) {
        flip_to_image(tube, rgb_buf, path);
    } else {
        display_show_digit(tube, rgb_buf, w, h);
    }
    /* Cache owns the buffer — no free() here */
}

/* ── display_cx_push_image ────────────────────────────────────────────────────
 * Decode an externally-pushed JPG (from POST /api/cx_image) into the per-tube
 * push buffer for the 24H_CX "Pushed image" panel.  Runs in the httpd task, so
 * it uses its OWN JPEG work buffer (never the display task's s_jpeg_work_buf)
 * and serialises the buffer write against the display-task blit with
 * s_cx_push_mutex.  which: 0 = tube 5 (LCD 4), 1 = tube 6 (LCD 5).
 * The image must decode to exactly LCD_WIDTH×LCD_HEIGHT; returns false otherwise. */
bool display_cx_push_image(int which, const uint8_t *jpg, size_t len)
{
    static uint8_t *s_push_work = NULL;   /* dedicated decode scratch (httpd task) */
    if (which < 0 || which > 1 || !jpg || len == 0 || !s_cx_push_mutex) return false;

    xSemaphoreTake(s_cx_push_mutex, portMAX_DELAY);

    if (!s_cx_push_buf[which])
        s_cx_push_buf[which] = PSRAM_MALLOC(LCD_WIDTH * LCD_HEIGHT * 2);
    if (!s_push_work)
        s_push_work = PSRAM_MALLOC(JPEG_WORK_BUF_SIZE);
    if (!s_cx_push_buf[which] || !s_push_work) {
        xSemaphoreGive(s_cx_push_mutex);
        return false;
    }

    esp_jpeg_image_cfg_t cfg = {0};
    cfg.indata                       = (uint8_t *)jpg;
    cfg.indata_size                  = (uint32_t)len;
    cfg.outbuf                       = s_cx_push_buf[which];
    cfg.outbuf_size                  = LCD_WIDTH * LCD_HEIGHT * 2;
    cfg.out_format                   = JPEG_IMAGE_FORMAT_RGB565;
    cfg.out_scale                    = JPEG_IMAGE_SCALE_0;
    cfg.flags.swap_color_bytes       = 1;   /* big-endian bytes for ST7735 */
    cfg.advanced.working_buffer      = s_push_work;
    cfg.advanced.working_buffer_size = JPEG_WORK_BUF_SIZE;

    esp_jpeg_image_output_t out = {0};
    esp_err_t err = esp_jpeg_decode(&cfg, &out);
    bool ok = (err == ESP_OK && out.width == LCD_WIDTH && out.height == LCD_HEIGHT);
    s_cx_push_valid[which] = ok;
    if (ok) s_cx_push_seq++;     /* signal the display task to re-render */
    xSemaphoreGive(s_cx_push_mutex);

    if (!ok)
        ESP_LOGW(TAG, "cx_image: decode failed (err=%d, %ux%u, need %dx%d)",
                 err, out.width, out.height, LCD_WIDTH, LCD_HEIGHT);
    return ok;
}

/* ── display_show_image_region ───────────────────────────────────────────────
 * Render a rectangular sub-region of a decoded image to a specific position
 * within a tube's LCD address space.  Used by the 24H Custom tube-6 panel
 * renderer to composite two half-images (e.g. day name + date digits) into
 * the single tube without a separate PSRAM compose buffer.
 *
 * path          : LittleFS path, loaded through the normal img_cache
 * src_x, src_y  : top-left of the crop rectangle in decoded image coordinates
 * src_w, src_h  : size of the crop rectangle (clamped to image bounds)
 * dst_x, dst_y  : destination top-left on the tube (added to ox/oy offsets)
 *
 * Burns the tube-select / deselect cycle internally so multiple calls for
 * the same tube re-select it each time — acceptable at 5 Hz.              */
static void display_show_image_region(int tube, const char *path,
                                      int src_x, int src_y, int src_w, int src_h,
                                      int dst_x, int dst_y)
{
    if (tube < 0 || tube >= LCD_COUNT) return;
    if ((s_burnin_mask | s_snow_mask) & (1u << tube)) return;

    if (!path) {
        /* NULL path = fill the destination region with black.
         * Used by the H/T panel to clear digit slots that are unused on a
         * given frame (e.g. single-digit number occupying only the right
         * slot), preventing stale pixels from a previous larger value. */
        if (src_w <= 0 || src_h <= 0) return;
        select_tube(tube);
        uint8_t ox = (uint8_t)((int)LCD_OFFSET_X + (int)s_burnin_shift_x
                               + (int)s_col_offsets[tube] + dst_x);
        uint8_t oy = (uint8_t)((int)LCD_OFFSET_Y + (int)s_row_offsets[tube] + dst_y);
        open_lcd_window(ox, oy, (uint8_t)src_w, (uint8_t)src_h);
        uint8_t chunk[LCD_WIDTH * 2 * DISP_CHUNK_ROWS];
        memset(chunk, 0, (size_t)src_w * 2 * DISP_CHUNK_ROWS);
        for (int y = 0; y < src_h; y += DISP_CHUNK_ROWS) {
            int rows = (y + DISP_CHUNK_ROWS <= src_h) ? DISP_CHUNK_ROWS : src_h - y;
            spi_transaction_t tr = { .length = (size_t)(rows * src_w * 2) * 8,
                                     .tx_buffer = chunk };
            spi_device_polling_transmit(spi_dev, &tr);
        }
        deselect_all();
        return;
    }

    int img_w = 0, img_h = 0;
    const uint8_t *buf = img_cache_get(path, &img_w, &img_h);
    if (!buf || img_w == 0 || img_h == 0) return;

    /* Clamp crop to image bounds */
    if (src_x < 0) src_x = 0;
    if (src_y < 0) src_y = 0;
    if (src_x >= img_w) return;
    if (src_y >= img_h) return;
    if (src_w <= 0 || src_x + src_w > img_w) src_w = img_w - src_x;
    if (src_h <= 0 || src_y + src_h > img_h) src_h = img_h - src_y;

    select_tube(tube);
    uint8_t ox = (uint8_t)((int)LCD_OFFSET_X + (int)s_burnin_shift_x
                           + (int)s_col_offsets[tube] + dst_x);
    uint8_t oy = (uint8_t)((int)LCD_OFFSET_Y + (int)s_row_offsets[tube] + dst_y);
    open_lcd_window(ox, oy, (uint8_t)src_w, (uint8_t)src_h);

    uint8_t  chunk[LCD_WIDTH * 2 * DISP_CHUNK_ROWS];
    uint8_t  br     = s_tube_brightness[tube];
    bool     do_br  = (br < 100);
    bool     do_gam = s_gamma_lut_active[tube];
    bool     do_px  = do_br || do_gam;

    for (int y = src_y; y < src_y + src_h; y += DISP_CHUNK_ROWS) {
        int rows = DISP_CHUNK_ROWS;
        if (y + rows > src_y + src_h) rows = (src_y + src_h) - y;
        for (int r = 0; r < rows; r++)
            memcpy(chunk + r * src_w * 2,
                   buf + (y + r) * img_w * 2 + src_x * 2,
                   (size_t)(src_w * 2));
        if (do_px) {
            int npx = rows * src_w;
            for (int j = 0; j < npx; j++) {
                uint16_t px = ((uint16_t)chunk[j*2] << 8) | chunk[j*2+1];
                uint32_t r5 = (px >> 11) & 0x1Fu;
                uint32_t g6 = (px >>  5) & 0x3Fu;
                uint32_t b5 =  px        & 0x1Fu;
                if (do_br) { r5=r5*br/100u; g6=g6*br/100u; b5=b5*br/100u; }
                if (do_gam) {
                    r5 = s_gamma_lut_5bit[tube][r5];
                    g6 = s_gamma_lut_6bit[tube][g6];
                    b5 = s_gamma_lut_5bit[tube][b5];
                }
                px = (uint16_t)((r5<<11)|(g6<<5)|b5);
                chunk[j*2]   = (uint8_t)(px>>8);
                chunk[j*2+1] = (uint8_t)(px&0xFF);
            }
        }
        spi_transaction_t tr = {
            .length    = (size_t)(rows * src_w * 2) * 8,
            .tx_buffer = chunk,
        };
        spi_device_polling_transmit(spi_dev, &tr);
    }
    deselect_all();
}

/* ════════════════════════════════════════════════════════════════════
 *  FlipClock split-flap animation
 *
 *  Activated automatically whenever:
 *    • the image path contains "/FlipClock/"   (theme detection from path)
 *    • the decoded frame is exactly LCD_WIDTH × LCD_HEIGHT
 *    • the path differs from the last path shown on this tube  ← prevents
 *      static images (e.g. colon) from re-animating every render tick
 *    • a cached previous frame exists (first show is instant, no animation)
 *
 *  Algorithm – 8 intermediate frames, then caller pushes the final frame:
 *
 *    Phase 1 (steps 0-3): old digit top half "falls" toward viewer
 *      – visible rows: cos(22.5°)×80=74 → cos(45°)×80=57 → 31 → 0
 *      – top portion is vertically compressed (nearest-neighbour scale)
 *      – thin dark band simulates card-edge thickness
 *      – bottom half stays as old digit throughout phase 1
 *
 *    Phase 2 (steps 4-7): new digit top half "unfolds" away from viewer
 *      – visible rows: 0 → 31 → 57 → 74  (reverse of phase 1)
 *      – bottom half switches to new digit at the phase boundary
 *
 *    Final frame: pushed by display_show_image() after flip_to_image()
 *      returns → full new digit, 80 rows top + 80 rows bottom.
 *
 *  Timing: each display_show_digit() ≈ 8 ms SPI  →  8 × 8 ms ≈ 67 ms
 *  total animation, comfortably inside the 200 ms render tick.
 *
 *  Memory: 6 × 25 600 B ≈ 150 KB PSRAM for per-tube previous-frame cache
 *          +  25 600 B PSRAM temporary frame (allocated/freed per call)
 * ════════════════════════════════════════════════════════════════════ */

#define FLIP_FRAME_BYTES  (LCD_WIDTH * LCD_HEIGHT * 2)  /* 25 600 */
#define FLIP_ROW_BYTES    (LCD_WIDTH * 2)               /* 160    */
#define FLIP_HALF         (LCD_HEIGHT / 2)              /* 80     */
#define FLIP_STEPS        8                             /* animation frames */
#define FLIP_EDGE_ROWS    2                             /* card-edge px     */

/* Per-tube state -------------------------------------------------------- */
static uint8_t *s_flip_prev[LCD_COUNT];        /* cached last frame (PSRAM) */
static char     s_flip_path[LCD_COUNT][320];   /* last image path per tube  */

/* flip_build_frame -------------------------------------------------------
 * Fill `out` (FLIP_FRAME_BYTES) with one animation frame.
 *   step 0-3 : old top half folds away  (phase 1)
 *   step 4-7 : new top half unfolds      (phase 2)
 */
static void flip_build_frame(uint8_t       *out,
                              const uint8_t *old_buf,
                              const uint8_t *new_buf,
                              int            step)
{
    bool phase2    = (step >= FLIP_STEPS / 2);
    int  half_step = step % (FLIP_STEPS / 2);           /* 0 .. 3 */

    /* Cosine easing: map half_step → visible top-half row count.
     *   Phase 1: angle = (half_step+1) × π/8   →  cos ≈ 0.92, 0.71, 0.38, 0.0
     *   Phase 2: angle = (4-half_step)  × π/8   →  cos ≈ 0.0,  0.38, 0.71, 0.92
     * Both produce top_rows ∈ { 74, 57, 31, 0 } (falling) or { 0, 31, 57, 74 } (rising). */
    int   angle_n  = phase2 ? (FLIP_STEPS / 2 - half_step) : (half_step + 1);
    float angle    = (float)angle_n * (float)M_PI / (float)FLIP_STEPS;
    int   top_rows = (int)(FLIP_HALF * cosf(angle) + 0.5f);
    if (top_rows < 0)         top_rows = 0;
    if (top_rows > FLIP_HALF) top_rows = FLIP_HALF;

    /* Source buffer for the top and bottom regions */
    const uint8_t *top_src = phase2 ? new_buf : old_buf;
    const uint8_t *bot_src = phase2 ? new_buf : old_buf;

    /* ── Top portion: vertically compress FLIP_HALF src rows → top_rows ── */
    for (int dy = 0; dy < top_rows; dy++) {
        /* Nearest-neighbour downscale: map dst row → src row within top half */
        int sy = (top_rows > 1) ? (dy * (FLIP_HALF - 1) / (top_rows - 1)) : 0;
        memcpy(out + (size_t)dy * FLIP_ROW_BYTES,
               top_src + (size_t)sy * FLIP_ROW_BYTES,
               FLIP_ROW_BYTES);
    }

    /* ── Card-edge band: narrow dark strip just below the top portion ── */
    int edge_end = top_rows + FLIP_EDGE_ROWS;
    if (edge_end > FLIP_HALF) edge_end = FLIP_HALF;
    for (int dy = top_rows; dy < edge_end; dy++)
        memset(out + (size_t)dy * FLIP_ROW_BYTES, 0x10, FLIP_ROW_BYTES);

    /* ── Gap: rows between edge and hinge filled with black ── */
    for (int dy = edge_end; dy < FLIP_HALF; dy++)
        memset(out + (size_t)dy * FLIP_ROW_BYTES, 0x00, FLIP_ROW_BYTES);

    /* ── Bottom half: old during phase 1, new during phase 2 ── */
    memcpy(out  + (size_t)FLIP_HALF * FLIP_ROW_BYTES,
           bot_src + (size_t)FLIP_HALF * FLIP_ROW_BYTES,
           (size_t)FLIP_HALF * FLIP_ROW_BYTES);
}

/* flip_to_image ----------------------------------------------------------
 * Run the split-flap animation from the cached previous frame to new_buf,
 * push the final frame, then update the per-tube cache.
 * Called from display_show_image() when /FlipClock/ is detected.
 */
static void flip_to_image(int tube, const uint8_t *new_buf, const char *path)
{
    bool path_changed = (strncmp(path,
                                 s_flip_path[tube],
                                 sizeof(s_flip_path[tube]) - 1) != 0);

    if (path_changed && s_flip_prev[tube] != NULL && s_flip_frame_buf != NULL) {
        /* Use the pre-allocated shared frame buffer — no per-animation malloc. */
        for (int step = 0; step < FLIP_STEPS; step++) {
            flip_build_frame(s_flip_frame_buf, s_flip_prev[tube], new_buf, step);
            /* display_show_digit() SPI transmission (~8 ms) naturally
             * paces the animation — no extra vTaskDelay() needed. */
            display_show_digit(tube, s_flip_frame_buf, LCD_WIDTH, LCD_HEIGHT);
        }
    }
    /* If s_flip_frame_buf is NULL (pre-alloc failed): animation skipped;
     * final frame pushed below so the tube still shows the correct image. */

    /* Always push the final (complete) new frame */
    display_show_digit(tube, new_buf, LCD_WIDTH, LCD_HEIGHT);

    /* Update per-tube previous-frame cache */
    if (!s_flip_prev[tube])
        s_flip_prev[tube] = PSRAM_MALLOC(FLIP_FRAME_BYTES);
    if (s_flip_prev[tube])
        memcpy(s_flip_prev[tube], new_buf, FLIP_FRAME_BYTES);

    strncpy(s_flip_path[tube], path, sizeof(s_flip_path[tube]) - 1);
    s_flip_path[tube][sizeof(s_flip_path[tube]) - 1] = '\0';
}

/* flip_prime_blank -------------------------------------------------------
 * Silently prime the flip animation cache for `tube` with AMPM/blank.jpg,
 * WITHOUT displaying anything on screen.
 *
 * Purpose: when entering weather mode from clock mode (12H), the degree-
 * symbol tube may have last shown colon.jpg.  Without priming, the flip
 * animation would show "colon folding into degree-symbol", which looks wrong.
 * After priming, the animation is "blank folding into degree-symbol", which
 * is the visually correct split-flap behaviour.
 *
 * No-op when:
 *   • theme is not "FlipClock" (no animation for other themes)
 *   • cache already holds blank.jpg for this tube (already primed)
 *   • cache already holds a degree image (temp just changed, no reset needed)
 */
static void flip_prime_blank(int tube, const char *theme)
{
    if (!theme || strncmp(theme, "FlipClock", 9) != 0) return;

    /* Build the blank.jpg path we would prime with */
    char blank_path[320];
    snprintf(blank_path, sizeof(blank_path),
             "/images/themes/%s/AMPM/blank.jpg", theme);

    /* Skip if the cache already holds blank or a degree image —
     * animating from blank→blank or degree→degree looks wrong. */
    if (strstr(s_flip_path[tube], "/AMPM/blank.jpg")     != NULL) return;
    if (strstr(s_flip_path[tube], "/Temperature/degree") != NULL) return;

    /* Fetch blank.jpg via the image cache — free decode if it's cached. */
    int w = 0, h = 0;
    const uint8_t *frame = img_cache_get(blank_path, &w, &h);
    if (!frame || w != LCD_WIDTH || h != LCD_HEIGHT) return;

    /* Copy into the per-tube previous-frame buffer for the flip animation. */
    if (!s_flip_prev[tube])
        s_flip_prev[tube] = PSRAM_MALLOC(FLIP_FRAME_BYTES);
    if (s_flip_prev[tube]) {
        memcpy(s_flip_prev[tube], frame, FLIP_FRAME_BYTES);
        strncpy(s_flip_path[tube], blank_path, sizeof(s_flip_path[tube]) - 1);
        s_flip_path[tube][sizeof(s_flip_path[tube]) - 1] = '\0';
    }
}

/* ── Path builders ─────────────────────────────────────────────────── */
void display_path_number(char *buf, size_t n, const char *theme, int digit)
{ snprintf(buf, n, "/images/themes/%s/Numbers/%d.jpg", theme, digit); }

void display_path_ampm(char *buf, size_t n, const char *theme, const char *name)
{ snprintf(buf, n, "/images/themes/%s/AMPM/%s.jpg", theme, name); }

void display_path_weather(char *buf, size_t n, const char *theme, const char *cond)
{ snprintf(buf, n, "/images/themes/%s/MutiInfo/Weather/%s.jpg", theme, cond); }

void display_path_temperature(char *buf, size_t n, const char *theme, const char *name)
{ snprintf(buf, n, "/images/themes/%s/MutiInfo/Temperature/%s.jpg", theme, name); }

void display_path_humidity(char *buf, size_t n, const char *theme, const char *name)
{ snprintf(buf, n, "/images/themes/%s/MutiInfo/Humidity/%s.jpg", theme, name); }


/* ── High-level helpers ────────────────────────────────────────────── */

/* Check whether a SPIFFS file exists given its logical path (no "/spiffs" prefix).
 * All img_cache / display_show_image paths omit the mount-point prefix; fopen
 * probes must add it themselves or they silently always fail.               */
static bool spiffs_file_exists(const char *path)
{
    char full[270];
    snprintf(full, sizeof(full), "/spiffs%s", path);
    FILE *f = fopen(full, "r");
    if (!f) return false;
    fclose(f);
    return true;
}

void display_show_number(int tube, int digit, const char *theme)
{
    char p[256];
    /* Prefer PNG over JPG when the theme supplies one. */
    snprintf(p, sizeof(p), "/images/themes/%s/Numbers/%d.png", theme, digit);
    if (!spiffs_file_exists(p)) display_path_number(p, sizeof(p), theme, digit);
    snprintf(s_png_bg_override, sizeof(s_png_bg_override),
             "/images/themes/%s/AMPM/blank.jpg", theme);
    display_show_image(tube, p);
    s_png_bg_override[0] = '\0';
}

void display_show_ampm(int tube, const char *name, const char *theme)
{
    char p[256]; display_path_ampm(p, sizeof(p), theme, name);
    /* WeatherLive ships no JPEG digit/symbol assets, so those are rendered
     * procedurally over black via display_show_image's interception.  The
     * platform/mode icons, however, exist as theme-independent system assets
     * (/images/system/{name}.jpg) and ARE safe to load — so for those we
     * substitute the system path, which display_show_image loads normally. */
    if (strstr(p, "/themes/WeatherLive")) {
        if (!strcmp(name, "youtube")   || !strcmp(name, "instagram") ||
            !strcmp(name, "tiktok")    || !strcmp(name, "mastodon")  ||
            !strcmp(name, "wait")) {
            /* Prefer PNG (transparent) over JPG for system icons. */
            snprintf(p, sizeof(p), "/images/system/%s.png", name);
            if (!spiffs_file_exists(p)) snprintf(p, sizeof(p), "/images/system/%s.jpg", name);
        }
        display_show_image(tube, p);
        return;
    }
    /* Prefer PNG over JPG when the theme supplies one. */
    char png_p[256];
    snprintf(png_p, sizeof(png_p), "/images/themes/%s/AMPM/%s.png", theme, name);
    if (spiffs_file_exists(png_p)) {
        snprintf(s_png_bg_override, sizeof(s_png_bg_override),
                 "/images/themes/%s/AMPM/blank.jpg", theme);
        display_show_image(tube, png_p);
        s_png_bg_override[0] = '\0';
        return;
    }
    /* Fall back to /images/system/{name}.png (then .jpg) when the theme-specific
     * asset is absent (e.g. a custom theme that predates the social-media icons).
     * Supply the theme's AMPM blank as the compositing background so system-path
     * PNGs appear over the correct theme background instead of black. */
    if (!img_cache_get(p, NULL, NULL)) {
        snprintf(p, sizeof(p), "/images/system/%s.png", name);
        if (!spiffs_file_exists(p)) snprintf(p, sizeof(p), "/images/system/%s.jpg", name);
    }
    snprintf(s_png_bg_override, sizeof(s_png_bg_override),
             "/images/themes/%s/AMPM/blank.jpg", theme);
    display_show_image(tube, p);
    s_png_bg_override[0] = '\0';
}

/* ── Colon-blink partial update ──────────────────────────────────────────
 * The colon tube alternates colon.jpg / blank.jpg every second.  Those two
 * images differ ONLY in the small colon-dot region; the rest of the tube is
 * identical.  Pushing the full 80×160 tube every second is a large per-second
 * SPI burst that couples into the always-on amplifier (audible 1 Hz pulse).
 * Instead we push ONLY the bounding box of pixels that differ between the two
 * images, leaving the identical background untouched — slashing the per-second
 * SPI burst (and its coupled noise).  The diff box is computed once per theme. */
static char s_colon_box_theme[32] = {0};
static int  s_colon_box_state = 0;   /* 0=unknown, 1=valid, -1=unavailable */
static int  s_colon_bx0, s_colon_by0, s_colon_bw, s_colon_bh;

static void colon_box_compute(const char *theme)
{
    if (s_colon_box_state != 0 && strcmp(theme, s_colon_box_theme) == 0)
        return;
    strncpy(s_colon_box_theme, theme, sizeof(s_colon_box_theme) - 1);
    s_colon_box_theme[sizeof(s_colon_box_theme) - 1] = '\0';
    s_colon_box_state = -1;

    char pc[256], pb[256];
    display_path_ampm(pc, sizeof(pc), theme, "colon");
    display_path_ampm(pb, sizeof(pb), theme, "blank");
    int wc = 0, hc = 0, wb = 0, hb = 0;
    const uint8_t *c = img_cache_get(pc, &wc, &hc);
    const uint8_t *b = img_cache_get(pb, &wb, &hb);
    if (!c || !b || wc != wb || hc != hb || wc <= 0 || hc <= 0) return;

    int x0 = wc, y0 = hc, x1 = -1, y1 = -1;
    for (int y = 0; y < hc; y++) {
        const uint8_t *cr = c + (size_t)y * wc * 2;
        const uint8_t *brow = b + (size_t)y * wc * 2;
        for (int x = 0; x < wc; x++) {
            if (cr[x*2] != brow[x*2] || cr[x*2+1] != brow[x*2+1]) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
    }
    if (x1 < 0) return;              /* identical → keep full-draw fallback */
    s_colon_bx0 = x0; s_colon_by0 = y0;
    s_colon_bw  = x1 - x0 + 1;
    s_colon_bh  = y1 - y0 + 1;
    s_colon_box_state = 1;
    ESP_LOGI(TAG, "Colon diff box '%s': x%d y%d %dx%d (was 80x160)",
             theme, x0, y0, s_colon_bw, s_colon_bh);
}

/* Per-second colon blink: push only the colon-dot diff box instead of the full
 * tube.  Falls back to a full display_show_ampm() if the box is unavailable.
 * Applies the same per-tube brightness/gamma as display_show_digit so the dots
 * match the digit colour.  The non-box background is already on-screen from the
 * last full render (identical in both images), so it needs no repaint. */
static void display_show_colon_blink(int tube, const char *theme, bool show_colon)
{
    colon_box_compute(theme);
    if (s_colon_box_state != 1) {
        display_show_ampm(tube, show_colon ? "colon" : "blank", theme);
        return;
    }
    char p[256];
    display_path_ampm(p, sizeof(p), theme, show_colon ? "colon" : "blank");
    int w = 0, h = 0;
    const uint8_t *img = img_cache_get(p, &w, &h);
    if (!img || w <= 0 || h <= 0) {
        display_show_ampm(tube, show_colon ? "colon" : "blank", theme);
        return;
    }

    uint8_t br     = s_tube_brightness[tube];
    bool    do_br  = (br < 100);
    bool    do_gam = s_gamma_lut_active[tube];

    select_tube(tube);
    uint8_t ox = (uint8_t)((int)LCD_OFFSET_X + (int)s_burnin_shift_x
                            + (int)s_col_offsets[tube] + s_colon_bx0);
    uint8_t oy = (uint8_t)((int)LCD_OFFSET_Y + (int)s_row_offsets[tube] + s_colon_by0);
    open_lcd_window(ox, oy, (uint8_t)s_colon_bw, (uint8_t)s_colon_bh);

    uint8_t line[LCD_WIDTH * 2];
    for (int yy = 0; yy < s_colon_bh; yy++) {
        const uint8_t *src = img + ((size_t)(s_colon_by0 + yy) * w + s_colon_bx0) * 2;
        if (do_br || do_gam) {
            for (int xx = 0; xx < s_colon_bw; xx++) {
                uint16_t px = ((uint16_t)src[xx*2] << 8) | src[xx*2+1];
                uint32_t r = (px >> 11) & 0x1Fu, g = (px >> 5) & 0x3Fu, b = px & 0x1Fu;
                if (do_br)  { r = r*br/100u; g = g*br/100u; b = b*br/100u; }
                if (do_gam) { r = s_gamma_lut_5bit[tube][r];
                              g = s_gamma_lut_6bit[tube][g];
                              b = s_gamma_lut_5bit[tube][b]; }
                px = (uint16_t)((r << 11) | (g << 5) | b);
                line[xx*2] = (uint8_t)(px >> 8); line[xx*2+1] = (uint8_t)(px & 0xFF);
            }
        } else {
            memcpy(line, src, (size_t)s_colon_bw * 2);
        }
        spi_transaction_t t = { .length = (size_t)(s_colon_bw * 2) * 8, .tx_buffer = line };
        spi_device_polling_transmit(spi_dev, &t);
    }
    deselect_all();
}

/* ════════════════════════════════════════════════════════════════════
 *  Display task – full mode renderer
 * ════════════════════════════════════════════════════════════════════ */
#include "config_mgr.h"
#include "ntp_time.h"
#include "weather.h"
#include "subscribers.h"
#include "microphone.h"
#include "freertos/semphr.h"

/* nextube_config_t is now visible (via config_mgr.h above).
 * Forward-declare WL helpers and state used by render_date and other early render funcs.
 * Definitions and explicit initialisers appear in the WeatherLive renderer section. */
static bool cx_is_wl_sky(const nextube_config_t *cfg);
static void wl_ensure_scene(const nextube_config_t *cfg);
static bool s_wl_scene_valid; /* tentative; defined with initialiser below */

/* ── Mode render helpers ────────────────────────────────────────────── */

/* Forward declaration: defined in the H/T helpers section below render_ap_pin. */
static uint16_t ht_sample_theme_color(const char *theme);

/* ── S1 — AP PIN renderer ────────────────────────────────────────────
 * Called from the display task whenever the setup AP is broadcasting and
 * no client is associated.  Shows the 8-digit PIN as a scrolling marquee
 * across the 6 tubes — the virtual tape is 3 blanks + 8 PIN digits (11
 * positions), advancing one step per second so all digits cycle past.
 *
 * Digits are rendered with the U8g2 logisoso42 embedded font — no theme
 * artwork required, so the PIN is always legible regardless of which theme
 * is active or whether any theme images have been cached yet.
 *
 * Colour is auto-sampled from the theme's Numbers/0.jpg centre pixel
 * (falls back to white 0xFFFF when the image cache is cold, e.g. first
 * boot before any theme image has been decoded).
 *
 * Defined here (after nextube_config_t becomes visible) rather than next
 * to display_show_number.
 */

/* Draw one tube for the AP-PIN marquee using U8g2 embedded font.
 * ch = '0'..'9' → digit centred on the full 80×160 tube.
 * Any other value → fills tube black (blank marquee gap).
 *
 * The U8g2 buffer (128×64) is blitted with 2× pixel scaling in both axes
 * so each source pixel maps to a 2×2 block on the physical tube.
 *
 * Layout (single 160-row SPI window per tube):
 *   Rows   0– 15  : black  (top margin, (160 - 64×2)/2 = 16 px)
 *   Rows  16–143  : U8g2 logisoso42 digit, 2× pixel-scaled (128 rows)
 *   Rows 144–159  : black  (bottom margin, 16 px)
 *
 * Horizontal: source columns 0..39 (40 px) → output columns 0..79 (80 px, 2×).
 * The digit is centred in the 40-column virtual space before scaling.
 */
static void pin_draw_tube(int tube, char ch, uint16_t fg)
{
    if (ch < '0' || ch > '9') {
        display_fill(tube, 0x0000);
        return;
    }

    /* Render single digit into shared U8g2 buffer (128×64 px, 1 bpp). */
    char str[2] = { ch, '\0' };
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFont(&s_u8g2, u8g2_font_logisoso42_tf);

    /* 2× pixel-scale factor: each U8g2 source pixel → 2×2 output pixels.
     * Horizontal: centre the glyph in LCD_WIDTH/SCALE = 40 virtual columns.
     * The blit loop doubles these to fill the 80-px physical tube width.   */
    const int SCALE = 2;
    u8g2_uint_t glyph_w = u8g2_GetUTF8Width(&s_u8g2, str);
    int x = ((int)(LCD_WIDTH / SCALE) - (int)glyph_w) / 2;
    if (x < 0) x = 0;

    /* Vertically centre digit cap height (ascent) in the 64-row buffer.
     * Digits have no descenders so we anchor on ascent alone:
     *   y_baseline = (BUF_H + ascent) / 2
     * e.g. logisoso42 ascent≈42 → y = (64+42)/2 = 53,
     *   glyph rows 11–53, centred at row 32 = 64/2.                     */
    const int BUF_H = 64;
    int ascent = (int)u8g2_GetAscent(&s_u8g2);
    int y = (BUF_H + ascent) / 2;
    if (y < ascent) y = ascent;
    if (y > BUF_H)  y = BUF_H;

    u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)x, (u8g2_uint_t)y, str);

    /* Get U8g2's internal 1-bpp tile buffer after the draw call.
     * The _f setup function allocates its own buffer via u8g2_m_16_8_f;
     * s_ht_buf no longer exists — this pointer is the canonical source. */
    const uint8_t *tile_buf = u8g2_GetBufferPtr(&s_u8g2);

    /* Apply per-tube brightness and gamma to fg once before the pixel loop
     * (bg is always 0x0000; scaling zero stays zero, no special case).   */
    {
        uint8_t  br     = s_tube_brightness[tube];
        bool     do_br  = (br < 100);
        bool     do_gam = s_gamma_lut_active[tube];
        if (do_br || do_gam) {
            uint32_t r5 = (fg >> 11) & 0x1Fu;
            uint32_t g6 = (fg >>  5) & 0x3Fu;
            uint32_t b5 =  fg        & 0x1Fu;
            if (do_br)  { r5=r5*br/100u; g6=g6*br/100u; b5=b5*br/100u; }
            if (do_gam) { r5=s_gamma_lut_5bit[tube][r5];
                          g6=s_gamma_lut_6bit[tube][g6];
                          b5=s_gamma_lut_5bit[tube][b5]; }
            fg = (uint16_t)((r5<<11)|(g6<<5)|b5);
        }
    }
    uint8_t fg_hi = (uint8_t)(fg >> 8);
    uint8_t fg_lo = (uint8_t)(fg & 0xFF);

    /* Open one full-tube SPI window and stream all 160 rows in
     * DISP_CHUNK_ROWS-row batches (same strategy as display_show_digit). */
    const int BUF_W  = 128;
    const int OUT_H  = BUF_H * SCALE;              /* 64 × 2 = 128 output rows */
    const int MARGIN = (LCD_HEIGHT - OUT_H) / 2;   /* (160 - 128) / 2 = 16 rows */

    select_tube(tube);
    uint8_t ox = (uint8_t)((int)LCD_OFFSET_X + (int)s_burnin_shift_x
                            + (int)s_col_offsets[tube]);
    uint8_t oy = (uint8_t)((int)LCD_OFFSET_Y + (int)s_row_offsets[tube]);
    open_lcd_window(ox, oy, (uint8_t)LCD_WIDTH, (uint8_t)LCD_HEIGHT);

    uint8_t chunk[LCD_WIDTH * 2 * DISP_CHUNK_ROWS];   /* 1280 B — SRAM stack */

    /* ── Top black margin (16 rows) ── */
    memset(chunk, 0, sizeof(chunk));
    for (int r = 0; r < MARGIN; r += DISP_CHUNK_ROWS) {
        int rows = (r + DISP_CHUNK_ROWS <= MARGIN) ? DISP_CHUNK_ROWS : MARGIN - r;
        spi_transaction_t t = { .length = (size_t)(rows * LCD_WIDTH * 2) * 8,
                                 .tx_buffer = chunk };
        spi_device_polling_transmit(spi_dev, &t);
    }

    /* ── Text region: 2× pixel-scaled blit (128 output rows) ──────────
     * Each output row maps to src_row = out_row / SCALE.
     * Each output column maps to src_col = out_col / SCALE.
     * Source cols 0..(LCD_WIDTH/SCALE - 1) = 0..39 → output cols 0..79. */
    for (int out_row = 0; out_row < OUT_H; out_row += DISP_CHUNK_ROWS) {
        int rows = (out_row + DISP_CHUNK_ROWS <= OUT_H) ? DISP_CHUNK_ROWS
                                                        : OUT_H - out_row;
        for (int r = 0; r < rows; r++) {
            int src_row  = (out_row + r) / SCALE;
            int tile_row = src_row / 8;
            int bit      = src_row % 8;
            for (int src_col = 0; src_col < LCD_WIDTH / SCALE; src_col++) {
                bool    lit     = (tile_buf[tile_row * BUF_W + src_col] >> bit) & 1;
                uint8_t hi      = lit ? fg_hi : 0x00;
                uint8_t lo      = lit ? fg_lo : 0x00;
                int     out_col = src_col * SCALE;
                chunk[(r * LCD_WIDTH + out_col    ) * 2]     = hi;
                chunk[(r * LCD_WIDTH + out_col    ) * 2 + 1] = lo;
                chunk[(r * LCD_WIDTH + out_col + 1) * 2]     = hi;
                chunk[(r * LCD_WIDTH + out_col + 1) * 2 + 1] = lo;
            }
        }
        spi_transaction_t t = { .length = (size_t)(rows * LCD_WIDTH * 2) * 8,
                                 .tx_buffer = chunk };
        spi_device_polling_transmit(spi_dev, &t);
    }

    /* ── Bottom black margin (16 rows) ── */
    memset(chunk, 0, sizeof(chunk));
    int bot = LCD_HEIGHT - MARGIN - OUT_H;   /* 160 - 16 - 128 = 16 rows */
    for (int r = 0; r < bot; r += DISP_CHUNK_ROWS) {
        int rows = (r + DISP_CHUNK_ROWS <= bot) ? DISP_CHUNK_ROWS : bot - r;
        spi_transaction_t t = { .length = (size_t)(rows * LCD_WIDTH * 2) * 8,
                                 .tx_buffer = chunk };
        spi_device_polling_transmit(spi_dev, &t);
    }

    deselect_all();
}

/* Marquee change-detection: the display task ticks at 5 Hz during the PIN
 * phase but the scroll position only advances at 1 Hz, so 4 of 5 ticks would
 * repaint identical content.  Reset to -1 by the AP-PIN exit guard so a
 * later re-entry (force-AP) repaints from scratch. */
static int      s_pin_last_scroll = -1;
static uint16_t s_pin_last_fg     = 0;

static void render_ap_pin(const nextube_config_t *cfg)
{
    const char *pin = wifi_manager_get_ap_pin();
    if (!pin || strlen(pin) < 8) return;

    /* Scrolling marquee: 3 blanks + 8 digits = 11-position tape, 1 step/s.
     *   scroll=0 → tubes: [ ][ ][ ][d0][d1][d2]
     *   scroll=3 → tubes: [d0][d1][d2][d3][d4][d5]
     *   scroll=8 → tubes: [d5][d6][d7][ ][ ][ ]    */
    const int seq_len = 11;
    const int step_ms = 1000;

    int64_t now_ms = esp_timer_get_time() / 1000;
    int     scroll  = (int)((now_ms / step_ms) % seq_len);

    /* Sample theme colour once; white fallback when cache is cold. */
    uint16_t fg = ht_sample_theme_color(cfg->theme);

    /* Skip the repaint when neither the scroll step nor the colour changed
     * since the last tick — saves 6 full-tube SPI pushes on 4 of 5 ticks. */
    if (scroll == s_pin_last_scroll && fg == s_pin_last_fg) return;
    s_pin_last_scroll = scroll;
    s_pin_last_fg     = fg;

    for (int tube = 0; tube < LCD_COUNT; tube++) {
        int  pos = (scroll + tube) % seq_len;
        char ch  = (pos < 3) ? '\0' : pin[pos - 3];
        pin_draw_tube(tube, ch, fg);
    }
}

/* Custom Clock: shows the current date as DD MM YY (or MM DD YY) across the
 * 6 tubes depending on the user's date_format setting.
 *   "DD/MM/YY" (default): [d1][d2][m1][m2][y1][y2]  e.g. 15 Mar 2026 → 150326
 *   "MM/DD/YY":           [m1][m2][d1][d2][y1][y2]  e.g. 15 Mar 2026 → 031526 */
static void render_date(const nextube_config_t *cfg, const struct tm *t)
{
    int d  = t->tm_mday;
    int mo = t->tm_mon + 1;
    int y  = t->tm_year % 100;
    int digits[6];
    if (strcmp(cfg->date_format, "MM/DD/YY") == 0) {
        digits[0] = mo/10; digits[1] = mo%10;
        digits[2] = d/10;  digits[3] = d%10;
    } else {
        digits[0] = d/10;  digits[1] = d%10;
        digits[2] = mo/10; digits[3] = mo%10;
    }
    digits[4] = y/10; digits[5] = y%10;
    bool wl = cx_is_wl_sky(cfg);
    if (wl) wl_ensure_scene(cfg);
    wl = wl && s_wl_scene_valid;
    for (int i = 0; i < 6; i++) {
        if (wl) {
            char ds[4]; snprintf(ds, sizeof(ds), "%d", digits[i]);
            wl_tube_str(i, u8g2_font_logisoso46_tf, ds, 100);
        } else {
            display_show_number(i, digits[i], cfg->theme);
        }
    }
}

/* ── H/T panel helpers (U8g2 embedded-font rendering for kind==2) ────────────
 * render_cx_tube6 kind==2 renders temperature and humidity as text using the
 * U8g2 virtual frame buffer (s_u8g2, 128×64, configured in
 * display_init).  The three helpers below handle colour sampling, pixel blitting,
 * and string rendering respectively.
 *
 * Tube half geometry:
 *   Physical half  : 80 × 80 px  (LCD_WIDTH × LCD_HEIGHT/2)
 *   U8g2 buffer    : 128 wide × 64 tall, 1 bpp column-major tiles
 *   Blit offset    : dst_y + 8 px → centres the 64-row block in each 80-row half
 */

/* Extract the theme's foreground text colour from its '1' digit image.
 *
 * Strategy:
 *   1. Sample the background brightness from the four corner pixels of the
 *      image (corners are reliably background on any digit glyph).
 *   2. If the background is BRIGHT  → return the DARKEST  pixel in the image
 *      (the text stroke on a light-background theme).
 *      If the background is DARK    → return the BRIGHTEST pixel in the image
 *      (the accent colour on a dark-background theme).
 *
 * This correctly handles both dark-bg/light-text and light-bg/dark-text themes
 * without any per-theme configuration.
 *
 * Luminance weights: 2r + g + 2b normalises RGB565's 5/6/5 bit depths so R,G,B
 * contribute roughly equally and no channel biases the bright/dark selection.
 *
 * Memoised by theme name — runs once per theme switch, not every render tick. */
/* Themes whose mid-tone background fools the bright/dark classifier.
 * Add entries here instead of fighting the heuristic.  Values are RGB565. */
static const struct { const char *theme; uint16_t color; } s_fg_overrides[] = {
    /* tan bg lum≈87 < threshold 93 → misclassified as dark; JPEG ringing picks white */
    { "RetroPaper", 0x30E2 },   /* dark ink brown #321C11 from Numbers/1.jpg */
};

static uint16_t ht_sample_theme_color(const char *theme)
{
    if (theme && theme[0] && strcmp(theme, s_theme_color_memo_theme) == 0)
        return s_theme_color_memo_color;

    for (size_t i = 0; i < sizeof(s_fg_overrides) / sizeof(s_fg_overrides[0]); i++) {
        if (theme && strcmp(theme, s_fg_overrides[i].theme) == 0)
            return s_fg_overrides[i].color;
    }

    char path[256];
    display_path_number(path, sizeof(path), theme, 1);
    int w = 0, h = 0;
    const uint8_t *px = img_cache_get(path, &w, &h);
    if (!px || w <= 0 || h <= 0)
        return 0xFFFF;   /* not cached yet — don't memoise, retry next tick */

    /* ── Step 1: background brightness from four corners ── */
    int corners[4] = {
        0,                          /* top-left     */
        (w - 1),                    /* top-right    */
        (h - 1) * w,                /* bottom-left  */
        (h - 1) * w + (w - 1),     /* bottom-right */
    };
    uint32_t bg_lum = 0;
    for (int i = 0; i < 4; i++) {
        int ci = corners[i];
        uint16_t c = ((uint16_t)px[ci * 2] << 8) | px[ci * 2 + 1];
        uint32_t r = (c >> 11) & 0x1Fu;
        uint32_t g = (c >>  5) & 0x3Fu;
        uint32_t b =  c        & 0x1Fu;
        bg_lum += (r << 1) + g + (b << 1);
    }
    bg_lum /= 4;   /* average corner luminance, 0..187 */

    /* Threshold: >93 (~50% of max) = bright background */
    bool bright_bg = (bg_lum > 93);

    /* ── Step 2: scan for brightest or darkest pixel ── */
    uint16_t best     = bright_bg ? 0xFFFF : 0x0000;
    uint32_t best_lum = bright_bg ? UINT32_MAX : 0;

    int n = w * h;
    for (int i = 0; i < n; i++) {
        uint16_t c = ((uint16_t)px[i * 2] << 8) | px[i * 2 + 1];
        uint32_t r = (c >> 11) & 0x1Fu;
        uint32_t g = (c >>  5) & 0x3Fu;
        uint32_t b =  c        & 0x1Fu;
        uint32_t lum = (r << 1) + g + (b << 1);
        if (bright_bg ? (lum < best_lum) : (lum > best_lum)) {
            best_lum = lum;
            best     = c;
        }
    }

    /* Sanity fallback: if the chosen colour is too close to the background
     * (near-identical luminance), the image is essentially monochrome —
     * fall back to white on dark or black on light. */
    uint16_t color;
    if (bright_bg)
        color = (best_lum > 80) ? 0x0000 : best;   /* darkest too light → black */
    else
        color = (best_lum < 8)  ? 0xFFFF : best;   /* brightest too dark → white */

    strncpy(s_theme_color_memo_theme, theme ? theme : "",
            sizeof(s_theme_color_memo_theme) - 1);
    s_theme_color_memo_theme[sizeof(s_theme_color_memo_theme) - 1] = '\0';
    s_theme_color_memo_color = color;
    return color;
}

/* Convert the U8g2 1-bpp tile buffer to RGB565 and push 64 rows to tube 5,
 * centred within the physical 80-row half (8 px top margin, 8 px bottom margin).
 *   dst_y  : 0 for top half, LCD_HEIGHT/2 (80) for bottom half
 *   fg     : RGB565 foreground colour; background is always black (0x0000)   */
static void ht_blit(int tube, const uint8_t *tile_buf, int dst_y, uint16_t fg)
{
    /* Apply per-tube brightness and gamma to fg once so the inner loop is
     * branch-free (bg is always 0x0000; brightness/gamma of 0 stays 0).    */
    {
        uint8_t  br     = s_tube_brightness[tube];
        bool     do_br  = (br < 100);
        bool     do_gam = s_gamma_lut_active[tube];
        if (do_br || do_gam) {
            uint32_t r = (fg >> 11) & 0x1Fu;
            uint32_t g = (fg >>  5) & 0x3Fu;
            uint32_t b =  fg        & 0x1Fu;
            if (do_br)  { r = r * br / 100u; g = g * br / 100u; b = b * br / 100u; }
            if (do_gam) { r = s_gamma_lut_5bit[tube][r];
                          g = s_gamma_lut_6bit[tube][g];
                          b = s_gamma_lut_5bit[tube][b]; }
            fg = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
    uint8_t fg_hi = (uint8_t)(fg >> 8);
    uint8_t fg_lo = (uint8_t)(fg & 0xFF);

    /* Centre the 64-row buffer in the 80-row physical half: 8 px top margin. */
    const int BUF_H = 64;
    const int BUF_W = 128;
    int y_start = dst_y + (LCD_HEIGHT / 2 - BUF_H) / 2;   /* dst_y + 8 */

    select_tube(tube);
    uint8_t ox = (uint8_t)((int)LCD_OFFSET_X + (int)s_burnin_shift_x
                            + (int)s_col_offsets[tube]);
    uint8_t oy = (uint8_t)((int)LCD_OFFSET_Y + (int)s_row_offsets[tube] + y_start);
    open_lcd_window(ox, oy, (uint8_t)LCD_WIDTH, (uint8_t)BUF_H);

    /* U8g2 tile layout: byte[tile_row * BUF_W + col] stores 8 vertical pixel
     * rows; bit 0 is the topmost row of that tile.  We only read the leftmost
     * LCD_WIDTH (80) columns — the extra 48 cols of the 128-wide buffer are
     * ignored (off-screen on the tube).                                       */
    uint8_t line[LCD_WIDTH * 2];
    for (int row = 0; row < BUF_H; row++) {
        int tile_row = row / 8;
        int bit      = row % 8;
        for (int col = 0; col < LCD_WIDTH; col++) {
            bool lit = (tile_buf[tile_row * BUF_W + col] >> bit) & 1;
            line[col * 2]     = lit ? fg_hi : 0x00;
            line[col * 2 + 1] = lit ? fg_lo : 0x00;
        }
        spi_transaction_t t = { .length = sizeof(line) * 8, .tx_buffer = line };
        spi_device_polling_transmit(spi_dev, &t);
    }
    deselect_all();
}

/* Like ht_blit() but blits exactly `rows` rows of the U8g2 1-bpp buffer to
 * tube at absolute tube row y_tube, without adding any centring margin.
 * Used by the label and region-text helpers that need arbitrary y positions.
 *
 * bg_rgb565: optional decoded RGB565 background image (LCD_WIDTH × LCD_HEIGHT,
 *            big-endian, as returned by img_cache_get).  When non-NULL each
 *            U8g2 zero-bit pixel is replaced by the corresponding background
 *            pixel from the source image rather than solid black.  Pass NULL
 *            for a solid-black background (original behaviour).              */
static void ht_blit_at(int tube, const uint8_t *tile_buf, int rows, int y_tube,
                        uint16_t fg, const uint8_t *bg_rgb565)
{
    /* Clamp to the physical tube height.  Without this, a blit that starts
     * near the bottom (e.g. HALF+23=103 with rows=64 → 167 > LCD_HEIGHT=160)
     * would (a) read past the end of the 80×160×2-byte bg_rgb565 buffer and
     * (b) send extra pixel data to the ST7735 after its window closes, which
     * some panel variants wrap to a visible row, producing a phantom colour bar. */
    if (y_tube >= LCD_HEIGHT) return;
    if (rows > LCD_HEIGHT - y_tube) rows = LCD_HEIGHT - y_tube;

    {
        uint8_t  br     = s_tube_brightness[tube];
        bool     do_br  = (br < 100);
        bool     do_gam = s_gamma_lut_active[tube];
        if (do_br || do_gam) {
            uint32_t r = (fg >> 11) & 0x1Fu;
            uint32_t g = (fg >>  5) & 0x3Fu;
            uint32_t b =  fg        & 0x1Fu;
            if (do_br)  { r = r * br / 100u; g = g * br / 100u; b = b * br / 100u; }
            if (do_gam) { r = s_gamma_lut_5bit[tube][r];
                          g = s_gamma_lut_6bit[tube][g];
                          b = s_gamma_lut_5bit[tube][b]; }
            fg = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
    uint8_t fg_hi = (uint8_t)(fg >> 8);
    uint8_t fg_lo = (uint8_t)(fg & 0xFF);
    const int BUF_W = 128;
    select_tube(tube);
    uint8_t ox = (uint8_t)((int)LCD_OFFSET_X + (int)s_burnin_shift_x
                            + (int)s_col_offsets[tube]);
    uint8_t oy = (uint8_t)((int)LCD_OFFSET_Y + (int)s_row_offsets[tube] + y_tube);
    open_lcd_window(ox, oy, (uint8_t)LCD_WIDTH, (uint8_t)rows);
    uint8_t line[LCD_WIDTH * 2];
    for (int row = 0; row < rows; row++) {
        int tile_row = row / 8;
        int bit      = row % 8;
        for (int col = 0; col < LCD_WIDTH; col++) {
            bool lit = (tile_buf[tile_row * BUF_W + col] >> bit) & 1;
            if (lit) {
                line[col * 2]     = fg_hi;
                line[col * 2 + 1] = fg_lo;
            } else if (bg_rgb565) {
                /* Sample the corresponding pixel from the background image.
                 * Layout: big-endian RGB565, stride = LCD_WIDTH pixels.     */
                int bg_idx = ((y_tube + row) * LCD_WIDTH + col) * 2;
                line[col * 2]     = bg_rgb565[bg_idx];
                line[col * 2 + 1] = bg_rgb565[bg_idx + 1];
            } else {
                line[col * 2]     = 0x00;
                line[col * 2 + 1] = 0x00;
            }
        }
        spi_transaction_t t = { .length = sizeof(line) * 8, .tx_buffer = line };
        spi_device_polling_transmit(spi_dev, &t);
    }
    deselect_all();
}

/* Height (rows) reserved for an "In" / "Out" label rendered with
 * u8g2_font_logisoso20_tf.  logisoso20: ascent=20, |descent|=4 → 24 rows.
 * Must match the blit_h computed inside ht_draw_label().                    */
#define HT_LABEL_H  24

/* Render a short label string (e.g. "In" / "Out") centred horizontally using
 * u8g2_font_logisoso20_tf (ascent=20, descent=−4, total glyph height=24 px).
 * y_tube: absolute tube row where the top of the label should appear.
 * bg: optional RGB565 background buffer (see ht_blit_at); NULL = solid black.*/
static void ht_draw_label(int tube, const char *str, int y_tube, uint16_t fg,
                           const uint8_t *bg)
{
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFont(&s_u8g2, u8g2_font_logisoso20_tf);
    int ascent  = (int)u8g2_GetAscent(&s_u8g2);
    int descent = (int)u8g2_GetDescent(&s_u8g2);   /* negative */
    int blit_h  = ascent - descent;                 /* = HT_LABEL_H */
    /* UTF-8 width/draw so localised labels with Latin-1 glyphs (e.g. the
     * Finnish "Sisä") render correctly — DrawStr would treat each byte as a
     * separate Latin-1 glyph and mangle multi-byte UTF-8 sequences. */
    u8g2_uint_t w = u8g2_GetUTF8Width(&s_u8g2, str);
    int x = ((int)LCD_WIDTH - (int)w) / 2;
    if (x < 0) x = 0;
    /* Place baseline at `ascent` so glyphs start at buffer row 0.           */
    u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)x, (u8g2_uint_t)ascent, str);
    ht_blit_at(tube, u8g2_GetBufferPtr(&s_u8g2), blit_h, y_tube, fg, bg);
}

/* Render a UTF-8 string centred horizontally (within LCD_WIDTH=80 px) and
 * vertically within a band of `height` rows placed at absolute tube row y_tube.
 * font: pointer to any compiled-in U8g2 font constant.
 * Blits at most 64 rows (the U8g2 buffer height limit).
 * bg: optional RGB565 background buffer (see ht_blit_at); NULL = solid black.*/
static void ht_draw_str_at(int tube, const char *str, int y_tube, int height,
                            const uint8_t *font, uint16_t fg,
                            const uint8_t *bg)
{
    const int BUF_H = 64;
    int blit_h = (height < BUF_H) ? height : BUF_H;

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFont(&s_u8g2, font);

    u8g2_uint_t str_w = u8g2_GetUTF8Width(&s_u8g2, str);
    int x = ((int)LCD_WIDTH - (int)str_w) / 2;
    if (x < 0) x = 0;

    int ascent  = (int)u8g2_GetAscent(&s_u8g2);
    int descent = (int)u8g2_GetDescent(&s_u8g2);   /* negative */
    /* Vertical centre within blit_h (same formula as ht_draw_str). */
    int y = (blit_h + ascent + descent) / 2;
    if (y < ascent) y = ascent;
    if (y > BUF_H)  y = BUF_H;

    u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)x, (u8g2_uint_t)y, str);
    ht_blit_at(tube, u8g2_GetBufferPtr(&s_u8g2), blit_h, y_tube, fg, bg);
}

/* ── Localised weekday abbreviation (tube-6 WEEKDATE panel) ───────────────
 * Returns a short (≤3-char) day-of-week label for the given ISO 639-1 language
 * code and tm_wday (0=Sunday … 6=Saturday).  Every glyph is ASCII or Latin-1
 * Supplement (é á í ì ç å ö ä ø æ), which u8g2_font_logisoso28_tf (_tf = full
 * glyph set, 0x20–0xFF) renders correctly through the UTF-8-aware
 * ht_draw_str_at().  Abbreviations are kept ≤3 chars so they fit the 80-px tube
 * at logisoso28.  Unknown or empty language codes fall back to English. */
static const char *weekday_abbrev(const char *lang, int wday)
{
    if (wday < 0 || wday > 6) wday = 1;   /* defensive: default to Monday */

    /* Index order matches tm_wday: 0=Sun, 1=Mon, … 6=Sat. */
    static const char *const en[7] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    static const char *const de[7] = { "So","Mo","Di","Mi","Do","Fr","Sa" };
    static const char *const fr[7] = { "Dim","Lun","Mar","Mer","Jeu","Ven","Sam" };
    static const char *const es[7] = { "Dom","Lun","Mar","Mié","Jue","Vie","Sáb" };
    static const char *const it[7] = { "Dom","Lun","Mar","Mer","Gio","Ven","Sab" };
    static const char *const pt[7] = { "Dom","Seg","Ter","Qua","Qui","Sex","Sáb" };
    static const char *const nl[7] = { "Zo","Ma","Di","Wo","Do","Vr","Za" };
    static const char *const sv[7] = { "Sön","Mån","Tis","Ons","Tor","Fre","Lör" };
    static const char *const no[7] = { "Søn","Man","Tir","Ons","Tor","Fre","Lør" };
    static const char *const da[7] = { "Søn","Man","Tir","Ons","Tor","Fre","Lør" };
    static const char *const fi[7] = { "Su","Ma","Ti","Ke","To","Pe","La" };

    if (lang && lang[0]) {
        if      (!strcmp(lang, "de")) return de[wday];
        else if (!strcmp(lang, "fr")) return fr[wday];
        else if (!strcmp(lang, "es")) return es[wday];
        else if (!strcmp(lang, "it")) return it[wday];
        else if (!strcmp(lang, "pt")) return pt[wday];
        else if (!strcmp(lang, "nl")) return nl[wday];
        else if (!strcmp(lang, "sv")) return sv[wday];
        else if (!strcmp(lang, "no")) return no[wday];
        else if (!strcmp(lang, "da")) return da[wday];
        else if (!strcmp(lang, "fi")) return fi[wday];
    }
    return en[wday];
}

/* ── Localised month abbreviation (tube-6 WeatherLive date panel) ─────────
 * 3-char month label for the given language code and tm_mon (0=Jan … 11=Dec).
 * Only ASCII and the accents already used by weekday_abbrev (ä é) appear, so
 * logisoso28_tf renders them; anything riskier (û) is spelled ASCII (Aou).
 * Unknown/empty language falls back to English. */
static const char *month_abbrev(const char *lang, int mon)
{
    if (mon < 0 || mon > 11) mon = 0;

    static const char *const en[12] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
    static const char *const de[12] = { "Jan","Feb","Mär","Apr","Mai","Jun","Jul","Aug","Sep","Okt","Nov","Dez" };
    static const char *const fr[12] = { "Jan","Fév","Mar","Avr","Mai","Jun","Jul","Aou","Sep","Oct","Nov","Déc" };
    static const char *const es[12] = { "Ene","Feb","Mar","Abr","May","Jun","Jul","Ago","Sep","Oct","Nov","Dic" };
    static const char *const it[12] = { "Gen","Feb","Mar","Apr","Mag","Giu","Lug","Ago","Set","Ott","Nov","Dic" };
    static const char *const pt[12] = { "Jan","Fev","Mar","Abr","Mai","Jun","Jul","Ago","Set","Out","Nov","Dez" };
    static const char *const nl[12] = { "Jan","Feb","Mrt","Apr","Mei","Jun","Jul","Aug","Sep","Okt","Nov","Dec" };
    static const char *const sv[12] = { "Jan","Feb","Mar","Apr","Maj","Jun","Jul","Aug","Sep","Okt","Nov","Dec" };
    static const char *const no[12] = { "Jan","Feb","Mar","Apr","Mai","Jun","Jul","Aug","Sep","Okt","Nov","Des" };
    static const char *const da[12] = { "Jan","Feb","Mar","Apr","Maj","Jun","Jul","Aug","Sep","Okt","Nov","Dec" };
    static const char *const fi[12] = { "Tam","Hel","Maa","Huh","Tou","Kes","Hei","Elo","Syy","Lok","Mar","Jou" };

    if (lang && lang[0]) {
        if      (!strcmp(lang, "de")) return de[mon];
        else if (!strcmp(lang, "fr")) return fr[mon];
        else if (!strcmp(lang, "es")) return es[mon];
        else if (!strcmp(lang, "it")) return it[mon];
        else if (!strcmp(lang, "pt")) return pt[mon];
        else if (!strcmp(lang, "nl")) return nl[mon];
        else if (!strcmp(lang, "sv")) return sv[mon];
        else if (!strcmp(lang, "no")) return no[mon];
        else if (!strcmp(lang, "da")) return da[mon];
        else if (!strcmp(lang, "fi")) return fi[mon];
    }
    return en[mon];
}

/* ── Localised "Indoor"/"Outdoor" label (tube-6 H/T panels) ───────────────
 * Short label drawn above the indoor / outdoor temperature+humidity panels.
 * Kept ≤4 chars so it fits the 80-px tube at logisoso20.  Rendered through
 * ht_draw_label()'s UTF-8 path, so Latin-1 glyphs (ä) display correctly.
 * indoor=true → "In"-style, false → "Out"-style.  Unknown/empty language
 * falls back to English.  ß is deliberately avoided (spelled "ss"). */
static const char *inout_label(const char *lang, bool indoor)
{
    if (lang && lang[0]) {
        if      (!strcmp(lang, "de")) return indoor ? "Inn"  : "Auss";
        else if (!strcmp(lang, "fr")) return indoor ? "Int"  : "Ext";
        else if (!strcmp(lang, "es")) return indoor ? "Int"  : "Ext";
        else if (!strcmp(lang, "it")) return indoor ? "Int"  : "Est";
        else if (!strcmp(lang, "pt")) return indoor ? "Int"  : "Ext";
        else if (!strcmp(lang, "nl")) return indoor ? "Bin"  : "Bui";
        else if (!strcmp(lang, "sv")) return indoor ? "Inne" : "Ute";
        else if (!strcmp(lang, "no")) return indoor ? "Inne" : "Ute";
        else if (!strcmp(lang, "da")) return indoor ? "Inde" : "Ude";
        else if (!strcmp(lang, "fi")) return indoor ? "Sisä" : "Ulko";
    }
    return indoor ? "In" : "Out";
}

/* Render a UTF-8 string into the U8g2 buffer using the 28-px
 * logisoso font, centred horizontally (within LCD_WIDTH=80) and vertically
 * (within the 64-row buffer), then blit to tube 5 at the given half offset.  */
static void ht_draw_str(const char *str, int dst_y, uint16_t fg)
{
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFont(&s_u8g2, u8g2_font_logisoso28_tf);

    /* Horizontal centre: measure with the UTF-8-aware width function so that
     * multi-byte characters (° = 0xC2 0xB0) are counted as one glyph.       */
    u8g2_uint_t str_w = u8g2_GetUTF8Width(&s_u8g2, str);
    int x = ((int)LCD_WIDTH - (int)str_w) / 2;
    if (x < 0) x = 0;

    /* Vertical centre: place the baseline so the glyph span [ascent..descent]
     * is centred in the 64-row buffer.
     *   baseline = (BUF_H + ascent + descent) / 2
     * For logisoso28: ascent≈28, descent≈-7 → baseline ≈ (64+28-7)/2 = 42.
     * Glyph span: top≈14, bottom≈49, centre≈31.5 ≈ 32 = 64/2.              */
    const int BUF_H = 64;
    int ascent  = (int)u8g2_GetAscent(&s_u8g2);
    int descent = (int)u8g2_GetDescent(&s_u8g2);   /* negative value */
    int y = (BUF_H + ascent + descent) / 2;
    if (y < ascent) y = ascent;   /* clamp: don't draw above buffer top */
    if (y > BUF_H)  y = BUF_H;   /* clamp: don't draw below buffer bottom */

    u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)x, (u8g2_uint_t)y, str);
    /* u8g2_SendBuffer is intentionally omitted: byte_cb is a no-op.
     * Read the rendered pixels directly from U8g2's internal buffer. */
    ht_blit(5, u8g2_GetBufferPtr(&s_u8g2), dst_y, fg);
}

/* ── NOAA solar calculator ───────────────────────────────────────────────────
 * Computes local sunrise and sunset as minutes past midnight (0–1439).
 * lat_deg / lon_deg: WGS-84 decimal degrees (N/E positive).
 * t: local struct tm for the desired date; uses tm_year/mon/mday/gmtoff.
 * Returns -1 for both outputs on polar day/night (sun never crosses horizon). */
static void solar_calc(float lat_deg, float lon_deg, const struct tm *t,
                       int *rise_min, int *set_min)
{
    int Y = t->tm_year + 1900, M = t->tm_mon + 1, D = t->tm_mday;
    /* Gregorian-to-JDN: months Jan/Feb treated as 13/14 of the previous year. */
    if (M <= 2) { Y--; M += 12; }
    int A = Y / 100;
    double JD = (int)(365.25*(Y+4716)) + (int)(30.6001*(M+1)) + D
              + (2 - A + A/4) - 1524.5;
    double JC  = (JD - 2451545.0) / 36525.0;
    double L0  = fmod(280.46646 + JC*(36000.76983 + JC*0.0003032), 360.0);
    double M0  = 357.52911 + JC*(35999.05029 - 0.0001537*JC);
    double Mr  = M0 * M_PI / 180.0;
    double e   = 0.016708634 - JC*(0.000042037 + 0.0000001267*JC);
    double C   = sin(Mr)*(1.914602 - JC*(0.004817 + 0.000014*JC))
               + sin(2*Mr)*(0.019993 - 0.000101*JC) + sin(3*Mr)*0.000289;
    double om  = 125.04 - 1934.136*JC;
    double lam = (L0 + C) - 0.00569 - 0.00478*sin(om*M_PI/180.0);
    double eps = (23.0 + (26.0 + (21.448 - JC*(46.815 + JC*(0.00059 - JC*0.001813)))/60.0)/60.0)
               + 0.00256*cos(om*M_PI/180.0);
    double decl = asin(sin(eps*M_PI/180.0)*sin(lam*M_PI/180.0));
    double yy   = tan((eps/2.0)*M_PI/180.0); yy *= yy;
    double L0r  = L0*M_PI/180.0, M0r = M0*M_PI/180.0;
    double eot  = 4.0*180.0/M_PI*(yy*sin(2*L0r) - 2*e*sin(M0r)
                + 4*e*yy*sin(M0r)*cos(2*L0r) - 0.5*yy*yy*sin(4*L0r)
                - 1.25*e*e*sin(2*M0r));
    double latr  = lat_deg * M_PI / 180.0;
    double cosHA = cos(90.833*M_PI/180.0) / (cos(latr)*cos(decl))
                 - tan(latr)*tan(decl);
    if (cosHA < -1.0 || cosHA > 1.0) { *rise_min = -1; *set_min = -1; return; }
    double HA   = acos(cosHA) * 180.0 / M_PI;
    double noon = 720.0 - 4.0*lon_deg - eot;
    /* UTC offset in minutes (east = positive), computed portably without
     * tm_gmtoff (GNU/BSD) or the 'timezone' global (not exported by ESP-IDF
     * newlib).  We compare localtime and gmtime for the same instant:
     *   tz_m = (local_hour*60 + local_min) - (utc_hour*60 + utc_min)
     *          + day_diff * 1440
     * tm_yday avoids month-boundary issues; multiplying by tm_year*365
     * handles the single edge case of a UTC offset spanning a year end.    */
    {
        time_t ts = time(NULL);
        struct tm ltm, utm;
        localtime_r(&ts, &ltm);
        gmtime_r(&ts, &utm);
        int day_diff = (ltm.tm_yday + ltm.tm_year * 365)
                     - (utm.tm_yday + utm.tm_year * 365);
        double tz_m = (double)(day_diff * 1440
                               + (ltm.tm_hour - utm.tm_hour) * 60
                               + (ltm.tm_min  - utm.tm_min));
        *rise_min = (int)(noon - HA*4.0 + tz_m + 0.5);
        *set_min  = (int)(noon + HA*4.0 + tz_m + 0.5);
    }
}

/* ── ht_draw_suntime ─────────────────────────────────────────────────────────
 * Renders one half of the Sunrise/Sunset panel onto tube 5 using U8g2 primitives.
 *
 * Layout (within the 128×64 U8g2 frame buffer, LCD_WIDTH=80 columns used):
 *   Rows  0–31  : sun icon — semicircle (radius 9, centred at x=40, y=16)
 *                 above a horizon line, three rays, and a direction caret:
 *                   rising=true  → ^ caret below horizon (sunrise)
 *                   rising=false → v caret below horizon (sunset)
 *   Rows 30–55  : "HH:MM" time string — logisoso20_tf, baseline at y=50,
 *                 centred in 80-px width.
 *
 * y_tube: absolute tube row for the top of the blit (56 rows written).
 * fg: RGB565 foreground colour (from ht_sample_theme_color).
 * bg: optional decoded RGB565 background image (LCD_WIDTH × LCD_HEIGHT);
 *     NULL = solid black background for "off" pixels.                          */
static void ht_draw_suntime(int tube, const char *timestr, bool rising,
                             int y_tube, uint16_t fg, const uint8_t *bg)
{
    u8g2_ClearBuffer(&s_u8g2);

    /* ── Sun icon — centred at (40, 16) ─────────────────────────────── */
    const int cx = 40, cy = 16, r = 9;

    /* Filled upper semicircle */
    u8g2_DrawDisc(&s_u8g2, (u8g2_uint_t)cx, (u8g2_uint_t)cy, (u8g2_uint_t)r,
                  U8G2_DRAW_UPPER_RIGHT | U8G2_DRAW_UPPER_LEFT);
    /* Horizon line */
    u8g2_DrawHLine(&s_u8g2, (u8g2_uint_t)(cx - r - 3), (u8g2_uint_t)cy,
                   (u8g2_uint_t)((r + 3) * 2 + 1));
    /* Three rays above the arc */
    u8g2_DrawLine(&s_u8g2, (u8g2_uint_t)cx,       (u8g2_uint_t)(cy-r-2),
                            (u8g2_uint_t)cx,       (u8g2_uint_t)(cy-r-5));  /* straight up  */
    u8g2_DrawLine(&s_u8g2, (u8g2_uint_t)(cx-r-1), (u8g2_uint_t)(cy-2),
                            (u8g2_uint_t)(cx-r-4), (u8g2_uint_t)(cy-5));    /* upper-left   */
    u8g2_DrawLine(&s_u8g2, (u8g2_uint_t)(cx+r+1), (u8g2_uint_t)(cy-2),
                            (u8g2_uint_t)(cx+r+4), (u8g2_uint_t)(cy-5));    /* upper-right  */
    /* Direction caret just below the horizon */
    if (rising) {
        /* ^ pointing up = sunrise */
        u8g2_DrawLine(&s_u8g2, (u8g2_uint_t)(cx-4), (u8g2_uint_t)(cy+5),
                                (u8g2_uint_t)cx,     (u8g2_uint_t)(cy+2));
        u8g2_DrawLine(&s_u8g2, (u8g2_uint_t)(cx+4), (u8g2_uint_t)(cy+5),
                                (u8g2_uint_t)cx,     (u8g2_uint_t)(cy+2));
    } else {
        /* v pointing down = sunset */
        u8g2_DrawLine(&s_u8g2, (u8g2_uint_t)(cx-4), (u8g2_uint_t)(cy+2),
                                (u8g2_uint_t)cx,     (u8g2_uint_t)(cy+5));
        u8g2_DrawLine(&s_u8g2, (u8g2_uint_t)(cx+4), (u8g2_uint_t)(cy+2),
                                (u8g2_uint_t)cx,     (u8g2_uint_t)(cy+5));
    }

    /* ── Time string — logisoso20, centred in 80 px, baseline at row 50 ── */
    u8g2_SetFont(&s_u8g2, u8g2_font_logisoso20_tf);
    u8g2_uint_t tw = u8g2_GetStrWidth(&s_u8g2, timestr);
    int tx = ((int)LCD_WIDTH - (int)tw) / 2;
    if (tx < 0) tx = 0;
    u8g2_DrawStr(&s_u8g2, (u8g2_uint_t)tx, 50, timestr);

    /* Blit 56 rows (0-55): captures icon (top ~y=2) and text (baseline y=50,
     * descent to y=54).  ht_blit_at clamps if y_tube+56 > LCD_HEIGHT.        */
    ht_blit_at(tube, u8g2_GetBufferPtr(&s_u8g2), 56, y_tube, fg, bg);
}

/* ── cx6_stamp_update_indicator ──────────────────────────────────────────────
 * Panels rendered via ht_blit_at (weekdate, H/T, sunrise/sunset) bypass
 * display_show_digit(), so they never trigger the 4-row red stripe that
 * display_show_digit() applies automatically.  Worse, the H/T humidity blit
 * and the weekdate date blit both extend to row 159, overwriting whatever
 * display_show_image() had drawn there.
 * Call this once after ALL blits for tube 5 are complete to re-stamp the
 * indicator when s_update_indicator is active.  No-op when inactive.          */
static void cx6_stamp_update_indicator(int tube)
{
    if (!s_update_indicator) return;
    select_tube(tube);
    uint8_t ox = (uint8_t)((int)LCD_OFFSET_X + (int)s_burnin_shift_x
                            + (int)s_col_offsets[tube]);
    uint8_t oy = (uint8_t)((int)LCD_OFFSET_Y + (int)s_row_offsets[tube]);
    open_lcd_window(ox, (uint8_t)(oy + LCD_HEIGHT - 4), (uint8_t)LCD_WIDTH, 4);
    uint8_t redline[LCD_WIDTH * 2];
    for (int x = 0; x < LCD_WIDTH; x++) { redline[x*2] = 0xF8; redline[x*2+1] = 0x00; }
    for (int row = 0; row < 4; row++) {
        spi_transaction_t tr = { .length = sizeof(redline) * 8, .tx_buffer = redline };
        spi_device_polling_transmit(spi_dev, &tr);
    }
    deselect_all();
}

/* ── render_cx_tube6 ─────────────────────────────────────────────────────────
 * Render the current 24H-Custom info panel onto tube 5 (the rightmost tube).
 * Each panel occupies the full 80×160 display by compositing two 80×80 halves:
 *
 *   WEATHER  — Full tube (80×160) : current weather condition icon JPEG.
 *              Falls back to black when weather API has no data.
 *
 *   WEEKDATE — Rows   0– 79 : day name — "Sun" … "Sat"
 *                            U8g2 logisoso28, centred in 64-row band (rows 8–71)
 *                            composited over AMPM/blank.jpg background.
 *              Rows  80–159 : date "DDMM" or "MMDD" (no separator; follows date_format)
 *                            U8g2 logisoso28, centred in 64-row band (rows 88–151)
 *                            composited over AMPM/blank.jpg background.
 *              Colour auto-sampled from Numbers/0.jpg centre pixel.
 *
 *   INDOOR   — Rows  10– 33 : "In" label   (logisoso20, HT_LABEL_H=24 px, +10 shift)
 *              Rows  34– 89 : indoor temperature   (logisoso28, 56-px band)
 *              Rows  90–153 : indoor humidity       (logisoso28, centred in 64-px blit)
 *              Colour auto-sampled from the theme's Numbers/0.jpg centre pixel.
 *
 *   OUTDOOR H/T — Rows  10– 33 : "Out" label  (logisoso20, HT_LABEL_H=24 px, +10 shift)
 *              Rows  34– 89 : outdoor temperature  (logisoso28, 56-px band)
 *              Rows  90–153 : outdoor humidity     (logisoso28, centred in 64-px blit)
 *              Colour auto-sampled from Numbers/0.jpg centre pixel.
 *              Falls back to black when weather API has no data.
 *
 * panel_id is an index into the ordered list [weather, weekdate, ht, temp,
 * sunrise, push]; the caller resolves which concrete panel this maps to.       */
/* Render one 24H-Custom info panel onto `lcd_tube` (5 = rightmost, 4 = the
 * 2nd-from-right in dual mode).  `enabled[8]` is the caller's panel set
 * (tube6_panel_* or tube5_panel_*); `panel_id` indexes the enabled entries;
 * `*last_kind` tracks the previously drawn kind for that tube so backgrounds
 * are cleared only on a panel switch. */
/* Defined later (with the WeatherLive renderer): the shared 80×160 scratch
 * framebuffer and the temperature-panel drawing, reused here so the asset-theme
 * Outdoor Temperature panel renders identically to the WeatherLive one. */
static uint8_t *wl_fb(void);
static void wl_temp_panel(uint8_t *fb, int temp_disp, bool range_ok,
                          int dmin_disp, int dmax_disp);
static void wl_humidity_panel(uint8_t *fb, int hum);
static void wl_wind_panel(uint8_t *fb, int wind_kph, const char *unit, int r, int g, int b);

/* Per-frame render state set by render_weatherlive at the top of each frame.
 * Defaults reflect legacy WeatherLive behaviour (white glyphs, dark shadow). */
static bool    s_wl_is_custom  = false;
static uint8_t s_wl_glyph_r = 255, s_wl_glyph_g = 255, s_wl_glyph_b = 255;
static uint8_t s_wl_font_r  = 255, s_wl_font_g  = 255, s_wl_font_b  = 255;
static bool    s_wl_shadow   = true;
static uint8_t s_wl_shadow_r = 0,  s_wl_shadow_g = 0,  s_wl_shadow_b = 0;
static char    s_wl_bg_theme[32] = "";
static char    s_wl_bg_png_cached[256] = ""; /* path of last successfully decoded PNG bg */
static uint8_t *s_wl_bg_png_buf        = NULL; /* RGB565 cache in PSRAM for custom PNG bg */
/* FreeType face id for the active custom digit font; -1 = no custom font (u8g2 logisoso). */
static int s_ft_face_id = -1;

/* Desired cap height (pixels) for full-tube clock-digit glyphs.
 * Passed as px_size to fr_draw_glyph_centered; the font_render layer
 * calibrates each loaded face so '0' renders at exactly this height. */
#define WL_FT_BIG_PX  FR_DIGIT_CAP_PX

/* Map a u8g2 font pointer to a target pixel-height for FreeType when wl_text()
 * is routed through font_render.  fr_draw_text applies norm_ratio on top, so
 * passing the u8g2 nominal size here produces the same rendered cap-height as
 * the logisoso bitmap font — the norm_ratio correction handles decorative fonts
 * (e.g. Jim Nightshade) automatically. */
static uint16_t ft_px_for_u8g2(const uint8_t *font)
{
    if (font == u8g2_font_logisoso42_tf) return 42;
    if (font == u8g2_font_logisoso28_tf) return 28;
    if (font == u8g2_font_logisoso24_tf) return 24;
    if (font == u8g2_font_logisoso20_tf) return 20;
    if (font == u8g2_font_logisoso16_tf) return 16;
    return 28;
}

/* Load or switch the active TTF face; flushes the glyph cache on face change. */
static void wl_refresh_ft_face(const char *custom_font)
{
    if (custom_font && custom_font[0]) {
        char path[128];
        snprintf(path, sizeof(path), "/spiffs/fonts/%s", custom_font);
        int id = fr_load_face(path);
        if (id != s_ft_face_id) {
            fr_cache_flush();
            s_ft_face_id = id;
        }
    } else {
        if (s_ft_face_id >= 0) {
            fr_cache_flush();
            fr_unload_face(s_ft_face_id);
        }
        s_ft_face_id = -1;
    }
}

/* Forward declarations for WeatherLive types/functions needed by render_cx_panel,
 * which is defined above the full WeatherLive implementation block. */
typedef struct wl_scene_s wl_scene_t;
static uint16_t wl_rgb565(int r, int g, int b);
static void wl_paint_background(uint8_t *fb, int tube, const wl_scene_t *sc);
static void wl_tube_sky(int tube);
static void wl_tube_str(int tube, const uint8_t *font, const char *str, int by);

/* Cached scene from the last render_weatherlive() frame — lets render_cx_panel
 * repaint the live sky as the CX-panel background. Written once per frame by
 * render_weatherlive(). */
static wl_scene_t s_wl_last_scene;
static bool       s_wl_scene_valid = false;

/* Cached colon ON/OFF framebuffers for the static-custom-face fast blink path.
 * Populated by wl_render_colon_tube(); pushed by wl_show_colon_blink().        */
static uint8_t *s_wl_colon_on_buf      = NULL;
static uint8_t *s_wl_colon_off_buf     = NULL;
static int16_t  s_wl_colon_bx0        = 0, s_wl_colon_by0 = 0;
static int16_t  s_wl_colon_bw         = 0, s_wl_colon_bh  = 0;
static bool     s_wl_colon_cache_valid = false;

/* Returns true when the active clock face uses the WeatherLive animated sky as
 * its background, so CX panels can substitute the sky for blank.jpg / black. */
static bool cx_is_wl_sky(const nextube_config_t *cfg)
{
    /* True for WeatherLive theme and for the Custom clockface.
     * Custom always uses the WL renderer; its background may be a static
     * theme image (via s_wl_bg_theme) rather than the animated sky.        */
    return (strncmp(cfg->theme, "WeatherLive", 11) == 0) ||
           (strcmp(cfg->clock_face, "custom") == 0);
}

/* Seed a raw framebuffer: paint WL sky or copy/clear blank.jpg into fb. */
static void seed_fb_blank(uint8_t *fb, const char *path)
{
    int bw, bh;
    const uint8_t *br = img_cache_get(path, &bw, &bh);
    if (br && bw == LCD_WIDTH && bh == LCD_HEIGHT)
        memcpy(fb, br, (size_t)LCD_WIDTH * LCD_HEIGHT * 2);
    else
        memset(fb, 0, (size_t)LCD_WIDTH * LCD_HEIGHT * 2);
}

static void cx_seed_framebuf(uint8_t *fb, int tube, const nextube_config_t *cfg)
{
    if (cx_is_wl_sky(cfg) && s_wl_scene_valid) {
        wl_paint_background(fb, tube, &s_wl_last_scene);
    } else {
        char bg_path[256];
        snprintf(bg_path, sizeof(bg_path), "/images/themes/%s/AMPM/blank.jpg", cfg->theme);
        seed_fb_blank(fb, bg_path);
    }
}

/* Load text-panel background: returns pointer usable as anti-alias bg for
 * ht_draw_* calls. Returns NULL on failure; caller must handle the empty tube. */
static const uint8_t *cx_load_text_bg(int tube, const nextube_config_t *cfg)
{
    if (cx_is_wl_sky(cfg) && s_wl_scene_valid) {
        uint8_t *fb = wl_fb();
        if (fb) {
            wl_paint_background(fb, tube, &s_wl_last_scene);
            display_show_digit(tube, fb, LCD_WIDTH, LCD_HEIGHT);
            return fb;
        }
        return NULL;
    }
    char bg_path[256];
    snprintf(bg_path, sizeof(bg_path), "/images/themes/%s/AMPM/blank.jpg", cfg->theme);
    int bw = 0, bh = 0;
    const uint8_t *bg = img_cache_get(bg_path, &bw, &bh);
    if (bw != LCD_WIDTH || bh != LCD_HEIGHT) return NULL;
    display_show_image(tube, bg_path);
    return bg;
}

/* Unpack RGB565 colour to 8-bit channels. */
static void rgb565_to_rgb8(uint16_t c, int *r, int *g, int *b)
{
    *r = ((c >> 11) & 31) * 255 / 31;
    *g = ((c >>  5) & 63) * 255 / 63;
    *b = ( c        & 31) * 255 / 31;
}

/* Foreground colour as separate 8-bit RGB channels (used by wl_*_panel helpers). */
static void cx_fg_rgb8(const nextube_config_t *cfg, int *r, int *g, int *b)
{
    if (cx_is_wl_sky(cfg) && s_wl_scene_valid) {
        *r = s_wl_font_r; *g = s_wl_font_g; *b = s_wl_font_b;
    } else {
        rgb565_to_rgb8(ht_sample_theme_color(cfg->theme), r, g, b);
    }
}

/* Convert Celsius to the display unit. */
static float to_display_temp(float celsius, bool use_fahrenheit)
{
    return use_fahrenheit ? celsius * 9.0f / 5.0f + 32.0f : celsius;
}

static void render_cx_panel(const nextube_config_t *cfg, const struct tm *t,
                             int lcd_tube, const bool enabled[8],
                             uint8_t panel_id, int8_t *last_kind)
{
    /* Resolve panel_id → concrete panel kind by walking the caller's enabled[].
     * Order: 0=weather, 1=weekdate, 2=indoor H/T, 3=outdoor temp+Hi/Lo,
     * 4=sunrise, 5=externally-pushed image, 6=outdoor humidity, 7=wind. */
    int kind = -1;
    int count = 0;
    for (int i = 0; i < 8; i++) {
        if (enabled[i]) {
            if (count == (int)panel_id) { kind = i; break; }
            count++;
        }
    }
    if (kind < 0) kind = 1;   /* fallback: weekdate */

    const int HALF = LCD_HEIGHT / 2;   /* 80 */
    const int push_idx = (lcd_tube == 5) ? 1 : 0;   /* tube6=LCD5→1, tube5=LCD4→0 */

    /* Weather-dependent panels (0 = icon, 3 = outdoor temp+Hi/Lo, 6 = humidity,
     * 7 = wind) with no data yet — cold boot: the first fetch takes 10 s to
     * minutes — render the weekdate panel in their slot instead of a black tube.
     * Day + date only need the RTC-seeded clock, valid from the first tick.
     * Self-healing: once weather_get() turns valid, the real panel renders
     * on its next rotation (and *last_kind tracking forces the bg clear). */
    if (kind == 0 || kind == 3 || kind == 6 || kind == 7) {
        const weather_data_t *w = weather_get();
        if (!w || !w->valid) kind = 1;
    }
    /* Pushed-image panel with nothing pushed yet → show weekdate until the first
     * /api/cx_image arrives, so an enabled-but-empty tube isn't a black slot. */
    if (kind == 5 && !s_cx_push_valid[push_idx]) kind = 1;

    if (kind == 0) {
        /* ── Weather icon panel ─────────────────────────────────────────────
         * Displays the current weather condition icon full-tube (80×160).   */
        const weather_data_t *w = weather_get();
        {
            const char *icon = (w && w->icon[0] != '\0') ? w->icon : "sun";
            char path[256];
            display_path_weather(path, sizeof(path), cfg->theme, icon);
            display_show_image(lcd_tube, path);
        }

    } else if (kind == 1) {
        /* ── Week/Date panel — three stacked U8g2 lines over blank.jpg ───────
         * Mirrors the WeatherLive weekdate layout:
         *   line 1 (rows   2– 52) : weekday abbrev — "Sun" … "Sat"
         *   line 2 (rows  55–105) : day-of-month OR localised month abbrev
         *   line 3 (rows 108–158) : the other of the two
         * The day/month stack ORDER follows Network › Date format:
         *   "MM/DD/YY" (US)   → month over day
         *   "DD/MM/YY" (intl) → day over month
         * All three lines use logisoso28.  Background: theme's AMPM/blank.jpg
         * (rows between bands keep the full theme image).  Colour: auto-sampled
         * from Numbers/0.jpg.  Fallback: solid black fill if blank.jpg absent. */

        const uint8_t *bg = cx_load_text_bg(lcd_tube, cfg);
        if (!bg && kind != *last_kind) display_fill(lcd_tube, 0x0000);
        uint16_t fg = (cx_is_wl_sky(cfg) && s_wl_scene_valid)
                      ? wl_rgb565(s_wl_font_r, s_wl_font_g, s_wl_font_b)
                      : ht_sample_theme_color(cfg->theme);

        const char *day = weekday_abbrev(cfg->language, t->tm_wday);
        const char *mon = month_abbrev(cfg->language, t->tm_mon);
        char dd[12];
        snprintf(dd, sizeof(dd), "%02d", t->tm_mday);

        /* US date format → month over day; international → day over month. */
        bool us_fmt = (strcmp(cfg->date_format, "MM/DD/YY") == 0);
        const char *line2 = us_fmt ? mon : dd;
        const char *line3 = us_fmt ? dd  : mon;

        ht_draw_str_at(lcd_tube, day,   2,  50, u8g2_font_logisoso28_tf, fg, bg);
        ht_draw_str_at(lcd_tube, line2, 55, 50, u8g2_font_logisoso28_tf, fg, bg);
        ht_draw_str_at(lcd_tube, line3, 108, 50, u8g2_font_logisoso28_tf, fg, bg);

    } else if (kind == 2) {
        /* ── Indoor H/T panel — U8g2 embedded font ──────────────────────── */
        /* Rows  10– 33 : "In" label   (logisoso20, HT_LABEL_H=24 rows, +10 shift)
         * Rows  34– 89 : indoor temperature  (logisoso28, 56-row band)
         * Rows  90–153 : indoor humidity     (logisoso28, centred in 64-px blit)
         * Colour auto-sampled from the theme's Numbers/0.jpg centre pixel.
         * The 60 *-sm.jpg symbol files previously required by this panel are
         * no longer needed and have been removed from the filesystem image.  */
        const sht30_reading_t *s = sht30_get();
        if (!s || !s->valid) {
            if (kind != *last_kind) display_fill(lcd_tube, 0x0000);
            goto cx_tube6_done;
        }

        const uint8_t *bg = cx_load_text_bg(lcd_tube, cfg);
        if (!bg) display_fill(lcd_tube, 0x0000);
        uint16_t fg = (cx_is_wl_sky(cfg) && s_wl_scene_valid)
                      ? wl_rgb565(s_wl_font_r, s_wl_font_g, s_wl_font_b)
                      : ht_sample_theme_color(cfg->theme);

        ht_draw_label(lcd_tube, inout_label(cfg->language, true), 18, fg, bg);

        {
            bool use_f = (strcmp(cfg->temp_format, "Fahrenheit") == 0);
            int  temp  = (int)lroundf(to_display_temp(s->temp_c, use_f));
            if (temp >  99) temp =  99;
            if (temp < -99) temp = -99;
            char buf[16];
            snprintf(buf, sizeof(buf), "%d\xc2\xb0%s", temp, use_f ? "F" : "C");
            ht_draw_str_at(lcd_tube, buf, HT_LABEL_H + 18, 56, u8g2_font_logisoso28_tf, fg, bg);
        }

        {
            int hum = (int)(s->humidity + 0.5f);
            if (hum > 99) hum = 99;
            if (hum <  0) hum = 0;
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", hum);
            ht_draw_str_at(lcd_tube, buf, HALF + 18, 64, u8g2_font_logisoso28_tf, fg, bg);
        }

    } else if (kind == 3) {
        /* ── Outdoor temperature panel — identical to the WeatherLive TEMP panel
         * (current temp + degree ring over today's Hi/Lo range track + marker +
         * lo/hi numbers), rendered into the shared framebuffer over the theme's
         * AMPM/blank.jpg background via the SAME wl_temp_panel() drawing code.
         * Falls back to black when weather API has no valid data.            */
        const weather_data_t *ow = weather_get();
        if (!ow || !ow->valid) {
            if (kind != *last_kind) display_fill(lcd_tube, 0x0000);
            goto cx_tube6_done;
        }
        {
            uint8_t *fb = wl_fb();
            if (!fb) { if (kind != *last_kind) display_fill(lcd_tube, 0x0000); goto cx_tube6_done; }

            cx_seed_framebuf(fb, lcd_tube, cfg);

            bool use_f     = (strcmp(cfg->temp_format, "Fahrenheit") == 0);
            int  temp_disp = (int)lroundf(to_display_temp(ow->temp_c,    use_f));
            int  dmin_disp = (int)lroundf(to_display_temp(ow->day_min_c, use_f));
            int  dmax_disp = (int)lroundf(to_display_temp(ow->day_max_c, use_f));

            wl_temp_panel(fb, temp_disp, ow->day_range_valid, dmin_disp, dmax_disp);
            display_show_digit(lcd_tube, fb, LCD_WIDTH, LCD_HEIGHT);
        }

    } else if (kind == 4) {
        /* ── Sunrise + Sunset combined panel ────────────────────────────────
         * Top half    : sunrise icon + local rise time "HH:MM"
         * Bottom half : sunset  icon + local set  time "HH:MM"
         * Solar times via NOAA algorithm from geocoded lat/lon; falls back to
         * "--:--" until the weather task has resolved the configured city.
         * Background: AMPM/blank.jpg.  Colour: theme's Numbers/0 centre px.  */
        float lat = 0.0f, lon = 0.0f;
        bool have_loc = weather_get_location(&lat, &lon);

        {
            const uint8_t *bg = cx_load_text_bg(lcd_tube, cfg);
            if (!bg) display_fill(lcd_tube, 0x0000);
            uint16_t fg = (cx_is_wl_sky(cfg) && s_wl_scene_valid)
                          ? wl_rgb565(s_wl_font_r, s_wl_font_g, s_wl_font_b)
                          : ht_sample_theme_color(cfg->theme);

            char rise_str[8] = "--:--";
            char set_str[8]  = "--:--";
            if (have_loc) {
                int rise_min = 0, set_min = 0;
                solar_calc(lat, lon, t, &rise_min, &set_min);
                if (rise_min >= 0)
                    snprintf(rise_str, sizeof(rise_str), "%02d:%02d",
                             (rise_min / 60) % 24, rise_min % 60);
                if (set_min >= 0)
                    snprintf(set_str, sizeof(set_str), "%02d:%02d",
                             (set_min / 60) % 24, set_min % 60);
            }

            /* Top half: sunrise (y_tube=14), bottom half: sunset (y_tube=HALF+14) */
            ht_draw_suntime(lcd_tube, rise_str, /*rising=*/true,  14,        fg, bg);
            ht_draw_suntime(lcd_tube, set_str,  /*rising=*/false, HALF + 14, fg, bg);
        }

    } else if (kind == 5) {
        /* ── Externally-pushed image (POST /api/cx_image?tube=5|6) ───────────
         * On WeatherLive sky face: composite the logo over the sky by keying out
         * near-black pixels (typical social-media icon black background).
         * On asset themes: blit the decoded 80×160 RGB565 push buffer directly. */
        if (cx_is_wl_sky(cfg) && s_wl_scene_valid) {
            uint8_t *fb = wl_fb();
            if (fb) {
                wl_paint_background(fb, lcd_tube, &s_wl_last_scene);
                if (s_cx_push_mutex) xSemaphoreTake(s_cx_push_mutex, portMAX_DELAY);
                if (s_cx_push_valid[push_idx] && s_cx_push_buf[push_idx]) {
                    const uint16_t *src = (const uint16_t *)s_cx_push_buf[push_idx];
                    uint16_t *dst = (uint16_t *)fb;
                    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
                        uint16_t px = src[i];
                        int r5 = (px >> 11) & 31, g6 = (px >> 5) & 63, b5 = px & 31;
                        if ((r5 + (g6 >> 1) + b5) > 8) dst[i] = px;
                    }
                }
                if (s_cx_push_mutex) xSemaphoreGive(s_cx_push_mutex);
                display_show_digit(lcd_tube, fb, LCD_WIDTH, LCD_HEIGHT);
            } else {
                if (s_cx_push_mutex) xSemaphoreTake(s_cx_push_mutex, portMAX_DELAY);
                if (s_cx_push_valid[push_idx] && s_cx_push_buf[push_idx])
                    display_show_digit(lcd_tube, s_cx_push_buf[push_idx], LCD_WIDTH, LCD_HEIGHT);
                else display_fill(lcd_tube, 0x0000);
                if (s_cx_push_mutex) xSemaphoreGive(s_cx_push_mutex);
            }
        } else {
            if (s_cx_push_mutex) xSemaphoreTake(s_cx_push_mutex, portMAX_DELAY);
            if (s_cx_push_valid[push_idx] && s_cx_push_buf[push_idx])
                display_show_digit(lcd_tube, s_cx_push_buf[push_idx], LCD_WIDTH, LCD_HEIGHT);
            else
                display_fill(lcd_tube, 0x0000);
            if (s_cx_push_mutex) xSemaphoreGive(s_cx_push_mutex);
        }

    } else if (kind == 6) {
        /* ── Outdoor humidity panel — identical to the WeatherLive HUMIDITY panel
         * (water-drop symbol with "%" over the value), rendered into the shared
         * framebuffer over the theme's AMPM/blank.jpg via the SAME
         * wl_humidity_panel() drawing code. */
        const weather_data_t *ow = weather_get();   /* validity ensured by guard above */
        uint8_t *fb = wl_fb();
        if (!fb) { if (kind != *last_kind) display_fill(lcd_tube, 0x0000); goto cx_tube6_done; }

        cx_seed_framebuf(fb, lcd_tube, cfg);

        int fg_r, fg_g, fg_b;
        cx_fg_rgb8(cfg, &fg_r, &fg_g, &fg_b);
        wl_humidity_panel(fb, ow ? (int)lroundf(ow->humidity) : 0);
        display_show_digit(lcd_tube, fb, LCD_WIDTH, LCD_HEIGHT);

    } else if (kind == 7) {
        /* ── Wind panel — identical to the WeatherLive WIND panel (wind symbol
         * over the speed value), rendered into the shared framebuffer over the
         * theme's AMPM/blank.jpg via the SAME wl_wind_panel() drawing code. */
        const weather_data_t *ow = weather_get();   /* validity ensured by guard above */
        uint8_t *fb = wl_fb();
        if (!fb) { if (kind != *last_kind) display_fill(lcd_tube, 0x0000); goto cx_tube6_done; }

        cx_seed_framebuf(fb, lcd_tube, cfg);

        int fg_r, fg_g, fg_b;
        cx_fg_rgb8(cfg, &fg_r, &fg_g, &fg_b);
        wl_wind_panel(fb, ow ? (int)lroundf(ow->wind_kph) : 0, cfg->wind_unit, fg_r, fg_g, fg_b);
        display_show_digit(lcd_tube, fb, LCD_WIDTH, LCD_HEIGHT);
    }

    *last_kind = (int8_t)kind;   /* record which panel was just drawn */

cx_tube6_done:
    /* Re-stamp the update indicator after every panel render.
     * ht_blit_at paths overwrite the bottom rows; display_fill misses it
     * entirely.  cx6_stamp_update_indicator() is a no-op when inactive. */
    cx6_stamp_update_indicator(lcd_tube);
}

/* ──────────────────────────────────────────────────────────────────────────
 * WeatherLive — procedural animated weather theme
 *
 * Unlike the asset themes (folders of JPEGs), WeatherLive draws everything from
 * primitives into an 80×160 RGB565 framebuffer per tube, then hands it to
 * display_show_digit() (which copies from PSRAM, applies per-tube gamma /
 * brightness, and streams it).  No image files are used.
 *
 * Slice 1 (this commit): vertical sky gradient behind the HH:MM clock, with
 * each glyph composited on top and outlined for legibility against the bright
 * background.  This proves the compositing path and the 6-tube redraw budget.
 * Later slices add the animated panorama (sun/clouds/rain/snow, day-night),
 * the tube-6 day-date / temp-range panels, and forecast-driven sky colours.
 * ────────────────────────────────────────────────────────────────────────── */

static inline uint16_t wl_rgb565(int r, int g, int b)
{
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Physical panorama geometry.  Active glass = 80 px wide ≈ 10.9 mm on the
 * 0.96" 80×160 panels (pitch ≈ 0.136 mm/px).  Between two lit areas there is
 * 1.5 mm of inactive border on each panel plus 13 mm of inter-panel spacing →
 * ~16 mm dead ≈ 118 px.  The sun/moon arc is laid out on a virtual canvas that
 * includes these gaps and is only drawn where it lands on glass, so the disc
 * arcs across the real device width and disappears into the gaps as it would
 * physically.  Lower WL_GAP_PX to visually tighten the gaps if the disc spends
 * too long hidden for your taste. */
#define WL_PITCH_MM_PER_PX  0.136f
#define WL_GAP_MM           (1.5f + 13.0f + 1.5f)
#define WL_GAP_PX           ((int)(WL_GAP_MM / WL_PITCH_MM_PER_PX + 0.5f))  /* ≈118 */
#define WL_TUBE_STRIDE      (LCD_WIDTH + WL_GAP_PX)   /* active + dead per tube */

/* Stylised weather scene shared across the six tubes (a left→right panorama).
 * Computed once per render in render_weatherlive() and handed to each tube. */
typedef struct wl_scene_s {
    int   tr, tg, tb;   /* sky colour at the top     */
    int   hr, hg, hb;   /* sky colour at the horizon */
    bool  body_show;    /* draw a sun/moon disc?     */
    bool  body_is_moon; /* true = render lunar phase shape; false = solid sun   */
    float moon_term;    /* terminator param = cos(2π·phase)                    */
    bool  moon_waxing;  /* bright limb on the right (true) or left (false)      */
    int   body_x;       /* disc centre X across the whole 6-tube panorama (px) */
    int   body_y;       /* disc centre Y (row 0..159)                          */
    int   body_r;       /* disc radius (px)                                    */
    int   br, bg, bb;   /* disc core colour                                    */
    float anim_t;       /* continuous animation clock (seconds)                */
    int   ncloud;       /* number of drifting cloud clusters (0 = clear)       */
    int   cr, cg, cb;   /* cloud colour                                        */
    int   ca;           /* cloud peak opacity (0..255)                         */
    int   precip;       /* 0 none · 1 rain · 2 snow                            */
    float wind;         /* normalised wind 0..1 (drives drift + rain slant)    */
    float flash;        /* lightning flash intensity this frame 0..1           */
    int   night;        /* night-sky darkness 0..255 (drives star visibility)  */
} wl_scene_t;

static void wl_lerp3(const int a[3], const int b[3], int t /*0..255*/, int out[3])
{
    for (int i = 0; i < 3; i++) out[i] = a[i] + (b[i] - a[i]) * t / 255;
}

/* Sky palette driven by the real geocoded sunrise/sunset (minutes-of-day).
 * Warm dawn/dusk peak AT sunrise/sunset; a ±TW twilight window blends to/from
 * night and day.  Full day naturally holds between sunrise+TW and sunset−TW, so
 * the "day" length tracks the season/location (long in summer, short in winter)
 * instead of fixed clock hours.  Fills top + horizon RGB. */
static void wl_sky_palette(int mins, int sunrise, int sunset, int top[3], int hor[3])
{
    static const int NIGHT[3]   = {  6,  10,  28}, H_NIGHT[3] = { 18,  22,  50};
    static const int DAWN[3]    = { 70,  70, 120}, H_DAWN[3]  = {245, 150,  95};
    static const int DAY[3]     = { 58, 120, 190}, H_DAY[3]   = {175, 208, 240};
    static const int DUSK[3]    = { 55,  45,  95}, H_DUSK[3]  = {250, 120,  70};
    const int TW = 55;   /* twilight half-window (minutes) around sun events */

    int srA = sunrise - TW, srB = sunrise + TW;
    int ssA = sunset  - TW, ssB = sunset  + TW;

    if (mins < srA || mins >= ssB) {                       /* night */
        wl_lerp3(NIGHT, NIGHT, 0, top); wl_lerp3(H_NIGHT, H_NIGHT, 0, hor);
    } else if (mins < sunrise) {                           /* night → dawn */
        int t = (mins - srA) * 255 / (sunrise - srA);
        wl_lerp3(NIGHT, DAWN, t, top); wl_lerp3(H_NIGHT, H_DAWN, t, hor);
    } else if (mins < srB) {                               /* dawn → day */
        int t = (mins - sunrise) * 255 / (srB - sunrise);
        wl_lerp3(DAWN, DAY, t, top); wl_lerp3(H_DAWN, H_DAY, t, hor);
    } else if (mins < ssA) {                               /* full day (held) */
        wl_lerp3(DAY, DAY, 0, top); wl_lerp3(H_DAY, H_DAY, 0, hor);
    } else if (mins < sunset) {                            /* day → dusk */
        int t = (mins - ssA) * 255 / (sunset - ssA);
        wl_lerp3(DAY, DUSK, t, top); wl_lerp3(H_DAY, H_DUSK, t, hor);
    } else {                                               /* dusk → night */
        int t = (mins - sunset) * 255 / (ssB - sunset);
        wl_lerp3(DUSK, NIGHT, t, top); wl_lerp3(H_DUSK, H_NIGHT, t, hor);
    }
}

/* Moon phase as a fraction of the synodic cycle: 0 = new, 0.25 = first quarter,
 * 0.5 = full, 0.75 = last quarter.  Date-based low precision (ignores timezone),
 * which is ample for a stylised moon shape. */
static float wl_moon_phase(const struct tm *t)
{
    int Y = t->tm_year + 1900, M = t->tm_mon + 1, D = t->tm_mday;
    if (M <= 2) { Y -= 1; M += 12; }
    int A = Y / 100, B = 2 - A + A / 4;             /* Gregorian correction */
    double jd = (double)(int)(365.25 * (Y + 4716))
              + (double)(int)(30.6001 * (M + 1))
              + D + B - 1524.5
              + (t->tm_hour - 12) / 24.0 + t->tm_min / 1440.0;
    double ph = fmod((jd - 2451550.1) / 29.530588853, 1.0);  /* since a known new moon */
    if (ph < 0) ph += 1.0;
    return (float)ph;
}

/* Alpha-blend an RGB colour over an existing big-endian RGB565 pixel. */
static inline void wl_blend_px(uint8_t *px, int r, int g, int b, int a /*0..255*/)
{
    if (a <= 0) return;
    if (a > 255) a = 255;
    uint16_t bgc = ((uint16_t)px[0] << 8) | px[1];
    int R5 = (bgc >> 11) & 0x1F, G6 = (bgc >> 5) & 0x3F, B5 = bgc & 0x1F;
    R5 += ((r >> 3) - R5) * a >> 8;
    G6 += ((g >> 2) - G6) * a >> 8;
    B5 += ((b >> 3) - B5) * a >> 8;
    uint16_t c = (uint16_t)((R5 << 11) | (G6 << 5) | B5);
    px[0] = (uint8_t)(c >> 8); px[1] = (uint8_t)(c & 0xFF);
}

/* Soft-edged filled disc (a cloud puff): opacity peaks at the centre and fades
 * to zero at the radius.  Clipped to the 80×160 framebuffer. */
static void wl_cloud_lump(uint8_t *fb, int cx, int cy, int R,
                          int r, int g, int b, int peak_a)
{
    if (R <= 0) return;
    int ro = R * R;
    for (int y = cy - R; y <= cy + R; y++) {
        if (y < 0 || y >= LCD_HEIGHT) continue;
        uint8_t *row = fb + y * LCD_WIDTH * 2;
        for (int x = cx - R; x <= cx + R; x++) {
            if (x < 0 || x >= LCD_WIDTH) continue;
            int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
            if (d2 >= ro) continue;
            wl_blend_px(row + x * 2, r, g, b, peak_a * (ro - d2) / ro);
        }
    }
}

/* Precipitation particle field, in gap-aware panorama coords (every particle is
 * spawned on a panel, never in a gap).  Updated once per frame in
 * render_weatherlive(); drawn per-tube in wl_draw_tube(). */
#define WL_NPART 56
static struct { float x, y, vy; int8_t drift; } s_wl_part[WL_NPART];
static bool    s_wl_part_init = false;
static int64_t s_wl_last_us   = 0;

/* Shared 80×160 RGB565 scratch framebuffer (big-endian byte pairs). */
static uint8_t *s_wl_fb = NULL;
static uint8_t *wl_fb(void)
{
    if (!s_wl_fb) s_wl_fb = PSRAM_MALLOC(LCD_WIDTH * LCD_HEIGHT * 2);
    return s_wl_fb;
}

/* Cached sky gradient (another 80×160 PSRAM slab).
 * The dither pattern is (x%4, y%4) — identical for every tube since all tubes
 * have x∈[0,79].  We bake the gradient once and memcpy it into each tube's fb,
 * recomputing only when the six sky-colour channels actually change (at most once
 * per minute during dawn/dusk transitions). */
static uint8_t *s_wl_sky_cache = NULL;
static int s_wl_sky_key[6]; /* tr,tg,tb,hr,hg,hb from the last bake */

/* Integer avalanche hash (lowbias32) — strong bit mixing so independent inputs
 * yield uncorrelated outputs.  Used to scatter the stars; slicing one linear
 * hash for both X and Y produced visible row banding. */
static inline unsigned wl_hash(unsigned x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/* Paint the animated sky panorama (gradient + sun/moon disc + drifting clouds +
 * precipitation) for one tube into its framebuffer.  Shared by the digit tubes
 * and the tube-6 info panel. */
static void wl_paint_background(uint8_t *fb, int tube, const wl_scene_t *sc)
{
    /* Custom background: load static blank image from the chosen theme (PNG preferred, JPG fallback). */
    if (s_wl_bg_theme[0] != '\0' && strncmp(s_wl_bg_theme, "WeatherLive", 11) != 0) {
        char bg_path[256];
        snprintf(bg_path, sizeof(bg_path), "/images/themes/%s/AMPM/blank.png", s_wl_bg_theme);
        if (spiffs_file_exists(bg_path)) {
            /* Decode once into PSRAM cache; reuse on subsequent calls. */
            if (strcmp(s_wl_bg_png_cached, bg_path) != 0) {
                s_wl_bg_png_cached[0] = '\0';
                if (!s_wl_bg_png_buf)
                    s_wl_bg_png_buf = PSRAM_MALLOC(LCD_WIDTH * LCD_HEIGHT * 2);
                if (s_wl_bg_png_buf) {
                    unsigned char *rgba = NULL;
                    unsigned pw = 0, ph = 0;
                    char _sbg[270]; snprintf(_sbg, sizeof(_sbg), "/spiffs%s", bg_path);
                    if (lodepng_decode32_file(&rgba, &pw, &ph, _sbg) == 0
                            && pw == (unsigned)LCD_WIDTH && ph == (unsigned)LCD_HEIGHT) {
                        uint8_t *dst8 = s_wl_bg_png_buf;
                        const unsigned char *src = rgba;
                        for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++, src += 4) {
                            uint16_t px = ((uint16_t)(src[0] >> 3) << 11)
                                        | ((uint16_t)(src[1] >> 2) << 5)
                                        |  (uint16_t)(src[2] >> 3);
                            dst8[i * 2]     = (uint8_t)(px >> 8);
                            dst8[i * 2 + 1] = (uint8_t)(px & 0xFF);
                        }
                        strncpy(s_wl_bg_png_cached, bg_path, sizeof(s_wl_bg_png_cached) - 1);
                        s_wl_bg_png_cached[sizeof(s_wl_bg_png_cached) - 1] = '\0';
                    } else {
                        ESP_LOGE(TAG, "PNG load failed: %s (pw=%u ph=%u)", bg_path, pw, ph);
                    }
                    free(rgba);
                }
            }
            if (s_wl_bg_png_cached[0] != '\0') {
                memcpy(fb, s_wl_bg_png_buf, (size_t)LCD_WIDTH * LCD_HEIGHT * 2);
                return;
            }
            /* PNG present but decode failed — fall through to JPEG. */
        }
        snprintf(bg_path, sizeof(bg_path), "/images/themes/%s/AMPM/blank.jpg", s_wl_bg_theme);
        int bw = 0, bh = 0;
        const uint8_t *bg = img_cache_get(bg_path, &bw, &bh);
        if (bg && bw == LCD_WIDTH && bh == LCD_HEIGHT) {
            memcpy(fb, bg, (size_t)LCD_WIDTH * LCD_HEIGHT * 2);
            return;
        }
        /* Fall through to procedural sky if neither PNG nor JPEG available. */
    }
    /* 1. Vertical sky gradient with ordered (Bayer 4×4) dithering.
     *
     * Without dithering the gradient is computed once per ROW and the 8-bit
     * value is hard-quantised to RGB565 (5/6/5 bits).  At night the whole sky
     * spans a tiny colour range (top {6,10,28} → horizon {18,22,50}), so many
     * adjacent rows collapse to the SAME RGB565 value — the top and bottom
     * extremes become flat ~10–20 px bands that read as a "mask" over the sky.
     * A static per-pixel Bayer offset (±½ quantisation step) spreads the
     * crossing so the bands dissolve into a smooth dither.  Static in (x,y) →
     * no frame-to-frame shimmer. */
    /* Sky gradient — computed once and cached; all 6 tubes share an identical
     * dither pattern (x%4, y%4 with x∈[0,79] for every tube), so the result
     * only changes when the six colour channels do (at most once per minute). */
    static const uint8_t WL_BAYER4[4][4] = {
        {  0,  8,  2, 10 }, { 12,  4, 14,  6 },
        {  3, 11,  1,  9 }, { 15,  7, 13,  5 },
    };
    int sky_key[6] = { sc->tr, sc->tg, sc->tb, sc->hr, sc->hg, sc->hb };
    bool sky_dirty = !s_wl_sky_cache
        || sky_key[0] != s_wl_sky_key[0] || sky_key[1] != s_wl_sky_key[1]
        || sky_key[2] != s_wl_sky_key[2] || sky_key[3] != s_wl_sky_key[3]
        || sky_key[4] != s_wl_sky_key[4] || sky_key[5] != s_wl_sky_key[5];
    if (sky_dirty) {
        if (!s_wl_sky_cache)
            s_wl_sky_cache = PSRAM_MALLOC(LCD_WIDTH * LCD_HEIGHT * 2);
        uint8_t *dst = s_wl_sky_cache ? s_wl_sky_cache : fb;
        for (int y = 0; y < LCD_HEIGHT; y++) {
            int r = sc->tr + (sc->hr - sc->tr) * y / (LCD_HEIGHT - 1);
            int g = sc->tg + (sc->hg - sc->tg) * y / (LCD_HEIGHT - 1);
            int b = sc->tb + (sc->hb - sc->tb) * y / (LCD_HEIGHT - 1);
            const uint8_t *bz = WL_BAYER4[y & 3];
            uint8_t *row = dst + y * LCD_WIDTH * 2;
            for (int x = 0; x < LCD_WIDTH; x++) {
                int d  = (int)bz[x & 3] - 8;
                int rr = r + (d >> 1); if (rr < 0) rr = 0; else if (rr > 255) rr = 255;
                int gg = g + (d >> 2); if (gg < 0) gg = 0; else if (gg > 255) gg = 255;
                int bb = b + (d >> 1); if (bb < 0) bb = 0; else if (bb > 255) bb = 255;
                uint16_t c = wl_rgb565(rr, gg, bb);
                row[x * 2] = (uint8_t)(c >> 8); row[x * 2 + 1] = (uint8_t)(c & 0xFF);
            }
        }
        if (s_wl_sky_cache)
            memcpy(s_wl_sky_key, sky_key, sizeof(s_wl_sky_key));
    }
    if (s_wl_sky_cache)
        memcpy(fb, s_wl_sky_cache, (size_t)LCD_WIDTH * LCD_HEIGHT * 2);

    /* 1b. Twinkling stars — fixed pseudo-random points in the upper sky that
     *     fade in through dusk and out through dawn (sc->night), each pulsing on
     *     its own phase.  Positions are panorama-absolute (hashed from the star
     *     index) so a star holds its place across the inter-tube gaps.  Drawn
     *     before the moon/clouds so those occlude it naturally. */
    if (sc->night > 0) {
        const int PANO_W = LCD_COUNT * LCD_WIDTH + (LCD_COUNT - 1) * WL_GAP_PX;
        const int NSTAR  = 90;
        for (int i = 0; i < NSTAR; i++) {
            /* Independent, well-mixed hashes for X, Y and per-star attributes so
             * the field scatters freely instead of banding into rows. */
            unsigned hx = wl_hash((unsigned)i * 2u + 1u);
            unsigned hy = wl_hash((unsigned)i * 2u + 2u);
            unsigned ha = wl_hash((unsigned)i + 0x9e3779b9u);
            int sxp = (int)(hx % (unsigned)PANO_W);
            int sy  = 2 + (int)(hy % 92u);                 /* rows 2..93 (upper sky) */
            int sx  = sxp - tube * WL_TUBE_STRIDE;
            if (sx < 0 || sx >= LCD_WIDTH) continue;
            /* Gentle in-place twinkle: each star stays lit and only modulates its
             * brightness on its own slow phase + rate, so it shimmers rather than
             * blinking fully on/off (which reads as flicker / shooting streaks).
             * tw stays in ~[0.40, 1.0] — never reaches zero. */
            float ph   = (float)(ha & 0x3FFu) * 0.006136f;            /* 0..2π   */
            float rate = 0.9f + (float)((ha >> 10) & 7u) * 0.17f;     /* 0.9..2.1 rad/s */
            float tw   = 0.70f + 0.30f * sinf(sc->anim_t * rate + ph);
            int base = 130 + (int)((ha >> 13) % 126u);     /* 130..255 per-star peak */
            int a    = (int)((float)(base * sc->night / 255) * tw);
            if (a < 6) continue;
            wl_blend_px(fb + (sy * LCD_WIDTH + sx) * 2, 255, 250, 235, a);
            /* Size variety: ~22% get soft orthogonal neighbours, ~8% also get
             * dimmer diagonals (a fatter star).  Neighbour alpha scales with the
             * core's current alpha so the whole star twinkles together — no
             * separately-blinking pixels.  Edge-guarded. */
            unsigned sz = (ha >> 20) % 100u;
            if (sz >= 70) {
                int an = a * 2 / 5;
                if (sx - 1 >= 0)         wl_blend_px(fb + (sy * LCD_WIDTH + sx - 1) * 2, 255, 250, 235, an);
                if (sx + 1 < LCD_WIDTH)  wl_blend_px(fb + (sy * LCD_WIDTH + sx + 1) * 2, 255, 250, 235, an);
                if (sy - 1 >= 0)         wl_blend_px(fb + ((sy - 1) * LCD_WIDTH + sx) * 2, 255, 250, 235, an);
                if (sy + 1 < LCD_HEIGHT) wl_blend_px(fb + ((sy + 1) * LCD_WIDTH + sx) * 2, 255, 250, 235, an);
            }
            if (sz >= 92) {
                int ad = a / 4;
                if (sx - 1 >= 0 && sy - 1 >= 0)                 wl_blend_px(fb + ((sy - 1) * LCD_WIDTH + sx - 1) * 2, 255, 250, 235, ad);
                if (sx + 1 < LCD_WIDTH && sy - 1 >= 0)          wl_blend_px(fb + ((sy - 1) * LCD_WIDTH + sx + 1) * 2, 255, 250, 235, ad);
                if (sx - 1 >= 0 && sy + 1 < LCD_HEIGHT)         wl_blend_px(fb + ((sy + 1) * LCD_WIDTH + sx - 1) * 2, 255, 250, 235, ad);
                if (sx + 1 < LCD_WIDTH && sy + 1 < LCD_HEIGHT)  wl_blend_px(fb + ((sy + 1) * LCD_WIDTH + sx + 1) * 2, 255, 250, 235, ad);
            }
        }
    }

    /* 2. Sun/moon disc (panorama-positioned).  The sun is a solid glowing disc;
     *    the moon carves the unlit portion with the real terminator so the phase
     *    (crescent → gibbous → full) shows, and has only a tight halo. */
    if (sc->body_show) {
        int cx = sc->body_x - tube * WL_TUBE_STRIDE;  /* centre in this tube's space */
        int cy = sc->body_y;
        int R  = sc->body_r;
        int GL = sc->body_is_moon ? 3 : (R + R / 2);  /* glow reach */
        if (cx > -(R + GL) && cx < LCD_WIDTH + (R + GL)) {
            int rr = R * R, ro = (R + GL) * (R + GL);
            int br5 = sc->br >> 3, bg6 = sc->bg >> 2, bb5 = sc->bb >> 3;
            for (int y = cy - (R + GL); y <= cy + (R + GL); y++) {
                if (y < 0 || y >= LCD_HEIGHT) continue;
                uint8_t *row = fb + y * LCD_WIDTH * 2;
                int dy = y - cy, dy2 = dy * dy;
                /* Terminator x-offset: depends only on dy, not dx — hoist out of
                 * the inner loop so sqrtf runs O(R) times per tube, not O(R²). */
                float moon_xt = 0.0f;
                if (sc->body_is_moon && dy2 < rr)
                    moon_xt = sc->moon_term * sqrtf((float)(rr - dy2));
                for (int x = cx - (R + GL); x <= cx + (R + GL); x++) {
                    if (x < 0 || x >= LCD_WIDTH) continue;
                    int dx = x - cx, d2 = dx * dx + dy2;
                    if (d2 > ro) continue;
                    uint8_t *px = row + x * 2;
                    if (d2 <= rr) {
                        if (sc->body_is_moon) {
                            bool lit = sc->moon_waxing ? ((float)dx >= moon_xt)
                                                       : ((float)dx <= -moon_xt);
                            uint16_t c = lit ? wl_rgb565(sc->br, sc->bg, sc->bb)
                                             : wl_rgb565(40, 44, 56);   /* earthshine-dark */
                            px[0] = (uint8_t)(c >> 8); px[1] = (uint8_t)(c & 0xFF);
                        } else {
                            uint16_t c = wl_rgb565(sc->br, sc->bg, sc->bb);
                            px[0] = (uint8_t)(c >> 8); px[1] = (uint8_t)(c & 0xFF);
                        }
                    } else {
                        int k = (ro - d2) * 200 / (ro - rr);   /* 0..200 glow */
                        uint16_t bgc = ((uint16_t)px[0] << 8) | px[1];
                        int R5 = (bgc >> 11) & 0x1F, G6 = (bgc >> 5) & 0x3F, B5 = bgc & 0x1F;
                        R5 += (br5 - R5) * k >> 8; G6 += (bg6 - G6) * k >> 8; B5 += (bb5 - B5) * k >> 8;
                        uint16_t c = (uint16_t)((R5 << 11) | (G6 << 5) | B5);
                        px[0] = (uint8_t)(c >> 8); px[1] = (uint8_t)(c & 0xFF);
                    }
                }
            }
        }
    }

    /* 2b. Drifting clouds — each cluster is three soft puffs, laid out on the
     *     gap-aware panorama and drawn only where it lands on this tube's glass.
     *
     * Trig hoist: cloud panorama-X (sinf gust + fmodf wrap) depends only on
     * anim_t and cloud index, not on tube — the same result was being computed
     * 6× per frame.  Cache it on the first tube each frame (anim_t change =
     * new frame) and reuse for the remaining 5 tubes. */
    static float s_cloud_gx[6];
    static float s_cloud_gx_t = -1e30f;
    if (sc->anim_t != s_cloud_gx_t) {
        const int   PANO_W = LCD_COUNT * LCD_WIDTH + (LCD_COUNT - 1) * WL_GAP_PX;
        const float span   = (float)(PANO_W + 160);
        for (int ci = 0; ci < sc->ncloud && ci < 6; ci++) {
            float base = 3.5f + (float)(ci % 3) + sc->wind * 18.0f;
            float gust = (6.0f + sc->wind * 14.0f) * sinf(sc->anim_t * 0.5f + (float)ci);
            s_cloud_gx[ci] = fmodf((float)ci * 167.0f + sc->anim_t * base + gust, span) - 80.0f;
        }
        s_cloud_gx_t = sc->anim_t;
    }
    for (int i = 0; i < sc->ncloud; i++) {
        int cy  = 26 + (i * 29) % 34;
        int cxl = (int)s_cloud_gx[i] - tube * WL_TUBE_STRIDE;
        if (cxl < -40 || cxl > LCD_WIDTH + 40) continue;
        wl_cloud_lump(fb, cxl,      cy,      16, sc->cr, sc->cg, sc->cb, sc->ca);
        wl_cloud_lump(fb, cxl - 14, cy + 4,  12, sc->cr, sc->cg, sc->cb, sc->ca);
        wl_cloud_lump(fb, cxl + 15, cy + 5,  13, sc->cr, sc->cg, sc->cb, sc->ca);
    }

    /* 2c. Precipitation — rain streaks or snow dots from the shared particle
     *     field; each particle belongs to exactly one tube's glass. */
    if (sc->precip) {
        for (int i = 0; i < WL_NPART; i++) {
            int cxl = (int)s_wl_part[i].x - tube * WL_TUBE_STRIDE;
            if (cxl < 0 || cxl >= LCD_WIDTH) continue;
            int py = (int)s_wl_part[i].y;
            if (sc->precip == 2) {                          /* snow: 2×2 soft dot */
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) {
                        int x = cxl + dx, y = py + dy;
                        if (x >= 0 && x < LCD_WIDTH && y >= 0 && y < LCD_HEIGHT)
                            wl_blend_px(fb + (y * LCD_WIDTH + x) * 2, 245, 248, 255, 230);
                    }
            } else {                                        /* rain: wind-slanted streak */
                int len = 7 + (int)(sc->wind * 6.0f);        /* longer in stronger wind */
                for (int s = 0; s < len; s++) {
                    int y = py + s;
                    int x = cxl + (int)((float)s * sc->wind * 1.3f);   /* lean downwind */
                    if (y < 0 || y >= LCD_HEIGHT) continue;
                    /* Bright, near-opaque 2-px-wide streak with a soft darker
                     * edge so it reads clearly over a light daytime sky. */
                    if (x >= 0 && x < LCD_WIDTH)
                        wl_blend_px(fb + (y * LCD_WIDTH + x) * 2, 225, 238, 255, 235);
                    if (x + 1 >= 0 && x + 1 < LCD_WIDTH)
                        wl_blend_px(fb + (y * LCD_WIDTH + x + 1) * 2, 150, 175, 205, 170);
                }
            }
        }
    }

    /* 2d. Lightning — wash the whole sky bright for the frame(s) of a flash. */
    if (sc->flash > 0.0f) {
        int a = (int)(sc->flash * 200.0f);
        if (a > 200) a = 200;
        for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++)
            wl_blend_px(fb + i * 2, 235, 242, 255, a);
    }
}

/* Render UTF-8 text centred horizontally at `cx`, baseline at `by`, into fb.
 * Two-pass: first blooms a 2px dark halo around every set pixel (inner d²≤2
 * alpha=180, outer d²≤5 alpha=90), then paints the glyph colour on top so
 * text always sits above its own shadow.  Uses the shared U8g2 1-bpp buffer. */
/* ft_px: explicit TTF pixel size; 0 = derive from ft_px_for_u8g2(font).
 * Lets per-element panel code request a different TTF size than the u8g2
 * fallback would imply, without touching the u8g2 rendering path at all. */
static void wl_text(uint8_t *fb, int cx, int by, const uint8_t *font,
                    const char *str, int r, int g, int b, uint16_t ft_px)
{
    if (s_ft_face_id >= 0) {
        /* 1. Get requested size */
        uint16_t req_px = ft_px ? ft_px : ft_px_for_u8g2(font);
        
        /* 2. No inflation multiplier needed anymore. The font_render math 
         * guarantees "req_px" yields a visible letter of exactly req_px height. */
        uint16_t ttf_px = req_px;

        /* 3. Baseline Nudge. TTF fonts drop below the standard U8g2 baseline
         * due to descenders. We subtract ~20% of the size to lift it back to center. */
        int ttf_by = by - (req_px / 5);

        fr_draw_text(fb, LCD_WIDTH, LCD_HEIGHT, cx, ttf_by,
                     (uint8_t)s_ft_face_id, ttf_px,
                     str, (uint8_t)r, (uint8_t)g, (uint8_t)b,
                     s_wl_shadow, s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b);
        return;
    }

    const int BUF_W = 128, BUF_H = 64;
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFont(&s_u8g2, font);
    int w    = (int)u8g2_GetUTF8Width(&s_u8g2, str);
    int asc  = (int)u8g2_GetAscent(&s_u8g2);
    int desc = -(int)u8g2_GetDescent(&s_u8g2);            /* make positive */
    int x = cx - w / 2;
    if (x < 0) x = 0;
    if (x > BUF_W - 1) x = BUF_W - 1;
    u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)x, (u8g2_uint_t)asc, str);
    const uint8_t *tile = u8g2_GetBufferPtr(&s_u8g2);
    int gh = asc + desc;
    if (gh > BUF_H) gh = BUF_H;
    int top = by - asc;                                   /* fb row of buffer row 0 */

    /* Pass 1: dark bloom — for every lit source pixel, paint shadow neighbours */
    if (s_wl_shadow) {
        for (int ry = 0; ry < gh; ry++) {
            int fy = top + ry;
            for (int rx = 0; rx < BUF_W && rx < LCD_WIDTH; rx++) {
                if (!((tile[(ry / 8) * BUF_W + rx] >> (ry % 8)) & 1)) continue;
                for (int dy = -2; dy <= 2; dy++) {
                    int ny = fy + dy;
                    if (ny < 0 || ny >= LCD_HEIGHT) continue;
                    for (int dx = -2; dx <= 2; dx++) {
                        int d2 = dx * dx + dy * dy;
                        if (d2 == 0 || d2 > 5) continue;
                        int nx = rx + dx;
                        if (nx < 0 || nx >= LCD_WIDTH) continue;
                        int a = (d2 <= 2) ? 180 : 90;
                        wl_blend_px(fb + (ny * LCD_WIDTH + nx) * 2, s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b, a);
                    }
                }
            }
        }
    }
    /* Pass 2: glyph colour on top so it always wins over the shadow */
    for (int ry = 0; ry < gh; ry++) {
        int fy = top + ry;
        if (fy < 0 || fy >= LCD_HEIGHT) continue;
        for (int rx = 0; rx < BUF_W && rx < LCD_WIDTH; rx++) {
            if (!((tile[(ry / 8) * BUF_W + rx] >> (ry % 8)) & 1)) continue;
            wl_blend_px(fb + (fy * LCD_WIDTH + rx) * 2, r, g, b, 255);
        }
    }
}

/* Return the pixel half-width of str at the chosen font/size for BOTH render
 * paths so label center positions are accurate regardless of whether a custom
 * TTF is loaded.  FT path uses fr_measure_text (norm_ratio + width-fit aware);
 * u8g2 path reads glyph metrics directly from the bitmap font.              */
static int wl_label_half_w(const char *str, const uint8_t *u8font, uint16_t ft_px)
{
    if (s_ft_face_id >= 0) {
        int adv = fr_measure_text(LCD_WIDTH, (uint8_t)s_ft_face_id, ft_px, str);
        return adv / 2;
    }
    u8g2_SetFont(&s_u8g2, u8font);
    return (int)u8g2_GetUTF8Width(&s_u8g2, str) / 2;
}

/* Small ring outline — the ° degree mark next to a temperature.
 * Draws a dark shadow annulus first, then the white ring on top. */
static void wl_degree(uint8_t *fb, int cx, int cy, int rad, int r, int g, int b)
{
    /* Shadow: dark ring just outside the white ring */
    if (s_wl_shadow) {
        int si2 = rad * rad, so2 = (rad + 2) * (rad + 2);
        for (int y = cy - rad - 3; y <= cy + rad + 3; y++) {
            if (y < 0 || y >= LCD_HEIGHT) continue;
            for (int x = cx - rad - 3; x <= cx + rad + 3; x++) {
                if (x < 0 || x >= LCD_WIDTH) continue;
                int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
                if (d2 >= si2 && d2 <= so2)
                    wl_blend_px(fb + (y * LCD_WIDTH + x) * 2, s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b, 160);
            }
        }
    }
    /* White ring on top */
    int ri2 = (rad - 1) * (rad - 1), ro2 = (rad + 1) * (rad + 1);
    for (int y = cy - rad - 1; y <= cy + rad + 1; y++) {
        if (y < 0 || y >= LCD_HEIGHT) continue;
        for (int x = cx - rad - 1; x <= cx + rad + 1; x++) {
            if (x < 0 || x >= LCD_WIDTH) continue;
            int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
            if (d2 >= ri2 && d2 <= ro2) wl_blend_px(fb + (y * LCD_WIDTH + x) * 2, r, g, b, 255);
        }
    }
}

/* Composite one big clock glyph (digit, ':', '.', '-', …) into `fb`, which must
 * already hold the desired background.  The glyph is rendered through U8g2's
 * 1-bpp buffer, bilinearly anti-aliased and ringed with a soft dark outline.
 * logisoso42 is upscaled 2× via bilinear AA.  TTF fonts go through font_render
 * and bypass this path entirely (see FreeType fast-path at top of function).
 * ' '/'\0' draw nothing (background shows through). */
static void wl_glyph(uint8_t *fb, char ch)
{
    if (ch == ' ' || ch == '\0') return;

    /* FreeType fast-path: delegate to font_render when a TTF face is loaded */
    if (s_ft_face_id >= 0) {
        fr_draw_glyph_centered(fb, LCD_WIDTH, LCD_HEIGHT,
                               (uint8_t)s_ft_face_id, (uint32_t)(unsigned char)ch,
                               WL_FT_BIG_PX,
                               s_wl_glyph_r, s_wl_glyph_g, s_wl_glyph_b,
                               s_wl_shadow, s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b);
        return;
    }

    char s[2] = { ch, '\0' };
    const int BUF_W = 128, BUF_H = 64;

    const uint8_t *dfont = u8g2_font_logisoso42_tf;
    const int SCALE = 2;

    const int SRC_W   = LCD_WIDTH  / SCALE;              /* 40 @ 2×, 20 @ 4× */
    /* Cap OUT_H to LCD_HEIGHT: BUF_H*4=256 would overflow the 160-px display. */
    const int OUT_H   = (BUF_H * SCALE <= LCD_HEIGHT) ? BUF_H * SCALE : LCD_HEIGHT;
    const int MARGIN  = (LCD_HEIGHT - OUT_H) / 2;        /* 16 @ 2×,  0 @ 4× */
    const int VIS_SRC = OUT_H / SCALE;                   /* 64 @ 2×, 40 @ 4× */

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFont(&s_u8g2, dfont);
    u8g2_uint_t gw = u8g2_GetUTF8Width(&s_u8g2, s);
    int gx = (SRC_W - (int)gw) / 2; if (gx < 0) gx = 0;
    int ascent = (int)u8g2_GetAscent(&s_u8g2);
    /* Centre baseline within the visible source rows, not the full BUF_H. */
    int gy = (VIS_SRC + ascent) / 2;
    if (gy < ascent) gy = ascent;
    if (gy > VIS_SRC) gy = VIS_SRC;
    u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)gx, (u8g2_uint_t)gy, s);
    const uint8_t *tile = u8g2_GetBufferPtr(&s_u8g2);

    #define WL_SRC(sx, sy)  ( (sx) >= 0 && (sx) < SRC_W && (sy) >= 0 && (sy) < BUF_H && \
        ( ( tile[ (((sy) / 8) * BUF_W) + (sx) ] >> ((sy) % 8) ) & 1 ) )

    /* Bilinear AA: map each output pixel centre back to fractional source coords. */
    const float inv_scale = 1.0f / (float)SCALE;
    const float half_inv  = 0.5f * inv_scale;

    const int halo_inner = 2;
    const int halo_outer = 5;

    for (int oy = 0; oy < OUT_H; oy++) {
        float fys = oy * inv_scale - half_inv;
        int   y0  = (int)floorf(fys);
        float ty  = fys - (float)y0;
        uint8_t *row = fb + (oy + MARGIN) * LCD_WIDTH * 2;
        for (int ox = 0; ox < LCD_WIDTH; ox++) {
            float fxs = ox * inv_scale - half_inv;
            int   x0  = (int)floorf(fxs);
            float tx  = fxs - (float)x0;
            int s00 = WL_SRC(x0,     y0    ) ? 255 : 0;
            int s10 = WL_SRC(x0 + 1, y0    ) ? 255 : 0;
            int s01 = WL_SRC(x0,     y0 + 1) ? 255 : 0;
            int s11 = WL_SRC(x0 + 1, y0 + 1) ? 255 : 0;
            float topc = s00 + (s10 - s00) * tx;
            float botc = s01 + (s11 - s01) * tx;
            int   cov  = (int)(topc + (botc - topc) * ty + 0.5f);
            if      (cov <= 70)  cov = 0;
            else if (cov >= 188) cov = 255;
            else                 cov = (cov - 70) * 255 / 118;
            uint8_t *px = row + ox * 2;
            if (cov > 4) {
                wl_blend_px(px, s_wl_glyph_r, s_wl_glyph_g, s_wl_glyph_b, cov);
            } else if (s_wl_shadow) {
                int halo_a = 0;
                for (int dn = -2; dn <= 2 && halo_a < 200; dn++) {
                    for (int dm = -2; dm <= 2 && halo_a < 200; dm++) {
                        int d2 = dm * dm + dn * dn;
                        if (d2 == 0 || d2 > halo_outer) continue;
                        if (!WL_SRC(x0 + dm, y0 + dn)) continue;
                        int a = (d2 <= halo_inner) ? 200 : 100;
                        if (a > halo_a) halo_a = a;
                    }
                }
                if (halo_a > 0) wl_blend_px(px, s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b, halo_a);
            }
        }
    }
    #undef WL_SRC
}

static void wl_draw_tube(int tube, char ch, const wl_scene_t *sc);  /* forward */

/* Pre-renders both colon-ON and colon-OFF states for tube 2 into PSRAM,
 * computes the bounding box of pixels that differ (the glyph footprint), and
 * pushes the correct current state to the LCD.
 *
 * For animated WL or a pure-WL background (no static theme image) the cache
 * is not useful — falls back to an ordinary wl_draw_tube() call without
 * caching.  Called from render_weatherlive() at every full-render tick.       */
static void wl_render_colon_tube(const struct tm *t, const wl_scene_t *sc)
{
    bool need_cache = s_wl_is_custom && s_wl_bg_theme[0] != '\0' &&
                      strncmp(s_wl_bg_theme, "WeatherLive", 11) != 0;
    bool colon_on   = (t->tm_sec % 2 == 0);

    if (!need_cache) {
        s_wl_colon_cache_valid = false;
        wl_draw_tube(2, colon_on ? ':' : ' ', sc);
        return;
    }

    uint8_t *fb = wl_fb();
    if (!fb) {
        s_wl_colon_cache_valid = false;
        display_fill(2, wl_rgb565(sc->tr, sc->tg, sc->tb));
        return;
    }

    size_t frame_sz = (size_t)LCD_WIDTH * LCD_HEIGHT * 2;

    if (!s_wl_colon_on_buf)  s_wl_colon_on_buf  = PSRAM_MALLOC(frame_sz);
    if (!s_wl_colon_off_buf) s_wl_colon_off_buf = PSRAM_MALLOC(frame_sz);

    if (!s_wl_colon_on_buf || !s_wl_colon_off_buf) {
        s_wl_colon_cache_valid = false;
        wl_draw_tube(2, colon_on ? ':' : ' ', sc);
        return;
    }

    /* Cache hit: buffers and diff-box are current — skip the expensive repaint
     * and just copy the right frame into fb, then push it to the LCD.
     * Invalidated externally (s_wl_colon_cache_valid = false) on any config,
     * mode, or theme change so a rebuild happens on the next full render.     */
    if (s_wl_colon_cache_valid) {
        memcpy(fb, colon_on ? s_wl_colon_on_buf : s_wl_colon_off_buf, frame_sz);
        display_show_digit(2, fb, LCD_WIDTH, LCD_HEIGHT);
        return;
    }

    /* Render OFF state (background only) → save */
    wl_paint_background(fb, 2, sc);
    memcpy(s_wl_colon_off_buf, fb, frame_sz);

    /* Render ON state (background + colon glyph) → save; fb now holds ON */
    wl_glyph(fb, ':');
    memcpy(s_wl_colon_on_buf, fb, frame_sz);

    /* Compute diff-box: bounding rect of pixels that differ ON vs OFF */
    int bx0 = LCD_WIDTH, by0 = LCD_HEIGHT, bx1 = -1, by1 = -1;
    for (int y = 0; y < LCD_HEIGHT; y++) {
        const uint16_t *r_on  = (const uint16_t *)(s_wl_colon_on_buf  + y * LCD_WIDTH * 2);
        const uint16_t *r_off = (const uint16_t *)(s_wl_colon_off_buf + y * LCD_WIDTH * 2);
        for (int x = 0; x < LCD_WIDTH; x++) {
            if (r_on[x] != r_off[x]) {
                if (x < bx0) bx0 = x;
                if (x > bx1) bx1 = x;
                if (y < by0) by0 = y;
                if (y > by1) by1 = y;
            }
        }
    }
    if (bx1 < 0) { bx0 = 0; by0 = 0; bx1 = 1; by1 = 1; }  /* empty glyph guard */

    s_wl_colon_bx0 = (int16_t)bx0;
    s_wl_colon_by0 = (int16_t)by0;
    s_wl_colon_bw  = (int16_t)(bx1 - bx0 + 1);
    s_wl_colon_bh  = (int16_t)(by1 - by0 + 1);
    s_wl_colon_cache_valid = true;

    /* Push current state; fb already holds the ON frame after wl_glyph() */
    if (!colon_on) memcpy(fb, s_wl_colon_off_buf, frame_sz);
    display_show_digit(2, fb, LCD_WIDTH, LCD_HEIGHT);
}

/* Fast colon blink for the static-custom-face: pushes only the diff-box
 * region from the PSRAM-cached ON or OFF frame — same pixel-budget as
 * display_show_colon_blink() but uses the WL/TTF-rendered glyph instead of
 * a theme JPEG.  Falls back to wl_draw_tube() when the cache is not yet
 * populated.                                                                  */
static void wl_show_colon_blink(bool show_colon)
{
    if (!s_wl_colon_cache_valid) {
        if (s_wl_scene_valid)
            wl_draw_tube(2, show_colon ? ':' : ' ', &s_wl_last_scene);
        return;
    }

    const uint8_t *buf = show_colon ? s_wl_colon_on_buf : s_wl_colon_off_buf;

    uint8_t br     = s_tube_brightness[2];
    bool    do_br  = (br < 100);
    bool    do_gam = s_gamma_lut_active[2];

    select_tube(2);
    uint8_t ox = (uint8_t)((int)LCD_OFFSET_X + (int)s_burnin_shift_x
                            + (int)s_col_offsets[2] + s_wl_colon_bx0);
    uint8_t oy = (uint8_t)((int)LCD_OFFSET_Y + (int)s_row_offsets[2] + s_wl_colon_by0);
    open_lcd_window(ox, oy, (uint8_t)s_wl_colon_bw, (uint8_t)s_wl_colon_bh);

    uint8_t line[LCD_WIDTH * 2];
    for (int yy = 0; yy < s_wl_colon_bh; yy++) {
        const uint8_t *src = buf + ((size_t)(s_wl_colon_by0 + yy) * LCD_WIDTH
                                    + s_wl_colon_bx0) * 2;
        if (do_br || do_gam) {
            for (int xx = 0; xx < s_wl_colon_bw; xx++) {
                uint16_t px = ((uint16_t)src[xx*2] << 8) | src[xx*2+1];
                uint32_t r  = (px >> 11) & 0x1Fu;
                uint32_t g  = (px >>  5) & 0x3Fu;
                uint32_t b  =  px        & 0x1Fu;
                if (do_br)  { r = r*br/100u; g = g*br/100u; b = b*br/100u; }
                if (do_gam) { r = s_gamma_lut_5bit[2][r];
                              g = s_gamma_lut_6bit[2][g];
                              b = s_gamma_lut_5bit[2][b]; }
                px = (uint16_t)((r << 11) | (g << 5) | b);
                line[xx*2] = (uint8_t)(px >> 8); line[xx*2+1] = (uint8_t)(px & 0xFF);
            }
        } else {
            memcpy(line, src, (size_t)s_wl_colon_bw * 2);
        }
        spi_transaction_t tx = { .length = (size_t)(s_wl_colon_bw * 2) * 8,
                                  .tx_buffer = line };
        spi_device_polling_transmit(spi_dev, &tx);
    }
    deselect_all();
}

/* Render one clock glyph (digit, ':' or blank) onto `tube`, over the scene's
 * sky panorama. */
static void wl_draw_tube(int tube, char ch, const wl_scene_t *sc)
{
    uint8_t *fb = wl_fb();
    if (!fb) { display_fill(tube, wl_rgb565(sc->tr, sc->tg, sc->tb)); return; }
    wl_paint_background(fb, tube, sc);
    wl_glyph(fb, ch);
    /* Push the frame (handles PSRAM→SRAM copy + per-tube gamma/brightness). */
    display_show_digit(tube, fb, LCD_WIDTH, LCD_HEIGHT);
}

/* WeatherLive has no JPEG theme assets, so when a non-clock mode (weather,
 * follower counts, countdown, pomodoro, …) asks to show a theme image, we render
 * the requested asset procedurally as a glyph/number over a solid black
 * background instead.  `path` is the LittleFS asset path that was about to be
 * loaded; the basename selects the glyph.  Digits and the colon/dot/minus match
 * the clock's look; degree/percent/AM-PM/K-M get a sensible procedural stand-in;
 * weather-condition icons and mode logos (no glyph equivalent) draw black. */
static void wl_render_asset(int tube, const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *bp    = slash ? slash + 1 : path;       /* e.g. "5.jpg" */
    char name[32];
    size_t i = 0;
    for (; bp[i] && bp[i] != '.' && i < sizeof(name) - 1; i++) name[i] = bp[i];
    name[i] = '\0';

    uint8_t *fb = wl_fb();
    if (!fb) { display_fill(tube, 0x0000); return; }
    memset(fb, 0, (size_t)LCD_WIDTH * LCD_HEIGHT * 2);   /* black fill */

    if (name[0] >= '0' && name[0] <= '9' && name[1] == '\0') {
        wl_glyph(fb, name[0]);                            /* single digit */
    } else if (!strcmp(name, "colon")) {
        wl_glyph(fb, ':');
    } else if (!strcmp(name, "dot")) {
        wl_glyph(fb, '.');
    } else if (!strcmp(name, "minus")) {
        wl_glyph(fb, '-');
    } else if (!strcmp(name, "blank")) {
        /* black only */
    } else if (!strcmp(name, "degreec") || !strcmp(name, "degreef")) {
        /* Full-tube upscaled letter in the selected font — matches digit tubes. */
        wl_glyph(fb, (name[6] == 'f') ? 'F' : 'C');
    } else if (!strcmp(name, "humidity")) {
        wl_glyph(fb, '%');
    } else if (!strcmp(name, "am")) {
        /* Two chars can't go through wl_glyph's single-char path; use wl_text
         * with the selected font so the typeface still matches. */
        wl_text(fb, 40, 96, u8g2_font_logisoso28_tf,
                "AM", s_wl_glyph_r, s_wl_glyph_g, s_wl_glyph_b, 0);
    } else if (!strcmp(name, "pm")) {
        wl_text(fb, 40, 96, u8g2_font_logisoso28_tf,
                "PM", s_wl_glyph_r, s_wl_glyph_g, s_wl_glyph_b, 0);
    } else if (!strcmp(name, "m-sub")) {
        wl_glyph(fb, 'M');
    } else if (!strcmp(name, "k-sub")) {
        wl_glyph(fb, 'K');
    }
    /* else: weather-condition icons / platform logos → leave black. */

    display_show_digit(tube, fb, LCD_WIDTH, LCD_HEIGHT);
}

/* Draw the asset-theme sun glyph (filled upper semicircle + horizon line +
 * three rays + a rise/set caret) and a time string into the panorama
 * framebuffer, composited over the live sky with a 1-px drop shadow.  Mirrors
 * ht_draw_suntime()'s iconography so WeatherLive's Sunrise panel matches the
 * JPEG asset themes.  `top` is the fb row of the icon/text block (56 rows). */
static void wl_suntime(uint8_t *fb, int top, bool rising, const char *timestr,
                       int r, int g, int b)
{
    const int BUF_W = 128;
    const int cx = 40, cy = 16, rr = 9;

    u8g2_ClearBuffer(&s_u8g2);
    /* Filled upper semicircle + horizon */
    u8g2_DrawDisc(&s_u8g2, (u8g2_uint_t)cx, (u8g2_uint_t)cy, (u8g2_uint_t)rr,
                  U8G2_DRAW_UPPER_RIGHT | U8G2_DRAW_UPPER_LEFT);
    u8g2_DrawHLine(&s_u8g2, (u8g2_uint_t)(cx - rr - 3), (u8g2_uint_t)cy,
                   (u8g2_uint_t)((rr + 3) * 2 + 1));
    /* Three rays above the arc */
    u8g2_DrawLine(&s_u8g2, (u8g2_uint_t)cx,        (u8g2_uint_t)(cy-rr-2),
                            (u8g2_uint_t)cx,        (u8g2_uint_t)(cy-rr-5));
    u8g2_DrawLine(&s_u8g2, (u8g2_uint_t)(cx-rr-1), (u8g2_uint_t)(cy-2),
                            (u8g2_uint_t)(cx-rr-4), (u8g2_uint_t)(cy-5));
    u8g2_DrawLine(&s_u8g2, (u8g2_uint_t)(cx+rr+1), (u8g2_uint_t)(cy-2),
                            (u8g2_uint_t)(cx+rr+4), (u8g2_uint_t)(cy-5));
    /* Direction caret just below the horizon (^ rise, v set) */
    if (rising) {
        u8g2_DrawLine(&s_u8g2, (u8g2_uint_t)(cx-4), (u8g2_uint_t)(cy+5),
                                (u8g2_uint_t)cx,     (u8g2_uint_t)(cy+2));
        u8g2_DrawLine(&s_u8g2, (u8g2_uint_t)(cx+4), (u8g2_uint_t)(cy+5),
                                (u8g2_uint_t)cx,     (u8g2_uint_t)(cy+2));
    } else {
        u8g2_DrawLine(&s_u8g2, (u8g2_uint_t)(cx-4), (u8g2_uint_t)(cy+2),
                                (u8g2_uint_t)cx,     (u8g2_uint_t)(cy+5));
        u8g2_DrawLine(&s_u8g2, (u8g2_uint_t)(cx+4), (u8g2_uint_t)(cy+2),
                                (u8g2_uint_t)cx,     (u8g2_uint_t)(cy+5));
    }
    /* Time string — logisoso20, centred in 80 px, baseline at row 50 */
    u8g2_SetFont(&s_u8g2, u8g2_font_logisoso20_tf);
    u8g2_uint_t tw = u8g2_GetStrWidth(&s_u8g2, timestr);
    int tx = ((int)LCD_WIDTH - (int)tw) / 2; if (tx < 0) tx = 0;
    u8g2_DrawStr(&s_u8g2, (u8g2_uint_t)tx, 50, timestr);

    /* Composite the 56-row icon+text block into fb at `top`.
     * Two-pass bloom — same approach as wl_text — so the shadow body is wide
     * enough to read against bright sky backgrounds. */
    const uint8_t *tile = u8g2_GetBufferPtr(&s_u8g2);
    /* Pass 1: dark bloom around every set pixel */
    if (s_wl_shadow) {
        for (int ry = 0; ry < 56; ry++) {
            int fy = top + ry;
            for (int rx = 0; rx < BUF_W && rx < LCD_WIDTH; rx++) {
                if (!((tile[(ry / 8) * BUF_W + rx] >> (ry % 8)) & 1)) continue;
                for (int dy = -2; dy <= 2; dy++) {
                    int ny = fy + dy;
                    if (ny < 0 || ny >= LCD_HEIGHT) continue;
                    for (int dx = -2; dx <= 2; dx++) {
                        int d2 = dx * dx + dy * dy;
                        if (d2 == 0 || d2 > 5) continue;
                        int nx = rx + dx;
                        if (nx < 0 || nx >= LCD_WIDTH) continue;
                        int a = (d2 <= 2) ? 180 : 90;
                        wl_blend_px(fb + (ny * LCD_WIDTH + nx) * 2, s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b, a);
                    }
                }
            }
        }
    }
    /* Pass 2: foreground colour on top */
    for (int ry = 0; ry < 56; ry++) {
        int fy = top + ry;
        if (fy < 0 || fy >= LCD_HEIGHT) continue;
        for (int rx = 0; rx < BUF_W && rx < LCD_WIDTH; rx++) {
            if (!((tile[(ry / 8) * BUF_W + rx] >> (ry % 8)) & 1)) continue;
            wl_blend_px(fb + (fy * LCD_WIDTH + rx) * 2, r, g, b, 255);
        }
    }
}

/* Render a temperature value auto-scaled to fit a vertical zone centred at
 * `zone_cy`.  ft_px[4] gives FreeType pixel heights for 1-/2-/3-/4+-char
 * values; u8_cap[4] gives the matching u8g2 cap heights (mapped to the
 * nearest logisoso font).  Horizontal centre is always 40 px (tube centre);
 * u8g2 path uses x=36 for text + x=64 for the degree ring. */
static void wl_draw_temp_scaled(uint8_t *fb, int temp_disp, int zone_cy,
                                 int r, int g, int b,
                                 const int ft_px[4], const int u8_cap[4])
{
    char bare[8]; snprintf(bare, sizeof(bare), "%d", temp_disp);
    int nc = (int)strlen(bare);
    if (temp_disp < 0) nc--;          /* don't count the minus sign */
    if (nc < 1) nc = 1;
    int idx = (nc <= 1) ? 0 : (nc <= 2) ? 1 : (nc <= 3) ? 2 : 3;

    if (s_ft_face_id >= 0) {
        uint16_t px = (uint16_t)ft_px[idx];
        int by = zone_cy + 7 * (int)px / 10;
        char tnum[16]; snprintf(tnum, sizeof(tnum), "%d\xc2\xb0", temp_disp);
        wl_text(fb, 40, by, u8g2_font_logisoso28_tf, tnum, r, g, b, px);
    } else {
        int cap = u8_cap[idx];
        const uint8_t *font =
            (cap >= 46) ? u8g2_font_logisoso46_tf :
            (cap >= 42) ? u8g2_font_logisoso42_tf :
            (cap >= 28) ? u8g2_font_logisoso28_tf :
                          u8g2_font_logisoso20_tf;
        int by = zone_cy + cap / 2;
        wl_text(fb, 36, by, font, bare, r, g, b, 0);
        wl_degree(fb, 64, by - cap + cap / 5, 3, r, g, b);
    }
}

/* Draw the temperature panel into `fb` (already holding the desired background —
 * the WeatherLive sky, or an asset theme's blank.jpg): current temperature on
 * top with a degree ring, over today's high/low range track with a current-temp
 * marker, and lo (left) / hi (right) numbers beneath.  Shared by the WeatherLive
 * TEMP panel and the asset-theme 24H_CX Outdoor Temperature panel so both render
 * identically.  Temperatures are already unit-converted by the caller. */
static void wl_temp_panel(uint8_t *fb, int temp_disp, bool range_ok,
                          int dmin_disp, int dmax_disp)
{
    static const int s_temp_ft_px[4]  = { 72, 56, 40, 32 };
    static const int s_temp_u8_cap[4] = { 46, 42, 28, 20 };
    wl_draw_temp_scaled(fb, temp_disp, 45, 255, 255, 255,
                        s_temp_ft_px, s_temp_u8_cap);
    if (range_ok && dmax_disp > dmin_disp) {
        const int bx0 = 12, bx1 = 68, by = 90;
        /* Shadow under the track — follows the global shadow setting. */
        if (s_wl_shadow) {
            for (int x = bx0; x <= bx1; x++) {
                wl_blend_px(fb + ((by + 3) * LCD_WIDTH + x) * 2,
                            s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b, 110);
                wl_blend_px(fb + ((by + 4) * LCD_WIDTH + x) * 2,
                            s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b, 55);
            }
        }
        for (int x = bx0; x <= bx1; x++)               /* range track */
            for (int yy = by; yy < by + 3; yy++)
                wl_blend_px(fb + (yy * LCD_WIDTH + x) * 2, 230, 235, 245, 90);
        int tt = temp_disp;
        if (tt < dmin_disp) tt = dmin_disp;
        if (tt > dmax_disp) tt = dmax_disp;
        int mx = bx0 + (bx1 - bx0) * (tt - dmin_disp) / (dmax_disp - dmin_disp);
        if (s_wl_shadow)
            wl_cloud_lump(fb, mx + 1, by + 2, 7,
                          s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b, 180);
        wl_cloud_lump(fb, mx, by + 1, 6, 255, 225, 120, 255);   /* current-temp marker */
        char lo[12], hi[12];
        snprintf(lo, sizeof(lo), "%d", dmin_disp);
        snprintf(hi, sizeof(hi), "%d", dmax_disp);
        int lo_len = (int)strlen(lo);
        int hi_len = (int)strlen(hi);

        /* Downgrade font when either label needs 3 chars (negative two-digit).
         * FT by-adj = 7*px/10; u8g2 by-adj = ascent/2 (logisoso20≈10, logisoso16≈7). */
        const uint8_t *lbl_font;
        uint16_t       lbl_px;
        int            lbl_by_adj;
        if (lo_len >= 3 || hi_len >= 3) {
            lbl_font   = u8g2_font_logisoso16_tf;
            lbl_px     = 22;
            lbl_by_adj = s_ft_face_id >= 0 ? 15 : 7;
        } else {
            lbl_font   = u8g2_font_logisoso20_tf;
            lbl_px     = 24;
            lbl_by_adj = s_ft_face_id >= 0 ? 19 : 10;
        }

        /* Measure actual pixel half-widths for both render paths.
         * FT path accounts for norm_ratio and width-fit; u8g2 reads metrics
         * from the bitmap font directly.  Both are exact for the chosen font. */
        int hw_lo = wl_label_half_w(lo, lbl_font, lbl_px);
        int hw_hi = wl_label_half_w(hi, lbl_font, lbl_px);

        /* Anchor lo's left edge to bx0 and hi's right edge to bx1.
         * Overlap push runs before the right-edge clamp so the push cannot
         * send hi off the tube (labels may touch for unusually wide fonts). */
        int cx_lo = bx0 + hw_lo + 1;
        int cx_hi = bx1 - hw_hi - 1;
        if (cx_lo - hw_lo < 1)
            cx_lo = hw_lo + 1;
        if (cx_hi - hw_hi < cx_lo + hw_lo + 3)
            cx_hi = cx_lo + hw_lo + 3 + hw_hi;
        if (cx_hi + hw_hi > LCD_WIDTH - 1)
            cx_hi = LCD_WIDTH - 1 - hw_hi;

        int _mid     = (by + 5 + LCD_HEIGHT - 1) / 2;
        int label_by = _mid + lbl_by_adj;
        wl_text(fb, cx_lo, label_by, lbl_font, lo, 200, 215, 235, lbl_px);
        wl_text(fb, cx_hi, label_by, lbl_font, hi, 235, 225, 150, lbl_px);
    }
}

/* Draw the humidity panel into `fb` (already holding the desired background):
 * drop scaled to the top half of the tube (shadow lands at row 79), fill locked
 * to the panel-1 blue (80,160,255).  Value+% centred in the bottom half.
 * Shared by the WeatherLive HUMIDITY panel and the asset-theme 24H_CX Humidity
 * panel so both render identically.  `hum` is the relative humidity in whole %. */
static void wl_humidity_panel(uint8_t *fb, int hum)
{
    /* Panel-1 geometry scaled 0.86× so shadow bottom lands at row 79 (top-half). */
    const int tip = 4, cx = 40, bcy = 58, rad = 19;
    const int SH = 2;

    /* Shadow half-width lookup: s_hum_sh_hw[i] = half-width at row bcy+i (circular cap).
     * Precomputed once from rad=19; SH outset added at use. */
    static int8_t s_hum_sh_hw[22];   /* indices 0..rad+SH = 0..21 */
    static bool   s_hum_sh_init;
    if (!s_hum_sh_init) {
        for (int i = 0; i <= rad + SH; i++) {
            float v = (float)(rad * rad) - (float)(i * i);
            s_hum_sh_hw[i] = (int8_t)((v > 0.0f) ? (int)(sqrtf(v) + 0.5f) : 0);
        }
        s_hum_sh_init = true;
    }
    if (s_wl_shadow) {
        for (int sy = tip - SH; sy <= bcy + rad + SH; sy++) {
            if (sy < 0 || sy >= LCD_HEIGHT) continue;
            int iw;
            if (sy < tip) {
                int dist = tip - sy;
                iw = (dist <= SH) ? (SH - dist) : 0;
            } else if (sy <= bcy) {
                float t = (float)(sy - tip) / (float)(bcy - tip);
                iw = (int)((float)rad * t * (2.0f - t) + 0.5f) + SH;
            } else {
                int di = sy - bcy;
                iw = (di <= rad + SH) ? (int)s_hum_sh_hw[di] + SH : 0;
            }
            for (int sx = cx - iw; sx <= cx + iw; sx++) {
                if (sx < 0 || sx >= LCD_WIDTH) continue;
                wl_blend_px(fb + (sy * LCD_WIDTH + sx) * 2,
                            s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b, 160);
            }
        }
    }

    /* Droplet fill — panel-1 blue (80, 160, 255). */
    for (int y = tip; y <= bcy + rad; y++) {
        if (y < 0 || y >= LCD_HEIGHT) continue;
        int cone_hw = 0;
        if (y <= bcy) {
            float t = (float)(y - tip) / (float)(bcy - tip);
            cone_hw = (int)((float)rad * t * (2.0f - t) + 0.5f);
        }
        for (int x = cx - rad; x <= cx + rad; x++) {
            if (x < 0 || x >= LCD_WIDTH) continue;
            int dx = x - cx, dy = y - bcy;
            bool in_circle = (dx * dx + dy * dy <= rad * rad);
            bool in_cone   = (y <= bcy && (dx < 0 ? -dx : dx) <= cone_hw);
            if (in_circle || in_cone)
                wl_blend_px(fb + (y * LCD_WIDTH + x) * 2, 80, 160, 255, 255);
        }
    }

    /* Specular highlight — scaled proportionally to new bulb size. */
    for (int y = bcy - rad + 3; y <= bcy - rad + 10; y++) {
        if (y < 0 || y >= LCD_HEIGHT) continue;
        for (int x = cx - 10; x <= cx - 2; x++) {
            if (x < 0 || x >= LCD_WIDTH) continue;
            int dx = x - (cx - 6), dy = y - (bcy - rad + 6);
            if (dx * dx + dy * dy <= 12)
                wl_blend_px(fb + (y * LCD_WIDTH + x) * 2, 220, 240, 255, 180);
        }
    }

    /* Humidity value with "%" — centred in bottom half (rows 80-159). */
    static int   s_hum_last = -1;
    static char  s_hum_str[8];
    if (hum != s_hum_last) { snprintf(s_hum_str, sizeof(s_hum_str), "%d%%", hum); s_hum_last = hum; }
    wl_text(fb, cx, 134, u8g2_font_logisoso28_tf, s_hum_str, s_wl_font_r, s_wl_font_g, s_wl_font_b, 28);
}

/* Draw the wind panel into `fb` (already holding the desired background): a wind
 * symbol — three horizontal streaks each ending in a curl (matching the supplied
 * wind icon) — over the wind speed value and a small unit label.  `wind_kph` is
 * the speed in km/h; `unit` ("km/h", "mph", or "m/s") selects the displayed
 * conversion + label.  Shared by the WeatherLive WIND panel and the asset-theme
 * 24H_CX Wind panel so both render identically. */
static void wl_wind_panel(uint8_t *fb, int wind_kph, const char *unit, int r, int g, int b)
{
    /* Three streaks: {x0, x1, y, curl-radius}.  Each is a thick rounded line
     * with a loop curled off its right end. */
    static const int st[3][4] = {
        { 16, 50, 30, 7 },
        { 10, 58, 50, 8 },
        { 18, 46, 70, 7 },
    };
    typedef struct { int8_t x, y; } wl_px_t;
    static wl_px_t s_cfg[3][80];   /* foreground curl pixels per streak */
    static int     s_cfgn[3];
    static wl_px_t s_csh[3][80];   /* shadow curl pixels per streak */
    static int     s_cshn[3];
    static bool    s_cinit;
    if (!s_cinit) {
        for (int si = 0; si < 3; si++) {
            int x1 = st[si][1], sy2 = st[si][2], cr = st[si][3];
            int ccx = x1, ccy = sy2 - cr;
            s_cfgn[si] = s_cshn[si] = 0;
            for (float a = 1.57f; a <= 7.0f; a += 0.05f) {
                /* foreground: rings cr and cr-1 */
                for (int t = 0; t < 2; t++) {
                    int rr = cr - t;
                    int px = ccx + (int)((float)rr * cosf(a) + 0.5f);
                    int py = ccy + (int)((float)rr * sinf(a) + 0.5f);
                    if (px < 0 || px >= LCD_WIDTH || py < 0 || py >= LCD_HEIGHT) continue;
                    bool dup = false;
                    for (int i = 0; i < s_cfgn[si]; i++)
                        if (s_cfg[si][i].x == (int8_t)px && s_cfg[si][i].y == (int8_t)py) { dup = true; break; }
                    if (!dup && s_cfgn[si] < 80) {
                        s_cfg[si][s_cfgn[si]].x = (int8_t)px;
                        s_cfg[si][s_cfgn[si]].y = (int8_t)py;
                        s_cfgn[si]++;
                    }
                }
                /* shadow: rings cr+1 and cr+2 */
                for (int t = 0; t < 2; t++) {
                    int rr = cr + 1 + t;
                    int px = ccx + (int)((float)rr * cosf(a) + 0.5f);
                    int py = ccy + (int)((float)rr * sinf(a) + 0.5f);
                    if (px < 0 || px >= LCD_WIDTH || py < 0 || py >= LCD_HEIGHT) continue;
                    bool dup = false;
                    for (int i = 0; i < s_cshn[si]; i++)
                        if (s_csh[si][i].x == (int8_t)px && s_csh[si][i].y == (int8_t)py) { dup = true; break; }
                    if (!dup && s_cshn[si] < 80) {
                        s_csh[si][s_cshn[si]].x = (int8_t)px;
                        s_csh[si][s_cshn[si]].y = (int8_t)py;
                        s_cshn[si]++;
                    }
                }
            }
        }
        s_cinit = true;
    }
    /* Shadow pass: expanded geometry drawn first so the foreground sits on top.
     * Gated on the global shadow setting and uses the configured shadow colour. */
    if (s_wl_shadow) {
        for (int s = 0; s < 3; s++) {
            int x0 = st[s][0], x1 = st[s][1], y = st[s][2], cr = st[s][3];
            /* Streak shadow: 1px wider each side, 2px taller. */
            for (int yy = y - 2; yy <= y + 2; yy++) {
                if (yy < 0 || yy >= LCD_HEIGHT) continue;
                for (int x = x0 - 1; x <= x1 + 1; x++) {
                    if (x < 0 || x >= LCD_WIDTH) continue;
                    wl_blend_px(fb + (yy * LCD_WIDTH + x) * 2,
                                s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b, 150);
                }
            }
            /* Curl shadow from precomputed table. */
            for (int pi = 0; pi < s_cshn[s]; pi++) {
                int px = (int)s_csh[s][pi].x, py = (int)s_csh[s][pi].y;
                wl_blend_px(fb + (py * LCD_WIDTH + px) * 2,
                            s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b, 150);
            }
        }
    }
    /* Foreground pass: white streaks and curls at original geometry. */
    for (int s = 0; s < 3; s++) {
        int x0 = st[s][0], x1 = st[s][1], y = st[s][2], cr = st[s][3];
        for (int yy = y - 1; yy <= y + 1; yy++) {
            if (yy < 0 || yy >= LCD_HEIGHT) continue;
            for (int x = x0; x <= x1; x++) {
                if (x < 0 || x >= LCD_WIDTH) continue;
                wl_blend_px(fb + (yy * LCD_WIDTH + x) * 2, r, g, b, 255);
            }
        }
        /* Curl foreground from precomputed table. */
        for (int pi = 0; pi < s_cfgn[s]; pi++) {
            int px = (int)s_cfg[s][pi].x, py = (int)s_cfg[s][pi].y;
            wl_blend_px(fb + (py * LCD_WIDTH + px) * 2, r, g, b, 255);
        }
    }
    /* Convert km/h → the configured unit, then draw the value with a small
     * unit label beneath it. */
    int val; const char *label;
    if (unit && strcmp(unit, "mph") == 0) {
        val = (int)lroundf((float)wind_kph * 0.621371f); label = "mph";
    } else if (unit && strcmp(unit, "m/s") == 0) {
        val = (int)lroundf((float)wind_kph / 3.6f);       label = "m/s";
    } else {
        val = wind_kph;                                   label = "km/h";
    }
    char v[12];
    snprintf(v, sizeof(v), "%d", val);
    /* zone: streak shadow bottom ~row 72, unit label top ~row 134, centre 103 */
    {
        int nc = (val >= 100) ? 3 : (val >= 10) ? 2 : 1;
        if (s_ft_face_id >= 0) {
            uint16_t px = (nc <= 1) ? 44 : (nc <= 2) ? 36 : 24;
            int by = 103 + 7 * (int)px / 10;
            wl_text(fb, 40, by, u8g2_font_logisoso28_tf, v, r, g, b, px);
        } else {
            const uint8_t *font; int cap;
            if      (nc <= 1) { font = u8g2_font_logisoso38_tf; cap = 36; }
            else if (nc <= 2) { font = u8g2_font_logisoso26_tf; cap = 24; }
            else              { font = u8g2_font_logisoso20_tf; cap = 18; }
            wl_text(fb, 40, 103 + cap / 2, font, v, r, g, b, 0);
        }
    }
    wl_text(fb, 40, 150, u8g2_font_logisoso16_tf, label, r, g, b, 0);
}

/* WeatherLive info-panel kinds (the subset rendered procedurally over the sky).
 * Today's high/low is folded into the TEMP panel — there is no separate kind. */
enum { WLP_WEEKDATE = 0, WLP_TEMP, WLP_SUNRISE, WLP_HUMIDITY, WLP_WIND, WLP_INDOOR_HT };

/* Render one WeatherLive info panel (`kind`) over the sky panorama on `tube`.
 * Temperatures are already unit-converted by the caller; sunrise/sunset are
 * minutes-of-day. */
static void wl_draw_panel(int tube, const wl_scene_t *sc, const struct tm *t,
                          const char *lang, bool date_us, const char *wind_unit,
                          int kind, int temp_disp,
                          bool range_ok, int dmin_disp, int dmax_disp,
                          int sunrise, int sunset,
                          int indoor_temp_disp, int indoor_hum)
{
    uint8_t *fb = wl_fb();
    if (!fb) { display_fill(tube, wl_rgb565(sc->tr, sc->tg, sc->tb)); return; }

    wl_paint_background(fb, tube, sc);

    /* Colors come from the per-frame render state set by render_weatherlive. */
    int fg_r = s_wl_font_r, fg_g = s_wl_font_g, fg_b = s_wl_font_b;

    if (kind == WLP_WEEKDATE) {
        /* Weekday, then day-of-month and localised month abbreviation stacked.
         * Order follows the Date format setting: US (MM/DD/YY) → month over day;
         * international (DD/MM/YY) → day over month. */
        char dd[12];
        snprintf(dd, sizeof(dd), "%02d", t->tm_mday);
        const char *mon = month_abbrev(lang, t->tm_mon);
        const char *line2 = date_us ? mon : dd;
        const char *line3 = date_us ? dd  : mon;
        /* 54px baseline interval, centred so top margin == bottom margin.
         * FT (ft_px=40): cap=40px, gap=14px, margin=6px  → by={54,108,162}
         *   cap_top = by - 6*40/5 = by-48; cap_bottom = by-8.
         * u8g2 logisoso28 (asc=28): gap=26px, margin=12px → by={40,94,148}
         *   Extra gap absorbs descender overflow from month abbreviations. */
        int _by1, _by2, _by3;
        if (s_ft_face_id >= 0) { _by1 =  54; _by2 = 108; _by3 = 162; }
        else                   { _by1 =  40; _by2 =  94; _by3 = 148; }
        wl_text(fb, 40, _by1, u8g2_font_logisoso28_tf, weekday_abbrev(lang, t->tm_wday), fg_r, fg_g, fg_b, 40);
        wl_text(fb, 40, _by2, u8g2_font_logisoso28_tf, line2, fg_r, fg_g, fg_b, 40);
        wl_text(fb, 40, _by3, u8g2_font_logisoso28_tf, line3, fg_r, fg_g, fg_b, 40);

    } else if (kind == WLP_TEMP) {
        wl_temp_panel(fb, temp_disp, range_ok, dmin_disp, dmax_disp);

    } else if (kind == WLP_HUMIDITY) {
        const weather_data_t *w = weather_get();
        wl_humidity_panel(fb, (w && w->valid) ? (int)lroundf(w->humidity) : 0);

    } else if (kind == WLP_WIND) {
        const weather_data_t *w = weather_get();
        wl_wind_panel(fb, (w && w->valid) ? (int)lroundf(w->wind_kph) : 0, wind_unit, fg_r, fg_g, fg_b);

    } else if (kind == WLP_INDOOR_HT) {
        /* "In" label — top zone rows 5-24. */
        wl_text(fb, 40, 28, u8g2_font_logisoso20_tf,
                inout_label(lang, true), fg_r, fg_g, fg_b, 20);

        if (indoor_hum >= 0) {
            /* Temperature — auto-scale into rows 32-97 (zone centre 64). */
            static const int s_ht_ft_px[4]  = { 55, 55, 36, 28 };
            static const int s_ht_u8_cap[4] = { 42, 42, 28, 28 };
            wl_draw_temp_scaled(fb, indoor_temp_disp, 64, fg_r, fg_g, fg_b,
                                s_ht_ft_px, s_ht_u8_cap);

            /* Humidity — centred in rows 103-155 (zone centre 129). */
            static int  s_iht_hum_last = -1;
            static char s_iht_hum_str[12];
            if (indoor_hum != s_iht_hum_last) {
                snprintf(s_iht_hum_str, sizeof(s_iht_hum_str), "%d%%", indoor_hum);
                s_iht_hum_last = indoor_hum;
            }
            int by_h = (s_ft_face_id >= 0) ? 129 + 7 * 32 / 10 : 129 + 14;
            wl_text(fb, 40, by_h, u8g2_font_logisoso28_tf, s_iht_hum_str,
                    fg_r, fg_g, fg_b, 32);
        } else {
            /* Sensor absent or not yet valid — show dashes. */
            int by_t = (s_ft_face_id >= 0) ? 64 + 7 * 36 / 10 : 64 + 14;
            wl_text(fb, 40, by_t, u8g2_font_logisoso28_tf, "--",
                    fg_r, fg_g, fg_b, 36);
            int by_h = (s_ft_face_id >= 0) ? 129 + 7 * 32 / 10 : 129 + 14;
            wl_text(fb, 40, by_h, u8g2_font_logisoso28_tf, "--%",
                    fg_r, fg_g, fg_b, 32);
        }

    } else { /* WLP_SUNRISE — asset-style sun glyphs: rise on top, set below. */
        char rs[8], ss[8];
        snprintf(rs, sizeof(rs), "%02d:%02d", (sunrise / 60) % 24, sunrise % 60);
        snprintf(ss, sizeof(ss), "%02d:%02d", (sunset  / 60) % 24, sunset  % 60);
        /* In Custom mode use the configured font color; in legacy WeatherLive
         * keep the warm-orange rise / cool-blue set tints. */
        int rr, rg, rb, sr2, sg, sb;
        if (s_wl_is_custom) {
            rr = fg_r; rg = fg_g; rb = fg_b;
            sr2 = fg_r; sg = fg_g; sb = fg_b;
        } else {
            rr = 255; rg = 235; rb = 200;
            sr2 = 210; sg = 220; sb = 240;
        }
        wl_suntime(fb, 8,  /*rising=*/true,  rs, rr,  rg,  rb);
        wl_suntime(fb, 84, /*rising=*/false, ss, sr2, sg,  sb);
    }

    display_show_digit(tube, fb, LCD_WIDTH, LCD_HEIGHT);
}

/* Build the ordered list of WeatherLive panel kinds enabled for a tube; returns
 * the count (≥1, falls back to weekday/date).  The TEMP panel already carries
 * today's high/low, so there is no separate Hi/Lo entry. */
static int wl_panel_list(bool wd, bool tp, bool sr, bool hm, bool wn, bool ht, int out[6])
{
    int n = 0;
    if (wd) out[n++] = WLP_WEEKDATE;
    if (tp) out[n++] = WLP_TEMP;
    if (sr) out[n++] = WLP_SUNRISE;
    if (hm) out[n++] = WLP_HUMIDITY;
    if (wn) out[n++] = WLP_WIND;
    if (ht) out[n++] = WLP_INDOOR_HT;
    if (n == 0) out[n++] = WLP_WEEKDATE;
    return n;
}

/* WeatherLive clock render: animated time-of-day sky panorama — sun/moon arc,
 * drifting clouds, and condition-driven rain/snow — with HH:MM on tubes 0–4 and
 * a rotating tube-6 panel (weekday+DD+MM ↔ current temp over today's high/low).
 * Runs at the fast (20 Hz) display tick. */
static void render_weatherlive(const nextube_config_t *cfg, const struct tm *t, bool demo)
{
    /* Safe loop exit for OTA / WebUI updaters: WeatherLive's realtime animation
     * keeps the display task hot every tick.  If a cooperative park has been
     * requested (display_show_wait → s_park_req), skip the whole heavy frame so
     * the task reaches its loop boundary and parks immediately, freeing the SPI
     * bus and CPU for the flash/serve path.  Mid-frame checks below bail out of
     * a frame already in progress.
     *
     * The same early-out also honours a short-lived busy hint (display_busy_hint)
     * raised around CPU/flash-bound web operations such as a config save, so the
     * httpd task on the same core isn't starved by this every-tick render. */
    if (s_park_req || esp_timer_get_time() < s_busy_until_us) return;

    /* Configure per-frame render state from cfg. Both WeatherLive and Custom
     * faces use the user's color/shadow prefs; only Custom can override the
     * background (WeatherLive always uses the animated sky). */
    s_wl_is_custom = (strcmp(cfg->clock_face, "custom") == 0);
    s_wl_glyph_r  = cfg->custom_glyph_color[0];
    s_wl_glyph_g  = cfg->custom_glyph_color[1];
    s_wl_glyph_b  = cfg->custom_glyph_color[2];
    s_wl_font_r   = cfg->custom_font_color[0];
    s_wl_font_g   = cfg->custom_font_color[1];
    s_wl_font_b   = cfg->custom_font_color[2];
    s_wl_shadow   = cfg->custom_shadow;
    s_wl_shadow_r  = cfg->custom_shadow_color[0];
    s_wl_shadow_g  = cfg->custom_shadow_color[1];
    s_wl_shadow_b  = cfg->custom_shadow_color[2];
    wl_refresh_ft_face(cfg->custom_font);
    if (s_wl_is_custom) {
        strncpy(s_wl_bg_theme, cfg->custom_bg, sizeof(s_wl_bg_theme) - 1);
        s_wl_bg_theme[sizeof(s_wl_bg_theme) - 1] = '\0';
    } else {
        s_wl_bg_theme[0] = '\0';
    }

    int64_t now_us = esp_timer_get_time();
    int mins = t->tm_hour * 60 + t->tm_min;

    /* Demo mode: accelerate to a full day every 60 s and auto-cycle every
     * weather condition (10 s each) so the whole WeatherLive repertoire shows
     * unattended.  Selected via the "WeatherLive Demo" theme. */
    const char *demo_ic = NULL;
    if (demo) {
        float vday = fmodf((float)now_us / 1000000.0f / 60.0f, 1.0f);   /* 0..1 per 60 s */
        mins = (int)(vday * 1440.0f) % 1440;
        static const char *const DEMO[8] = {
            "sun", "fewClouds", "overcastClouds", "fog",
            "rain", "squalls", "thunderstorm", "snow" };
        demo_ic = DEMO[(int)((now_us / 10000000LL) % 8)];               /* 10 s each */
    }

    /* Static mode freezes the animation clock and precipitation so the scene is
     * a still snapshot, redrawn only when the clock changes (the display task
     * also drops to the slow tick).  Demo always animates. */
    bool animate = demo || cfg->wlive_animate;

    wl_scene_t sc;

    /* Geocoded sunrise/sunset (NOAA solar_calc), fallback 6:00/19:00 until a
     * location is known or on polar day/night.  Drives BOTH the sky palette and
     * the sun/moon arc, so the day length tracks the real season/location.
     * (In demo mode `mins` is the accelerated virtual clock, but the sun events
     *  stay real, so the sky still cycles correctly across them.) */
    int SUNRISE = 360, SUNSET = 1140;
    float lat = 0.0f, lon = 0.0f;
    bool  have_loc = weather_get_location(&lat, &lon);
    if (have_loc) {
        int rise = -1, set = -1;
        solar_calc(lat, lon, t, &rise, &set);
        if (rise >= 0 && set >= 0 && set > rise) { SUNRISE = rise; SUNSET = set; }
    }

    int top[3], hor[3];
    wl_sky_palette(mins, SUNRISE, SUNSET, top, hor);
    sc.tr = top[0]; sc.tg = top[1]; sc.tb = top[2];
    sc.hr = hor[0]; sc.hg = hor[1]; sc.hb = hor[2];

    /* Night factor (0..255) for the stars: fully dark outside the ±TW twilight
     * windows, ramping to 0 across dawn/dusk so stars fade in/out with the sky.
     * Mirrors wl_sky_palette's TW window. */
    {
        const int TW = 55;
        int srA = SUNRISE - TW, srB = SUNRISE + TW;
        int ssA = SUNSET  - TW, ssB = SUNSET  + TW;
        if      (mins < srA || mins >= ssB) sc.night = 255;          /* deep night */
        else if (mins < srB) sc.night = 255 * (srB - mins) / (srB - srA);  /* dawn */
        else if (mins < ssA) sc.night = 0;                           /* full day   */
        else                 sc.night = 255 * (mins - ssA) / (ssB - ssA);  /* dusk */
        if (sc.night < 0)   sc.night = 0;
        if (sc.night > 255) sc.night = 255;
    }

    /* Sun/moon arc — same SUNRISE/SUNSET; X sweeps the 6-tube panorama, Y arcs
     * with a parabola (highest at mid-span); at night a moon traces the same
     * path. */

    /* Virtual canvas spans all six active areas + the five inter-tube gaps, so
     * the arc sweeps the true physical width (left edge of tube 0 → right edge
     * of tube 5). */
    const int PANO_W    = LCD_COUNT * LCD_WIDTH + (LCD_COUNT - 1) * WL_GAP_PX;
    const int HORIZON_Y = 118;                        /* arc baseline row       */

    sc.body_show    = false;
    sc.body_is_moon = false;
    sc.body_r       = 15;

    if (mins >= SUNRISE && mins < SUNSET) {
        /* Daytime: the sun arcs from sunrise (left) to sunset (right). */
        float frac = (float)(mins - SUNRISE) / (float)(SUNSET - SUNRISE);
        int   arc  = (int)(80.0f * 4.0f * frac * (1.0f - frac));
        sc.body_show = true;
        sc.body_x = (int)(frac * PANO_W);
        sc.body_y = HORIZON_Y - arc;
        sc.br = 255; sc.bg = 228; sc.bb = 120;        /* warm sun */
    } else {
        /* Night: real moon phase, positioned by phase relative to the sun.
         * A full moon transits at solar midnight (opposite the sun); a new moon
         * transits near noon (so it's absent from the night sky); the quarters
         * transit near dusk/dawn.  Moonrise/set ≈ transit ± 6.2 h. */
        float phase = wl_moon_phase(t);
        sc.moon_term   = cosf(2.0f * (float)M_PI * phase);
        sc.moon_waxing = (phase <= 0.5f);
        if (lat < 0.0f) sc.moon_waxing = !sc.moon_waxing;   /* S-hemisphere mirror */

        int solar_noon = (SUNRISE + SUNSET) / 2;
        int transit    = (solar_noon + (int)(phase * 1440.0f)) % 1440;
        const int HALF_UP = 372;                      /* ~6.2 h either side */
        int mrise = (transit - HALF_UP + 1440) % 1440;
        int since = (mins - mrise + 1440) % 1440;     /* minutes into the up-window */
        if (since < 2 * HALF_UP) {                    /* moon above the horizon */
            float mfrac = (float)since / (float)(2 * HALF_UP);
            int   arc   = (int)(80.0f * 4.0f * mfrac * (1.0f - mfrac));
            sc.body_show    = true;
            sc.body_is_moon = true;
            sc.body_x = (int)(mfrac * PANO_W);
            sc.body_y = HORIZON_Y - arc;
            sc.br = 230; sc.bg = 234; sc.bb = 248;     /* pale moon */
        }
        /* else: moon below the horizon → empty night sky (e.g. around new moon) */
    }

    /* ── Animation clock (continuous) + frame delta ──────────────────────── */
    float dt = (s_wl_last_us != 0) ? (now_us - s_wl_last_us) / 1000000.0f : 0.05f;
    if (dt < 0)     dt = 0.0f;
    if (dt > 0.25f) dt = 0.25f;        /* clamp after a suspend/pause */
    s_wl_last_us = now_us;
    sc.anim_t = animate ? (float)now_us / 1000000.0f : 0.0f;   /* frozen when static */

    /* ── Condition → cloud density + precipitation ───────────────────────── */
    const weather_data_t *w = weather_get();
    const char *ic = demo_ic ? demo_ic : ((w && w->valid) ? w->icon : "");
    sc.precip = 0; sc.ncloud = 1; sc.ca = 110;             /* default: clear, one wisp */
    sc.cr = 245; sc.cg = 248; sc.cb = 255;
    if      (!strcmp(ic, "fewClouds"))      { sc.ncloud = 2; sc.ca = 150; }
    else if (!strcmp(ic, "overcastClouds")) { sc.ncloud = 6; sc.ca = 200; sc.cr = 200; sc.cg = 205; sc.cb = 215; }
    else if (!strcmp(ic, "fog"))            { sc.ncloud = 6; sc.ca = 150; sc.cr = 205; sc.cg = 208; sc.cb = 214; }
    else if (!strcmp(ic, "rain"))           { sc.ncloud = 5; sc.ca = 190; sc.cr = 170; sc.cg = 178; sc.cb = 190; sc.precip = 1; }
    else if (!strcmp(ic, "squalls"))        { sc.ncloud = 5; sc.ca = 200; sc.cr = 160; sc.cg = 168; sc.cb = 182; sc.precip = 1; }
    else if (!strcmp(ic, "thunderstorm"))   { sc.ncloud = 6; sc.ca = 210; sc.cr = 140; sc.cg = 146; sc.cb = 160; sc.precip = 1; }
    else if (!strcmp(ic, "snow"))           { sc.ncloud = 5; sc.ca = 190; sc.cr = 210; sc.cg = 215; sc.cb = 225; sc.precip = 2; }

    /* ── Wind (≈50 km/h saturates) drives cloud drift, gusts and rain slant ── */
    sc.wind = w->wind_kph / 50.0f;
    if (sc.wind < 0.0f)      sc.wind = 0.0f;
    else if (sc.wind > 1.0f) sc.wind = 1.0f;

    /* ── Lightning: random flashes during thunderstorms ──────────────────── */
    sc.flash = 0.0f;
    {
        static int64_t s_flash_until = 0, s_next_flash = 0;
        if (!strcmp(ic, "thunderstorm")) {
            if (now_us >= s_next_flash) {                 /* schedule the next strike */
                s_flash_until = now_us + (60 + (int)(esp_random() % 140)) * 1000LL;   /* 60–200 ms */
                s_next_flash  = now_us + (2500 + (int)(esp_random() % 7000)) * 1000LL; /* 2.5–9.5 s */
            }
            if (now_us < s_flash_until) {
                float rem = (float)(s_flash_until - now_us) / 1000.0f;   /* ms left → fade out */
                sc.flash = rem / 180.0f;
                if (sc.flash > 1.0f) sc.flash = 1.0f;
            }
        } else {
            s_next_flash = 0;   /* reset so a fresh storm flashes promptly */
        }
    }

    /* Mirror the lightning flash onto the accent LEDs when the user has enabled
     * the weather override; always push 0 when disabled so toggling it off
     * (even mid-strike) restores the normal accent immediately. */
    leds_weather_flash(cfg->led_weather_override ? (uint8_t)(sc.flash * 255.0f) : 0);

    /* ── Particle field: spawn once, then advance once per frame ──────────── */
    if (!s_wl_part_init) {
        for (int i = 0; i < WL_NPART; i++) {
            int pt = esp_random() % LCD_COUNT;
            s_wl_part[i].x = (float)(pt * WL_TUBE_STRIDE + (int)(esp_random() % LCD_WIDTH));
            s_wl_part[i].y = (float)(esp_random() % LCD_HEIGHT);
            s_wl_part[i].vy = 0;
            s_wl_part[i].drift = (int8_t)((esp_random() % 3) - 1);
        }
        s_wl_part_init = true;
    }
    if (animate && sc.precip) {
        float vy = (sc.precip == 2) ? 32.0f : 165.0f;     /* snow slow, rain fast (px/s) */
        for (int i = 0; i < WL_NPART; i++) {
            s_wl_part[i].y += vy * dt;
            if (sc.precip == 2)                            /* snow blows sideways with wind */
                s_wl_part[i].x += ((float)s_wl_part[i].drift * 6.0f + sc.wind * 34.0f) * dt;
            if (s_wl_part[i].y >= LCD_HEIGHT) {            /* recycle onto a random panel top */
                int pt = esp_random() % LCD_COUNT;
                s_wl_part[i].x = (float)(pt * WL_TUBE_STRIDE + (int)(esp_random() % LCD_WIDTH));
                s_wl_part[i].y -= LCD_HEIGHT;
                s_wl_part[i].drift = (int8_t)((esp_random() % 3) - 1);
            }
        }
    }

    int h = t->tm_hour, m = t->tm_min;
    bool pm = (h >= 12);
    if (strcmp(cfg->time_type, "12H") == 0) { h %= 12; if (h == 0) h = 12; }

    /* Unit-converted temp + today's range for the info panels. */
    bool fahr = (strcmp(cfg->temp_format, "Fahrenheit") == 0);
    int  temp_disp = (int)lroundf(to_display_temp(w->temp_c,    fahr));
    int  dmin_disp = (int)lroundf(to_display_temp(w->day_min_c, fahr));
    int  dmax_disp = (int)lroundf(to_display_temp(w->day_max_c, fahr));

    /* Indoor sensor (SHT30) — pre-converted to match the outdoor unit.
     * indoor_hum = -1 signals absent or not-yet-valid sensor. */
    int indoor_temp_disp = 0, indoor_hum = -1;
    {
        const sht30_reading_t *sht = sht30_get();
        if (sht && sht->valid) {
            indoor_temp_disp = (int)lroundf(to_display_temp(sht->temp_c, fahr));
            indoor_hum = (int)lroundf(sht->humidity);
            if (indoor_hum > 99) indoor_hum = 99;
            if (indoor_hum < 0)  indoor_hum = 0;
        }
    }

    bool date_us = (strcmp(cfg->date_format, "MM/DD/YY") == 0);  /* month-over-day */
    const char *wind_unit = cfg->wind_unit;
    bool is_24cx = (strcmp(cfg->time_type, "24H_CX") == 0);
    bool is_12h  = (strcmp(cfg->time_type, "12H")    == 0);
    bool is_24   = !is_12h && !is_24cx && (strcmp(cfg->time_type, "24H") == 0);
    bool dual    = is_24cx && cfg->cx_dual_panel;
    int  rot_ms  = (cfg->tube6_panel_ms >= 1000) ? cfg->tube6_panel_ms : 5000;
    int64_t rot_us = (int64_t)rot_ms * 1000;

    char hi_digit = (h / 10) ? (char)('0' + h / 10)
                             : (cfg->leading_zero ? '0' : ' ');

    /* Tube-6 panel kinds for this device (WeatherLive subset).  Default layout
     * (non-CX) rotates weekday/date → temp → lo/hi on tube 6 every ~rot_ms. */
    /* Panel-list cache — only rebuilt when the config flags change. */
    static int  s_l6[6], s_n6, s_l5[6], s_n5;
    static bool s_t6wd, s_t6tp, s_t6sr, s_t6hm, s_t6wn, s_t6ht;
    static bool s_t5wd, s_t5tp, s_t5sr, s_t5hm, s_t5wn, s_t5ht;
    static bool s_pl_init;
    bool t6wd = cfg->tube6_panel_weekdate, t6tp = cfg->tube6_panel_temp,
         t6sr = cfg->tube6_panel_sunrise,  t6hm = cfg->tube6_panel_humidity,
         t6wn = cfg->tube6_panel_wind,     t6ht = cfg->tube6_panel_ht;
    bool t5wd = cfg->tube5_panel_weekdate, t5tp = cfg->tube5_panel_temp,
         t5sr = cfg->tube5_panel_sunrise,  t5hm = cfg->tube5_panel_humidity,
         t5wn = cfg->tube5_panel_wind,     t5ht = cfg->tube5_panel_ht;
    if (!s_pl_init ||
        t6wd!=s_t6wd || t6tp!=s_t6tp || t6sr!=s_t6sr ||
        t6hm!=s_t6hm || t6wn!=s_t6wn || t6ht!=s_t6ht ||
        t5wd!=s_t5wd || t5tp!=s_t5tp || t5sr!=s_t5sr ||
        t5hm!=s_t5hm || t5wn!=s_t5wn || t5ht!=s_t5ht) {
        s_n6 = is_24cx
            ? wl_panel_list(t6wd, t6tp, t6sr, t6hm, t6wn, t6ht, s_l6)
            : (s_l6[0] = WLP_WEEKDATE, s_l6[1] = WLP_TEMP, s_l6[2] = WLP_SUNRISE, 3);
        s_n5 = wl_panel_list(t5wd, t5tp, t5sr, t5hm, t5wn, t5ht, s_l5);
        s_t6wd=t6wd; s_t6tp=t6tp; s_t6sr=t6sr; s_t6hm=t6hm; s_t6wn=t6wn; s_t6ht=t6ht;
        s_t5wd=t5wd; s_t5tp=t5tp; s_t5sr=t5sr; s_t5hm=t5hm; s_t5wn=t5wn; s_t5ht=t5ht;
        s_pl_init = true;
    }
    int k6 = s_l6[(int)((now_us / rot_us) % s_n6)];

    /* Yield ~1 ms (FreeRTOS tick is 1 kHz) after each tube so httpd (which runs
     * below the display task on the same core) gets a window each frame instead
     * of being starved through the whole 6-tube push.  After each yield, bail
     * out immediately if a park was requested mid-frame (OTA / WebUI updater),
     * leaving the remaining tubes for the wait-screen — see s_park_req above. */
    s_wl_last_scene  = sc;
    s_wl_scene_valid = true;

    #define WL_YIELD()  do { vTaskDelay(1); if (s_park_req) return; } while (0)
    if (dual) {
        /* H H M M [p5][p6] — colon dropped, minutes shift left to tubes 2 & 3. */
        wl_draw_tube(0, hi_digit,             &sc);  WL_YIELD();
        wl_draw_tube(1, (char)('0' + h % 10), &sc);  WL_YIELD();
        wl_draw_tube(2, (char)('0' + m / 10), &sc);  WL_YIELD();
        wl_draw_tube(3, (char)('0' + m % 10), &sc);  WL_YIELD();
        int k5 = s_l5[(int)((now_us / rot_us) % s_n5)];
        wl_draw_panel(4, &sc, t, cfg->language, date_us, wind_unit, k5, temp_disp, w->day_range_valid,
                      dmin_disp, dmax_disp, SUNRISE, SUNSET, indoor_temp_disp, indoor_hum);  WL_YIELD();
        wl_draw_panel(5, &sc, t, cfg->language, date_us, wind_unit, k6, temp_disp, w->day_range_valid,
                      dmin_disp, dmax_disp, SUNRISE, SUNSET, indoor_temp_disp, indoor_hum);  WL_YIELD();
    } else if (is_12h) {
        /* H H : M M AM/PM */
        wl_draw_tube(0, hi_digit,                          &sc);  WL_YIELD();
        wl_draw_tube(1, (char)('0' + h % 10),              &sc);  WL_YIELD();
        wl_render_colon_tube(t, &sc);                              WL_YIELD();
        wl_draw_tube(3, (char)('0' + m / 10),              &sc);  WL_YIELD();
        wl_draw_tube(4, (char)('0' + m % 10),              &sc);  WL_YIELD();
        {
            uint8_t *fb = wl_fb();
            if (fb) {
                wl_paint_background(fb, 5, &sc);
                wl_text(fb, 40,  65, u8g2_font_logisoso28_tf, pm ? "P" : "A", s_wl_font_r, s_wl_font_g, s_wl_font_b, 0);
                wl_text(fb, 40, 119, u8g2_font_logisoso28_tf, "M",             s_wl_font_r, s_wl_font_g, s_wl_font_b, 0);
                display_show_digit(5, fb, LCD_WIDTH, LCD_HEIGHT);
            }
        }
        WL_YIELD();
    } else if (is_24) {
        /* H H M M S S — no colon, seconds on tubes 4 & 5, no panel */
        wl_draw_tube(0, hi_digit,                        &sc);  WL_YIELD();
        wl_draw_tube(1, (char)('0' + h % 10),            &sc);  WL_YIELD();
        wl_draw_tube(2, (char)('0' + m / 10),            &sc);  WL_YIELD();
        wl_draw_tube(3, (char)('0' + m % 10),            &sc);  WL_YIELD();
        wl_draw_tube(4, (char)('0' + t->tm_sec / 10),   &sc);  WL_YIELD();
        wl_draw_tube(5, (char)('0' + t->tm_sec % 10),   &sc);  WL_YIELD();
    } else {
        /* H H : M M [p6] — 24H_NS / 24H_CX single: single info panel on tube 6. */
        wl_draw_tube(0, hi_digit,                          &sc);  WL_YIELD();
        wl_draw_tube(1, (char)('0' + h % 10),              &sc);  WL_YIELD();
        wl_render_colon_tube(t, &sc);                              WL_YIELD();
        wl_draw_tube(3, (char)('0' + m / 10),              &sc);  WL_YIELD();
        wl_draw_tube(4, (char)('0' + m % 10),              &sc);  WL_YIELD();
        wl_draw_panel(5, &sc, t, cfg->language, date_us, wind_unit, k6, temp_disp, w->day_range_valid,
                      dmin_disp, dmax_disp, SUNRISE, SUNSET, indoor_temp_disp, indoor_hum);    WL_YIELD();
    }
    #undef WL_YIELD
}

static void render_clock(const nextube_config_t *cfg, const struct tm *t)
{
    if (strcmp(cfg->clock_face, "custom") == 0 ||
            strncmp(cfg->theme, "WeatherLive", 11) == 0) {
        bool demo = strcmp(cfg->theme, "WeatherLive Demo") == 0;
        render_weatherlive(cfg, t, demo);
        return;
    }

    bool is_12h  = (strcmp(cfg->time_type, "12H")    == 0);
    bool is_24ns = (strcmp(cfg->time_type, "24H_NS") == 0);
    bool is_24cx = (strcmp(cfg->time_type, "24H_CX") == 0);
    bool is_flip = (strcmp(cfg->theme, "FlipClock")  == 0);
    /* Colon blinks every other second on all themes except FlipClock,
     * which has its own flip animation and always shows a solid colon. */
    const char *colon_img = (is_flip || t->tm_sec % 2 == 0) ? "colon" : "blank";
    int h = t->tm_hour, m = t->tm_min, s = t->tm_sec;

    if (is_12h) {
        bool pm = (h >= 12);
        h = h % 12;
        if (h == 0) h = 12;
        /* tubes: H1  H2  colon  M1  M2  AM/PM  (no seconds in 12H) */
        if (h / 10 == 0) {
            if (cfg->leading_zero)
                display_show_number(0, 0,     cfg->theme);
            else
                display_show_ampm  (0, "blank", cfg->theme);
        } else {
            display_show_number(0, h / 10,    cfg->theme);
        }
        display_show_number(1, h % 10,        cfg->theme);
        display_show_ampm  (2, colon_img,     cfg->theme);
        display_show_number(3, m / 10,        cfg->theme);
        display_show_number(4, m % 10,        cfg->theme);
        display_show_ampm  (5, pm ? "pm" : "am", cfg->theme);
    } else if (is_24ns || is_24cx) {
        /* 24H Custom dual-panel: H1 H2 M1 M2 [p5][p6] — colon dropped, minutes
         * shift left to tubes 2 & 3, and tubes 4 & 5 are independent info panels
         * drawn by render_cx_panel().  Otherwise: H1 H2 : M1 M2 [tube5]. */
        bool dual_cx = is_24cx && cfg->cx_dual_panel;
        if (h / 10 == 0) {
            if (cfg->leading_zero)
                display_show_number(0, 0,     cfg->theme);
            else
                display_show_ampm  (0, "blank", cfg->theme);
        } else {
            display_show_number(0, h / 10,    cfg->theme);
        }
        display_show_number(1, h % 10,        cfg->theme);
        if (dual_cx) {
            display_show_number(2, m / 10,    cfg->theme);
            display_show_number(3, m % 10,    cfg->theme);
            /* tubes 4 & 5 handled by render_cx_panel() — leave alone */
        } else {
            display_show_ampm  (2, colon_img, cfg->theme);
            display_show_number(3, m / 10,    cfg->theme);
            display_show_number(4, m % 10,    cfg->theme);
            if (is_24ns) {
                if (strcmp(cfg->clock_tube5, "weather") == 0) {
                    const weather_data_t *w = weather_get();
                    if (w && w->valid && w->icon[0] != '\0') {
                        char path[128];
                        display_path_weather(path, sizeof(path), cfg->theme, w->icon);
                        display_show_image(5, path);
                    } else {
                        display_show_ampm(5, "blank", cfg->theme);
                    }
                } else {
                    display_show_ampm(5, "blank", cfg->theme);
                }
            }
            /* is_24cx single-panel: tube 5 handled by render_cx_panel() */
        }
    } else {
        /* 24H: all six tubes = H1 H2 M1 M2 S1 S2 (no colon tube) */
        if (!cfg->leading_zero && h / 10 == 0)
            display_show_ampm  (0, "blank", cfg->theme);
        else
            display_show_number(0, h / 10,  cfg->theme);
        display_show_number(1, h % 10,  cfg->theme);
        display_show_number(2, m / 10,  cfg->theme);
        display_show_number(3, m % 10,  cfg->theme);
        display_show_number(4, s / 10,  cfg->theme);
        display_show_number(5, s % 10,  cfg->theme);
    }
}

/* Build a static WeatherLive scene snapshot from the current time + weather
 * data.  Called by non-clock app modes (social media, etc.) that need the
 * animated sky background when render_weatherlive() is not running.
 * No-op if a valid scene is already cached. */
static void wl_ensure_scene(const nextube_config_t *cfg)
{
    if (s_wl_scene_valid) {
        /* Always advance anim_t so cloud/star animation stays smooth at full
         * frame rate regardless of how often the scene is rebuilt below. */
        if (cfg->wlive_animate)
            s_wl_last_scene.anim_t = (float)esp_timer_get_time() / 1000000.0f;

        /* Throttle the expensive rebuild (solar_calc, sky palette, weather_get)
         * to 1 Hz.  Sky gradient, sun/moon position and cloud type all change on
         * minute timescales; rebuilding them at animation rate (10–20 Hz) wastes
         * CPU and repeatedly locks the weather-fetch mutex, stalling the HTTP
         * stack — visible as weather-data lag and countdown/pomodoro value jumps. */
        static int64_t s_ensure_slow_us = 0;
        int64_t s_now_us = esp_timer_get_time();
        if (s_now_us - s_ensure_slow_us < 1000000LL)
            return;
        s_ensure_slow_us = s_now_us;

        /* Refresh time-of-day fields (gradient, sun/moon arc, night factor) so
         * the sky tracks real time in non-clock modes — same logic as
         * render_weatherlive's per-frame rebuild.  Cloud layout stays fixed. */
        time_t now_sec = time(NULL);
        struct tm lt; localtime_r(&now_sec, &lt);
        int mins = lt.tm_hour * 60 + lt.tm_min;

        int SUNRISE = 360, SUNSET = 1140;
        float lat = 0.0f, lon = 0.0f;
        if (weather_get_location(&lat, &lon)) {
            int rise = -1, set = -1;
            solar_calc(lat, lon, &lt, &rise, &set);
            if (rise >= 0 && set >= 0 && set > rise) { SUNRISE = rise; SUNSET = set; }
        }

        int top3[3], hor3[3];
        wl_sky_palette(mins, SUNRISE, SUNSET, top3, hor3);
        s_wl_last_scene.tr = top3[0]; s_wl_last_scene.tg = top3[1]; s_wl_last_scene.tb = top3[2];
        s_wl_last_scene.hr = hor3[0]; s_wl_last_scene.hg = hor3[1]; s_wl_last_scene.hb = hor3[2];

        {   const int TW = 55;
            int srA = SUNRISE - TW, srB = SUNRISE + TW;
            int ssA = SUNSET  - TW, ssB = SUNSET  + TW;
            int night;
            if      (mins < srA || mins >= ssB) night = 255;
            else if (mins < srB) night = 255 * (srB - mins) / (srB - srA);
            else if (mins < ssA) night = 0;
            else                 night = 255 * (mins - ssA) / (ssB - ssA);
            if (night < 0) night = 0;
            if (night > 255) night = 255;
            s_wl_last_scene.night = night;
        }

        const int PANO_W    = LCD_COUNT * LCD_WIDTH + (LCD_COUNT - 1) * WL_GAP_PX;
        const int HORIZON_Y = 118;
        s_wl_last_scene.body_show = false; s_wl_last_scene.body_is_moon = false; s_wl_last_scene.body_r = 15;
        if (mins >= SUNRISE && mins < SUNSET) {
            float frac = (float)(mins - SUNRISE) / (float)(SUNSET - SUNRISE);
            int arc = (int)(80.0f * 4.0f * frac * (1.0f - frac));
            s_wl_last_scene.body_show = true;
            s_wl_last_scene.body_x = (int)(frac * PANO_W);
            s_wl_last_scene.body_y = HORIZON_Y - arc;
            s_wl_last_scene.br = 255; s_wl_last_scene.bg = 228; s_wl_last_scene.bb = 120;
        } else {
            float phase = wl_moon_phase(&lt);
            s_wl_last_scene.moon_term   = cosf(2.0f * (float)M_PI * phase);
            s_wl_last_scene.moon_waxing = (phase <= 0.5f);
            if (lat < 0.0f) s_wl_last_scene.moon_waxing = !s_wl_last_scene.moon_waxing;
            int solar_noon = (SUNRISE + SUNSET) / 2;
            int transit    = (solar_noon + (int)(phase * 1440.0f)) % 1440;
            const int HALF_UP = 372;
            int mrise = (transit - HALF_UP + 1440) % 1440;
            int since = (mins - mrise + 1440) % 1440;
            if (since < 2 * HALF_UP) {
                float mfrac = (float)since / (float)(2 * HALF_UP);
                int arc = (int)(80.0f * 4.0f * mfrac * (1.0f - mfrac));
                s_wl_last_scene.body_show = true; s_wl_last_scene.body_is_moon = true;
                s_wl_last_scene.body_x = (int)(mfrac * PANO_W);
                s_wl_last_scene.body_y = HORIZON_Y - arc;
                s_wl_last_scene.br = 230; s_wl_last_scene.bg = 234; s_wl_last_scene.bb = 248;
            }
        }

        {
            const weather_data_t *wdat = weather_get();
            const char *ic = (wdat && wdat->valid) ? wdat->icon : "";
            s_wl_last_scene.precip = 0; s_wl_last_scene.ncloud = 1; s_wl_last_scene.ca = 110;
            s_wl_last_scene.cr = 245; s_wl_last_scene.cg = 248; s_wl_last_scene.cb = 255;
            s_wl_last_scene.flash = 0.0f;
            if      (!strcmp(ic, "fewClouds"))      { s_wl_last_scene.ncloud = 2; s_wl_last_scene.ca = 150; }
            else if (!strcmp(ic, "overcastClouds")) { s_wl_last_scene.ncloud = 6; s_wl_last_scene.ca = 200; s_wl_last_scene.cr = 200; s_wl_last_scene.cg = 205; s_wl_last_scene.cb = 215; }
            else if (!strcmp(ic, "fog"))            { s_wl_last_scene.ncloud = 6; s_wl_last_scene.ca = 150; s_wl_last_scene.cr = 205; s_wl_last_scene.cg = 208; s_wl_last_scene.cb = 214; }
            else if (!strcmp(ic, "rain"))           { s_wl_last_scene.ncloud = 5; s_wl_last_scene.ca = 190; s_wl_last_scene.cr = 170; s_wl_last_scene.cg = 178; s_wl_last_scene.cb = 190; s_wl_last_scene.precip = 1; }
            else if (!strcmp(ic, "squalls"))        { s_wl_last_scene.ncloud = 5; s_wl_last_scene.ca = 200; s_wl_last_scene.cr = 160; s_wl_last_scene.cg = 168; s_wl_last_scene.cb = 182; s_wl_last_scene.precip = 1; }
            else if (!strcmp(ic, "thunderstorm"))   { s_wl_last_scene.ncloud = 6; s_wl_last_scene.ca = 210; s_wl_last_scene.cr = 140; s_wl_last_scene.cg = 146; s_wl_last_scene.cb = 160; s_wl_last_scene.precip = 1; }
            else if (!strcmp(ic, "snow"))           { s_wl_last_scene.ncloud = 5; s_wl_last_scene.ca = 190; s_wl_last_scene.cr = 210; s_wl_last_scene.cg = 215; s_wl_last_scene.cb = 225; s_wl_last_scene.precip = 2; }
            s_wl_last_scene.wind = (wdat && wdat->valid) ? wdat->wind_kph / 50.0f : 0.0f;
            if (s_wl_last_scene.wind < 0.0f) s_wl_last_scene.wind = 0.0f;
            else if (s_wl_last_scene.wind > 1.0f) s_wl_last_scene.wind = 1.0f;
        }
        return;
    }

    time_t now_sec = time(NULL);
    struct tm lt; localtime_r(&now_sec, &lt);
    int mins = lt.tm_hour * 60 + lt.tm_min;

    int SUNRISE = 360, SUNSET = 1140;
    float lat = 0.0f, lon = 0.0f;
    if (weather_get_location(&lat, &lon)) {
        int rise = -1, set = -1;
        solar_calc(lat, lon, &lt, &rise, &set);
        if (rise >= 0 && set >= 0 && set > rise) { SUNRISE = rise; SUNSET = set; }
    }

    wl_scene_t sc; memset(&sc, 0, sizeof(sc));

    int top3[3], hor3[3];
    wl_sky_palette(mins, SUNRISE, SUNSET, top3, hor3);
    sc.tr = top3[0]; sc.tg = top3[1]; sc.tb = top3[2];
    sc.hr = hor3[0]; sc.hg = hor3[1]; sc.hb = hor3[2];

    {   const int TW = 55;
        int srA = SUNRISE - TW, srB = SUNRISE + TW;
        int ssA = SUNSET  - TW, ssB = SUNSET  + TW;
        if      (mins < srA || mins >= ssB) sc.night = 255;
        else if (mins < srB) sc.night = 255 * (srB - mins) / (srB - srA);
        else if (mins < ssA) sc.night = 0;
        else                 sc.night = 255 * (mins - ssA) / (ssB - ssA);
        if (sc.night < 0) sc.night = 0;
        if (sc.night > 255) sc.night = 255;
    }

    const int PANO_W    = LCD_COUNT * LCD_WIDTH + (LCD_COUNT - 1) * WL_GAP_PX;
    const int HORIZON_Y = 118;
    sc.body_show = false; sc.body_is_moon = false; sc.body_r = 15;
    if (mins >= SUNRISE && mins < SUNSET) {
        float frac = (float)(mins - SUNRISE) / (float)(SUNSET - SUNRISE);
        int arc = (int)(80.0f * 4.0f * frac * (1.0f - frac));
        sc.body_show = true;
        sc.body_x = (int)(frac * PANO_W);
        sc.body_y = HORIZON_Y - arc;
        sc.br = 255; sc.bg = 228; sc.bb = 120;
    } else {
        float phase = wl_moon_phase(&lt);
        sc.moon_term   = cosf(2.0f * (float)M_PI * phase);
        sc.moon_waxing = (phase <= 0.5f);
        if (lat < 0.0f) sc.moon_waxing = !sc.moon_waxing;
        int solar_noon = (SUNRISE + SUNSET) / 2;
        int transit    = (solar_noon + (int)(phase * 1440.0f)) % 1440;
        const int HALF_UP = 372;
        int mrise = (transit - HALF_UP + 1440) % 1440;
        int since = (mins - mrise + 1440) % 1440;
        if (since < 2 * HALF_UP) {
            float mfrac = (float)since / (float)(2 * HALF_UP);
            int arc = (int)(80.0f * 4.0f * mfrac * (1.0f - mfrac));
            sc.body_show = true; sc.body_is_moon = true;
            sc.body_x = (int)(mfrac * PANO_W);
            sc.body_y = HORIZON_Y - arc;
            sc.br = 230; sc.bg = 234; sc.bb = 248;
        }
    }

    sc.anim_t = (float)esp_timer_get_time() / 1000000.0f;
    const weather_data_t *wdat = weather_get();
    const char *ic = (wdat && wdat->valid) ? wdat->icon : "";
    sc.precip = 0; sc.ncloud = 1; sc.ca = 110;
    sc.cr = 245; sc.cg = 248; sc.cb = 255; sc.flash = 0.0f;
    if      (!strcmp(ic, "fewClouds"))      { sc.ncloud = 2; sc.ca = 150; }
    else if (!strcmp(ic, "overcastClouds")) { sc.ncloud = 6; sc.ca = 200; sc.cr = 200; sc.cg = 205; sc.cb = 215; }
    else if (!strcmp(ic, "fog"))            { sc.ncloud = 6; sc.ca = 150; sc.cr = 205; sc.cg = 208; sc.cb = 214; }
    else if (!strcmp(ic, "rain"))           { sc.ncloud = 5; sc.ca = 190; sc.cr = 170; sc.cg = 178; sc.cb = 190; sc.precip = 1; }
    else if (!strcmp(ic, "squalls"))        { sc.ncloud = 5; sc.ca = 200; sc.cr = 160; sc.cg = 168; sc.cb = 182; sc.precip = 1; }
    else if (!strcmp(ic, "thunderstorm"))   { sc.ncloud = 6; sc.ca = 210; sc.cr = 140; sc.cg = 146; sc.cb = 160; sc.precip = 1; }
    else if (!strcmp(ic, "snow"))           { sc.ncloud = 5; sc.ca = 190; sc.cr = 210; sc.cg = 215; sc.cb = 225; sc.precip = 2; }
    sc.wind = (wdat && wdat->valid) ? wdat->wind_kph / 50.0f : 0.0f;
    if (sc.wind < 0.0f) sc.wind = 0.0f; else if (sc.wind > 1.0f) sc.wind = 1.0f;

    s_wl_font_r = 255; s_wl_font_g = 255; s_wl_font_b = 255;
    s_wl_shadow = true;
    s_wl_shadow_r = 0; s_wl_shadow_g = 0; s_wl_shadow_b = 0;
    s_wl_bg_theme[0] = '\0';
    wl_refresh_ft_face(cfg->custom_font);

    s_wl_last_scene  = sc;
    s_wl_scene_valid = true;
}

/* Decoded-icon cache for png_composite_over_sky().  The platform/mode icon
 * composited over the live sky in wl_tube_icon() never changes within a mode,
 * but the sky beneath it animates every frame — so without a cache the PNG
 * would be inflated from SPIFFS flash 10–20×/second.  One decoded RGBA frame is
 * held, keyed by path; a mode switch (new path) replaces it.  Kept as RGBA so
 * the per-pixel alpha blend over the freshly painted sky is byte-identical to
 * the uncached path. */
static char           s_icon_cache_path[270] = "";
static unsigned char *s_icon_cache_rgba = NULL;   /* LCD_WIDTH*LCD_HEIGHT*4, PSRAM */

/* Decode a PNG from `path` and alpha-blend it into an already-painted sky fb.
 * Returns true on success. RGBA output from lodepng; proper per-pixel blend.
 * The decode is cached (see s_icon_cache_*): repeated calls with the same path
 * — i.e. every animated frame of one mode — skip the flash read and inflate. */
static bool png_composite_over_sky(uint8_t *fb, const char *path)
{
    const unsigned char *rgba = NULL;
    unsigned char *to_free = NULL;
    if (s_icon_cache_rgba && strcmp(s_icon_cache_path, path) == 0) {
        rgba = s_icon_cache_rgba;                 /* hit: no flash read / inflate */
    } else {
        unsigned char *dec = NULL;
        unsigned pw = 0, ph = 0;
        char _sp[270]; snprintf(_sp, sizeof(_sp), "/spiffs%s", path);
        if (lodepng_decode32_file(&dec, &pw, &ph, _sp) != 0
            || pw != (unsigned)LCD_WIDTH || ph != (unsigned)LCD_HEIGHT) {
            free(dec);
            return false;
        }
        if (!s_icon_cache_rgba)
            s_icon_cache_rgba = PSRAM_MALLOC((size_t)LCD_WIDTH * LCD_HEIGHT * 4);
        if (s_icon_cache_rgba) {
            memcpy(s_icon_cache_rgba, dec, (size_t)LCD_WIDTH * LCD_HEIGHT * 4);
            strncpy(s_icon_cache_path, path, sizeof(s_icon_cache_path) - 1);
            s_icon_cache_path[sizeof(s_icon_cache_path) - 1] = '\0';
            rgba = s_icon_cache_rgba;
            free(dec);
        } else {
            rgba = dec; to_free = dec;            /* alloc failed: use & free now */
        }
    }
    uint8_t *dst8 = fb;
    const unsigned char *src = rgba;
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++, src += 4) {
        uint8_t a = src[3];
        if (a == 0) continue;
        uint16_t px;
        if (a == 255) {
            px = ((uint16_t)(src[0] >> 3) << 11)
               | ((uint16_t)(src[1] >> 2) << 5)
               |  (uint16_t)(src[2] >> 3);
        } else {
            uint16_t bg16 = ((uint16_t)dst8[i * 2] << 8) | dst8[i * 2 + 1];
            int sky_r = ((bg16 >> 11) & 31) * 255 / 31;
            int sky_g = ((bg16 >>  5) & 63) * 255 / 63;
            int sky_b = ( bg16        & 31) * 255 / 31;
            int out_r = (src[0] * a + sky_r * (255 - a)) / 255;
            int out_g = (src[1] * a + sky_g * (255 - a)) / 255;
            int out_b = (src[2] * a + sky_b * (255 - a)) / 255;
            px = ((uint16_t)(out_r >> 3) << 11)
               | ((uint16_t)(out_g >> 2) << 5)
               |  (uint16_t)(out_b >> 3);
        }
        dst8[i * 2]     = (uint8_t)(px >> 8);
        dst8[i * 2 + 1] = (uint8_t)(px & 0xFF);
    }
    free(to_free);
    return true;
}

/* Paint sky for `tube`, then composite /images/system/{name}.png (with real
 * alpha) or fall back to .jpg black-key compositing if no PNG exists. */
static void wl_tube_icon(int tube, const char *name)
{
    if (!s_wl_scene_valid) { display_fill(tube, 0x0000); return; }
    uint8_t *fb = wl_fb();
    if (!fb) { display_fill(tube, 0x0000); return; }
    wl_paint_background(fb, tube, &s_wl_last_scene);

    char p[256];
    snprintf(p, sizeof(p), "/images/system/%s.png", name);
    if (!png_composite_over_sky(fb, p)) {
        /* PNG not present — fall back to JPEG with black-key compositing */
        snprintf(p, sizeof(p), "/images/system/%s.jpg", name);
        int iw = 0, ih = 0;
        const uint8_t *img = img_cache_get(p, &iw, &ih);
        if (img && iw == LCD_WIDTH && ih == LCD_HEIGHT) {
            const uint16_t *src = (const uint16_t *)img;
            uint16_t *dst = (uint16_t *)fb;
            for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
                uint16_t px = src[i];
                int r5 = (px >> 11) & 31, g6 = (px >> 5) & 63, b5 = px & 31;
                if ((r5 + (g6 >> 1) + b5) > 8) dst[i] = px;
            }
        }
    }
    display_show_digit(tube, fb, LCD_WIDTH, LCD_HEIGHT);
}

static void render_number6(uint32_t value, const nextube_config_t *cfg,
                           const char *icon_tube0, const char *suffix_tube5)
{
    bool wl = cx_is_wl_sky(cfg) && s_wl_scene_valid;
    const char *theme = cfg->theme;
    char ds[8];

    /* Tube 0: mode icon  |  tubes 1-4: digits  |  tube 5: suffix/blank */
    if (icon_tube0) {
        if (wl) wl_tube_icon(0, icon_tube0);
        else display_show_ampm(0, icon_tube0, theme);
    } else {
        uint8_t d0 = (value / 100000) % 10;
        if (wl) { snprintf(ds, sizeof(ds), "%d", d0); wl_tube_str(0, u8g2_font_logisoso46_tf, ds, 100); }
        else display_show_number(0, d0, theme);
    }

    static const uint32_t div4[4] = { 10000, 1000, 100, 10 };
    bool leading = true;
    for (int i = 0; i < 4; i++) {
        uint8_t d = (value / div4[i]) % 10;
        if (leading && d == 0) {
            if (wl) wl_tube_sky(i + 1);
            else display_show_ampm(i + 1, "blank", theme);
        } else {
            if (wl) { snprintf(ds, sizeof(ds), "%d", d); wl_tube_str(i + 1, u8g2_font_logisoso46_tf, ds, 100); }
            else display_show_number(i + 1, d, theme);
            leading = false;
        }
    }

    /* Tube 5: suffix symbol or units digit */
    if (suffix_tube5) {
        if (wl) {
            const char *sym = strcmp(suffix_tube5, "k-sub") == 0 ? "K" :
                              strcmp(suffix_tube5, "m-sub") == 0 ? "M" : suffix_tube5;
            wl_tube_str(5, u8g2_font_logisoso46_tf, sym, 100);
        } else {
            display_show_ampm(5, suffix_tube5, theme);
        }
    } else {
        if (wl) { snprintf(ds, sizeof(ds), "%d", (int)(value % 10)); wl_tube_str(5, u8g2_font_logisoso46_tf, ds, 100); }
        else display_show_number(5, value % 10, theme);
    }
}

/* Generic follower/subscriber renderer.
 * icon: AMPM asset name for tube 0 (e.g. "youtube", "instagram", "tiktok").
 * count: the follower/subscriber count to display.
 *
 * Decimal logic for K/M ranges — only when the integer part is a single digit
 * (fits in 4 tubes 2-5 alongside the dot, decimal digit(s), and suffix):
 *
 *   dec2 != 0  → 2 decimals  "N.DD K/M"  T1=int  T2=dot  T3=d1  T4=d2  T5=suffix
 *   dec1 != 0  → 1 decimal   "_.N.D K/M" T1=blank T2=int T3=dot T4=d1  T5=suffix
 *   both zero  → no decimal  "_._._.N K/M"                T4=int T5=suffix (blanks)
 *
 * 2+ digit K/M integers have no room for a decimal alongside the suffix,
 * so they fall through to render_number6 (3 significant digits, no dot). */
/* Show platform icon + 5 blank tubes when the account is not configured. */
static void render_followers_blank(const nextube_config_t *cfg, const char *icon)
{
    bool wl = cx_is_wl_sky(cfg);
    if (wl) wl_ensure_scene(cfg);
    if (wl && s_wl_scene_valid) {
        wl_tube_icon(0, icon);
        for (int t = 1; t <= 5; t++) wl_tube_sky(t);
    } else {
        display_show_ampm(0, icon, cfg->theme);
        for (int t = 1; t <= 5; t++) display_show_ampm(t, "blank", cfg->theme);
    }
}

static void render_followers(const nextube_config_t *cfg,
                             uint32_t count, const char *icon)
{
    bool wl = cx_is_wl_sky(cfg);
    if (wl) wl_ensure_scene(cfg);
    wl = wl && s_wl_scene_valid;
    char ds[8];

#define WL_ICON(t)           do { if (wl) wl_tube_icon(t, icon);                                              else display_show_ampm  (t, icon,    cfg->theme); } while(0)
#define WL_BLANK(t)          do { if (wl) wl_tube_sky(t);                                                     else display_show_ampm  (t, "blank", cfg->theme); } while(0)
#define WL_DOT(t)            do { if (wl) wl_tube_str(t, u8g2_font_logisoso46_tf, ".", 100);                  else display_show_ampm  (t, "dot",   cfg->theme); } while(0)
#define WL_DIGIT(t, d)       do { if (wl) { snprintf(ds,sizeof(ds),"%d",(int)(d)); wl_tube_str(t, u8g2_font_logisoso46_tf, ds, 100); } else display_show_number(t, (d), cfg->theme); } while(0)
#define WL_SUFFIX(t, sym, n) do { if (wl) wl_tube_str(t, u8g2_font_logisoso46_tf, sym, 100);                 else display_show_ampm  (t, n,       cfg->theme); } while(0)

    if (count >= 1000000) {
        uint32_t int_m = count / 1000000;
        if (int_m < 10) {
            uint32_t dec1 = (count % 1000000) / 100000;
            uint32_t dec2 = (count % 100000)  / 10000;
            WL_ICON(0);
            if (dec2) {
                WL_DIGIT(1, int_m); WL_DOT(2); WL_DIGIT(3, dec1); WL_DIGIT(4, dec2);
            } else if (dec1) {
                WL_BLANK(1); WL_DIGIT(2, int_m); WL_DOT(3); WL_DIGIT(4, dec1);
            } else {
                WL_BLANK(1); WL_BLANK(2); WL_BLANK(3); WL_DIGIT(4, int_m);
            }
            WL_SUFFIX(5, "M", "m-sub");
        } else {
            render_number6(count / 100000, cfg, icon, "m-sub");
        }
    } else if (count >= 1000) {
        uint32_t int_k = count / 1000;
        if (int_k < 10) {
            uint32_t dec1 = (count % 1000) / 100;
            uint32_t dec2 = (count % 100)  / 10;
            WL_ICON(0);
            if (dec2) {
                WL_DIGIT(1, int_k); WL_DOT(2); WL_DIGIT(3, dec1); WL_DIGIT(4, dec2);
            } else if (dec1) {
                WL_BLANK(1); WL_DIGIT(2, int_k); WL_DOT(3); WL_DIGIT(4, dec1);
            } else {
                WL_BLANK(1); WL_BLANK(2); WL_BLANK(3); WL_DIGIT(4, int_k);
            }
            WL_SUFFIX(5, "K", "k-sub");
        } else {
            render_number6(count / 100, cfg, icon, "k-sub");
        }
    } else {
        /* Raw count < 1 K — use all five digit tubes, suppress leading zeros */
        WL_ICON(0);
        static const uint32_t div5[5] = { 10000, 1000, 100, 10, 1 };
        bool leading = true;
        for (int i = 0; i < 5; i++) {
            uint8_t d = (count / div5[i]) % 10;
            if (leading && d == 0 && i < 4) { WL_BLANK(i + 1); }
            else { WL_DIGIT(i + 1, d); leading = false; }
        }
    }

#undef WL_ICON
#undef WL_BLANK
#undef WL_DOT
#undef WL_DIGIT
#undef WL_SUFFIX
}

static void render_countdown_display(const nextube_config_t *cfg,
                                     int32_t remaining_s)
{
    /* Honour OTA park and config-save busy hint — same guard as render_weatherlive. */
    if (s_park_req || esp_timer_get_time() < s_busy_until_us) return;
    if (remaining_s < 0) remaining_s = 0;
    int m = remaining_s / 60, s = remaining_s % 60;
    bool wl = cx_is_wl_sky(cfg);
    if (wl) wl_ensure_scene(cfg);
    wl = wl && s_wl_scene_valid;
    if (wl) {
        char ds[12];
        wl_tube_sky(0);                                                        vTaskDelay(1);
        snprintf(ds, sizeof(ds), "%d", m / 10); wl_tube_str(1, u8g2_font_logisoso46_tf, ds, 100); vTaskDelay(1);
        snprintf(ds, sizeof(ds), "%d", m % 10); wl_tube_str(2, u8g2_font_logisoso46_tf, ds, 100); vTaskDelay(1);
        wl_tube_str(3, u8g2_font_logisoso46_tf, ":", 100);                    vTaskDelay(1);
        snprintf(ds, sizeof(ds), "%d", s / 10); wl_tube_str(4, u8g2_font_logisoso46_tf, ds, 100); vTaskDelay(1);
        snprintf(ds, sizeof(ds), "%d", s % 10); wl_tube_str(5, u8g2_font_logisoso46_tf, ds, 100);
    } else {
        display_show_ampm(0, "countdown", cfg->theme);
        display_show_number(1, m / 10,  cfg->theme);
        display_show_number(2, m % 10,  cfg->theme);
        display_show_ampm  (3, "colon", cfg->theme);
        display_show_number(4, s / 10,  cfg->theme);
        display_show_number(5, s % 10,  cfg->theme);
    }
}

static void render_pomodoro_display(const nextube_config_t *cfg,
                                    int32_t remaining_s, bool in_break)
{
    /* Honour OTA park and config-save busy hint — same guard as render_weatherlive. */
    if (s_park_req || esp_timer_get_time() < s_busy_until_us) return;
    if (remaining_s < 0) remaining_s = 0;
    int m = remaining_s / 60, s = remaining_s % 60;
    bool wl = cx_is_wl_sky(cfg);
    if (wl) wl_ensure_scene(cfg);
    wl = wl && s_wl_scene_valid;
    if (wl) {
        char ds[12];
        wl_tube_sky(0);                                                        vTaskDelay(1);
        snprintf(ds, sizeof(ds), "%d", m / 10); wl_tube_str(1, u8g2_font_logisoso46_tf, ds, 100); vTaskDelay(1);
        snprintf(ds, sizeof(ds), "%d", m % 10); wl_tube_str(2, u8g2_font_logisoso46_tf, ds, 100); vTaskDelay(1);
        wl_tube_str(3, u8g2_font_logisoso46_tf, ":", 100);                    vTaskDelay(1);
        snprintf(ds, sizeof(ds), "%d", s / 10); wl_tube_str(4, u8g2_font_logisoso46_tf, ds, 100); vTaskDelay(1);
        wl_tube_sky(5);
    } else {
        display_show_ampm(0, "pomodoro", cfg->theme);
        display_show_number(1, m / 10, cfg->theme);
        display_show_number(2, m % 10, cfg->theme);
        display_show_ampm  (3, "colon", cfg->theme);
        display_show_number(4, s / 10, cfg->theme);
        display_show_ampm  (5, in_break ? "pomodorolb" : "pomodorosb", cfg->theme);
    }
}


/* ── Spectrum bar visualiser ─────────────────────────────────────────── *
 *                                                                         *
 * Each of the 6 ST7735 displays shows 4 segmented mini-bars (one per     *
 * Goertzel band), side by side.  Layout (80 px wide, 160 px tall):       *
 *                                                                         *
 *   Width:  1px pad | 18px bar | 2px gap | ... × 4 | 1px pad = 80 ✓     *
 *   Height: 3px top + 13 × (10px seg + 2px gap) − 2px + 3px bot = 160 ✓ *
 *                                                                         *
 * Colour: user-configurable single base colour (spectrum_lcd_RGB config). *
 * Brightness ramp 0.75→1.00 bottom-to-top for visual depth.              *
 * Peak dot: bright-white segment that holds then decays (~1 s at 20 Hz). *
 * Unlit segments: ~6% ghost so the full bar outline is always visible.    */
#define SPEC_SEGS           13   /* segments per bar                              */
#define SPEC_SEG_H          10   /* segment height (px)                           */
#define SPEC_GAP_H           2   /* gap between segments (px)                     */
#define SPEC_PAD_TOP         3   /* blank rows at top of screen                   */
#define SPEC_PAD_BOT         3   /* blank rows at bottom of screen                */
/* Total height: 3 + 13*(10+2) − 2 + 3 = 160 ✓ */

#define SPEC_BARS_PER_TUBE   4   /* mini-bars per tube                            */
#define SPEC_BAR_W          18   /* bar width (px)                                */
#define SPEC_BAR_GAP         2   /* horizontal gap between bars (px)              */
#define SPEC_BAR_PAD         1   /* outer left/right padding (px)                 */
/* Total width: 1 + (18+2)*4 − 2 + 1 = 80 ✓ */

_Static_assert(LCD_COUNT * SPEC_BARS_PER_TUBE == MIC_BAND_COUNT,
               "SPEC_BARS_PER_TUBE x LCD_COUNT must equal MIC_BAND_COUNT");

static float s_spec_peak[LCD_COUNT * SPEC_BARS_PER_TUBE];   /* visual peak hold per band */

/* Segment colour: user base colour with brightness ramp, or ghost at ~6%. */
static void spec_seg_color(int s, bool lit,
                            uint8_t br, uint8_t bg, uint8_t bb,
                            uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (lit) {
        /* Brightness ramp 0.75 -> 1.00 bottom-to-top for visual depth */
        float bright = 0.75f + 0.25f * ((float)s / (float)(SPEC_SEGS - 1));
        *r = (uint8_t)((float)br * bright);
        *g = (uint8_t)((float)bg * bright);
        *b = (uint8_t)((float)bb * bright);
    } else {
        /* Ghost: ~6% brightness so the full bar outline is always visible */
        *r = br >> 4;
        *g = bg >> 4;
        *b = bb >> 4;
    }
}

static void render_spectrum(const nextube_config_t *cfg)
{
    float bands[LCD_COUNT * SPEC_BARS_PER_TUBE];
    mic_get_bands(bands);

    uint8_t br = cfg->spectrum_lcd_rgb[0];
    uint8_t bg = cfg->spectrum_lcd_rgb[1];
    uint8_t bb = cfg->spectrum_lcd_rgb[2];

    /* Optional: follow the WLED primary colour (live).  Falls back to the
     * configured colour when WLED Sync isn't running / hasn't received a
     * packet, when the strip is off, or when the received colour is
     * near-black — palette effects (Rainbow, Fire, …) don't use col[0] and
     * WLED may leave it at (0,0,0), which would render invisible bars (the
     * same caveat the LED task handles via the fx field). */
    if (cfg->spectrum_lcd_wled) {
        wled_sync_state_t ws;
        if (wled_sync_get(&ws) && ws.on &&
            ((int)ws.r + (int)ws.g + (int)ws.b) >= 24) {
            br = ws.r; bg = ws.g; bb = ws.b;
        }
    }

    /* Update peak-hold and precompute lit segment count + peak dot for every band.
     * When mic_task has gated all bands to zero, decay the peak dots 5× faster
     * than normal so any "echo" (lingering peak dots from a suppressed noise blip)
     * clears in ~160 ms instead of ~700 ms. */
    bool bands_all_zero = true;
    for (int i = 0; i < LCD_COUNT * SPEC_BARS_PER_TUBE; i++)
        if (bands[i] > 0.0f) { bands_all_zero = false; break; }
    float peak_decay_step = bands_all_zero ? 0.25f : 0.05f;

    int  lit_count[LCD_COUNT * SPEC_BARS_PER_TUBE];
    int  peak_dot [LCD_COUNT * SPEC_BARS_PER_TUBE];
    bool peak_vis [LCD_COUNT * SPEC_BARS_PER_TUBE];
    for (int i = 0; i < LCD_COUNT * SPEC_BARS_PER_TUBE; i++) {
        float e = bands[i];
        if (e < 0.0f) e = 0.0f; else if (e > 1.0f) e = 1.0f;
        if (e >= s_spec_peak[i]) {
            s_spec_peak[i] = e;
        } else {
            s_spec_peak[i] -= peak_decay_step;
            if (s_spec_peak[i] < 0.0f) s_spec_peak[i] = 0.0f;
        }
        lit_count[i] = (int)(e              * (float)SPEC_SEGS       + 0.5f);
        if (lit_count[i] > SPEC_SEGS)  lit_count[i] = SPEC_SEGS;
        peak_dot [i] = (int)(s_spec_peak[i] * (float)(SPEC_SEGS - 1) + 0.5f);
        if (peak_dot[i] >= SPEC_SEGS)  peak_dot[i]  = SPEC_SEGS - 1;
        peak_vis [i] = (s_spec_peak[i] > 0.02f);
    }

    /* Row-driven render — no PSRAM framebuffer.
     *
     * PSRAM was: 6 tubes × memset(25 600 B) + pixel writes + memcpy-to-SPI
     *   ≈ 150 KB PSRAM writes + 150 KB PSRAM reads per frame → ~25 ms/frame.
     *
     * New path: for each tube, precompute the 52 (4 bars × 13 segs) RGB565
     * colours once, then fill a 160-byte SRAM line buffer per row and transmit
     * immediately.  No PSRAM touched; line buffer is always DMA-safe. */
    uint8_t line[LCD_WIDTH * 2];   /* 160 B SRAM line buffer */

    for (int tube = 0; tube < LCD_COUNT; tube++) {

        /* Skip masked tubes — burn-in or snow will overwrite them immediately
         * after render_spectrum returns, so generating the spectrum frame and
         * pushing it over SPI would produce no visible output. */
        if ((s_burnin_mask | s_snow_mask) & (1u << tube)) continue;

        /* Precompute RGB565 colour for every (bar, segment) on this tube.
         * 52 spec_seg_color() calls here replace 640 calls inside the row loop. */
        uint16_t seg_color[SPEC_BARS_PER_TUBE][SPEC_SEGS];
        for (int bar = 0; bar < SPEC_BARS_PER_TUBE; bar++) {
            int bidx = tube * SPEC_BARS_PER_TUBE + bar;
            for (int s = 0; s < SPEC_SEGS; s++) {
                uint8_t r, g, b;
                if (peak_vis[bidx] && s == peak_dot[bidx]) {
                    r = g = b = 255;
                } else {
                    spec_seg_color(s, s < lit_count[bidx], br, bg, bb, &r, &g, &b);
                }
                seg_color[bar][s] = ((uint16_t)(r & 0xF8) << 8)
                                  | ((uint16_t)(g & 0xFC) << 3)
                                  |             (b >> 3);
            }
        }

        select_tube(tube);
        uint8_t ox = (uint8_t)((int)LCD_OFFSET_X + (int)s_burnin_shift_x + (int)s_col_offsets[tube]);
        uint8_t oy = (uint8_t)((int)LCD_OFFSET_Y                          + (int)s_row_offsets[tube]);
        open_lcd_window(ox, oy, LCD_WIDTH, LCD_HEIGHT);

        for (int y = 0; y < LCD_HEIGHT; y++) {
            /* Map row y → segment index (or -1 for a black gap/pad row).
             *
             * dist=0 is the bottom pixel of segment 0; increases going up.
             * Each 12-pixel cycle = SPEC_SEG_H(10) pixels + SPEC_GAP_H(2) gap.
             * Segments with seg_index ≥ SPEC_SEGS, or gap positions, → black. */
            int dist      = (LCD_HEIGHT - SPEC_PAD_BOT - 1) - y;   /* 156 − y */
            int seg_index = -1;
            if (dist >= 0) {
                int s   = dist / (SPEC_SEG_H + SPEC_GAP_H);
                int pos = dist % (SPEC_SEG_H + SPEC_GAP_H);
                if (s < SPEC_SEGS && pos < SPEC_SEG_H) seg_index = s;
            }

            /* Fill 80-pixel line:
             *   [1 pad][18 bar0][2 gap][18 bar1][2 gap][18 bar2][2 gap][18 bar3][1 trail]
             * Total: 1 + (18+2)×3 + 18 + 1 = 80 ✓ */
            uint8_t *px = line;
            *px++ = 0; *px++ = 0;                            /* left pad */
            for (int bar = 0; bar < SPEC_BARS_PER_TUBE; bar++) {
                uint8_t hi = 0, lo = 0;
                if (seg_index >= 0) {
                    uint16_t c = seg_color[bar][seg_index];
                    hi = (uint8_t)(c >> 8);
                    lo = (uint8_t)(c);
                }
                for (int x = 0; x < SPEC_BAR_W; x++) { *px++ = hi; *px++ = lo; }
                if (bar < SPEC_BARS_PER_TUBE - 1) {
                    *px++ = 0; *px++ = 0;                    /* inter-bar gap (first byte) */
                    *px++ = 0; *px++ = 0;                    /* inter-bar gap (second byte) */
                }
            }
            *px++ = 0; *px++ = 0;                            /* trailing pad */

            spi_transaction_t t = {
                .length    = LCD_WIDTH * 2 * 8,
                .tx_buffer = line,
            };
            spi_device_polling_transmit(spi_dev, &t);
        }
        deselect_all();
    }
}

/* Album: cycle through /images/album/ (jpg) files */
#include "dirent.h"
#define MAX_ALBUM      64
#define MAX_ALBUM_PATH 280   /* "/images/album/" (14) + d_name (255) + NUL */
static char s_album_files[MAX_ALBUM][MAX_ALBUM_PATH];
static int  s_album_count  = 0;
static int  s_album_index  = 0;
static bool s_album_loaded = false;

void display_album_invalidate(void)
{
    s_album_loaded = false;
    s_album_index  = 0;
}

static void album_load_list(bool shuffle)
{
    if (s_album_loaded) return;
    s_album_count = 0;
    DIR *dp = opendir("/spiffs/images/album");
    if (dp) {
        struct dirent *e;
        while ((e = readdir(dp)) && s_album_count < MAX_ALBUM) {
            char *ext = strrchr(e->d_name, '.');
            if (ext && strcasecmp(ext, ".jpg") == 0) {
                snprintf(s_album_files[s_album_count], MAX_ALBUM_PATH,
                         "/images/album/%s", e->d_name);
                s_album_count++;
            }
        }
        closedir(dp);
    }
    /* Fisher-Yates shuffle when requested */
    if (shuffle && s_album_count > 1) {
        for (int i = (int)s_album_count - 1; i > 0; i--) {
            int j = (int)(esp_random() % (uint32_t)(i + 1));
            char tmp[MAX_ALBUM_PATH];
            memcpy(tmp,                  s_album_files[j], MAX_ALBUM_PATH);
            memcpy(s_album_files[j],     s_album_files[i], MAX_ALBUM_PATH);
            memcpy(s_album_files[i],     tmp,              MAX_ALBUM_PATH);
        }
    }
    s_album_loaded = true;
}

/* Scan /spiffs/images/themes/, build a sorted rotation pool (filtered to the
 * user's selection when sel_count > 0), then advance cfg->theme to the next
 * entry.  Called at most once per theme_rotation_interval_s — the SPIFFS scan
 * cost is negligible at that frequency.  No mutex needed: called only from
 * the display task; config_set_theme() handles its own mutex for the write. */
#define MAX_THEMES_ROT     48
#define THEME_NAME_MAX_ROT 32

static void advance_theme(const char *current_theme,
                          uint8_t sel_count,
                          const char sel[][THEME_NAME_MAX_ROT])
{
    /* 1. Collect all installed theme directory names */
    char all[MAX_THEMES_ROT][THEME_NAME_MAX_ROT];
    int  total = 0;
    DIR *dp = opendir("/spiffs/images/themes");
    if (dp) {
        struct dirent *e;
        while ((e = readdir(dp)) && total < MAX_THEMES_ROT) {
            if (e->d_type == DT_DIR && e->d_name[0] != '.') {
                strncpy(all[total], e->d_name, THEME_NAME_MAX_ROT - 1);
                all[total][THEME_NAME_MAX_ROT - 1] = '\0';
                total++;
            }
        }
        closedir(dp);
    }
    /* 2. Insertion sort (matches api_themes() in web_server.c) */
    for (int i = 1; i < total; i++) {
        char tmp[THEME_NAME_MAX_ROT];
        strncpy(tmp, all[i], THEME_NAME_MAX_ROT);
        int j = i - 1;
        while (j >= 0 && strcmp(all[j], tmp) > 0) {
            strncpy(all[j + 1], all[j], THEME_NAME_MAX_ROT);
            j--;
        }
        strncpy(all[j + 1], tmp, THEME_NAME_MAX_ROT);
    }
    /* 3. Build rotation pool */
    char pool[MAX_THEMES_ROT][THEME_NAME_MAX_ROT];
    int  pool_count = 0;
    if (sel_count == 0) {
        /* All installed themes */
        for (int i = 0; i < total; i++) {
            strncpy(pool[pool_count], all[i], THEME_NAME_MAX_ROT - 1);
            pool[pool_count][THEME_NAME_MAX_ROT - 1] = '\0';
            pool_count++;
        }
    } else {
        /* Intersection: only selected themes that are actually installed */
        for (int i = 0; i < total; i++) {
            for (int s = 0; s < sel_count; s++) {
                if (strcmp(all[i], sel[s]) == 0) {
                    strncpy(pool[pool_count], all[i], THEME_NAME_MAX_ROT - 1);
                    pool[pool_count][THEME_NAME_MAX_ROT - 1] = '\0';
                    pool_count++;
                    break;
                }
            }
        }
    }
    if (pool_count <= 1) return;  /* nothing to cycle */

    /* 4. Find current theme; advance with wrap-around */
    int cur = -1;
    for (int i = 0; i < pool_count; i++) {
        if (strcmp(pool[i], current_theme) == 0) { cur = i; break; }
    }
    int next = (cur < 0) ? 0 : (cur + 1) % pool_count;
    config_set_theme(pool[next]);
    ESP_LOGI(TAG, "Theme rotation: \"%s\"", pool[next]);
}

static void render_album(const nextube_config_t *cfg,
                         TickType_t *last_switch, bool force)
{
    album_load_list(cfg->album_shuffle);
    if (s_album_count == 0) {
        for (int i = 0; i < LCD_COUNT; i++) {
            /* Skip masked tubes — burn-in/snow will cover them anyway. */
            if (!((s_burnin_mask | s_snow_mask) & (1u << i)))
                display_fill(i, 0x0000);
        }
        return;
    }
    uint32_t interval_ms = cfg->album_switch_ms ? cfg->album_switch_ms : 2000;
    TickType_t now = xTaskGetTickCount();
    if (force || (now - *last_switch) >= pdMS_TO_TICKS(interval_ms)) {
        *last_switch = now;
        /* Each tube shows a different image offset by its position, creating a
         * sliding-window effect.  With fewer images than tubes the list wraps
         * naturally so some images repeat — no special-casing needed. */
        for (int i = 0; i < LCD_COUNT; i++)
            display_show_image(i, s_album_files[(s_album_index + i) % s_album_count]);
        s_album_index = (s_album_index + 1) % s_album_count;
    }
}

/* ── Weather mode ───────────────────────────────────────────────────── */
/*
 * Layout: [TT][TT][unit][HH][HH][icon]
 *   Layout (see render_weather() for full table):
 *     positive 1-digit      : [blank][blank][units][C/F][icon]
 *     positive 2-digit      : [blank][tens][units][C/F][icon]
 *     negative 1-digit      : [blank][-][units][C/F][icon]
 *     negative 2-digit      : [-][tens][units][C/F][icon]
 *   Leading zeros are blank.  Negative temps suppress humidity entirely.
 *   All blank slots use AMPM/blank.jpg for a theme-consistent appearance.
 *   Unit: AMPM/blank.jpg OR-composited with Temperature/degreec.jpg or degreef.jpg
 *   tube  5   : weather icon from MutiInfo/Weather/{icon}.jpg
 *
 * Actual SPIFFS filenames (must match exactly):
 *   Temperature/ : degreec  degreef  minus
 *   AMPM/        : blank  (used as base layer; degreec/f OR-composited on top)
 *   Weather/     : sun  fewClouds  overcastClouds  fog
 *                  rain  snow  squalls  thunderstorm
 *                  sand  tornado  volcanicAsh
 */
/* ── Sun animation shared state ─────────────────────────────────────────────
 * File-scope so wx_sun_anim_frame (LCD path) and wx_sun_anim_frame_buf
 * (WL buffer path) advance the same counter without double-stepping.        */
static float s_sun_pos = 30.0f;  /* sun starts at horizon — visible from first frame */
static int   s_sun_ph  = 0;
static int   s_sun_cnt = 0;

/* Composite a u8g2 tile buffer into an RGB565 framebuffer in place.
 * Lit bits (1) write fg colour; unlit bits leave the background pixel
 * already in fb unchanged.  fg is raw RGB565 — display_show_digit applies
 * per-tube brightness/gamma when the complete buffer is sent to the LCD.
 * Buffer layout: tile[(row/8)*128 + col], bit = row%8 (u8g2 vertical 1bpp). */
static void wl_blit_tile_into_fb(uint8_t *fb, const uint8_t *tile,
                                  int rows, int y0, uint16_t fg)
{
    const int BUF_W = 128;
    uint8_t fh = (uint8_t)(fg >> 8), fl = (uint8_t)(fg & 0xFF);
    for (int row = 0; row < rows; row++) {
        int ay = y0 + row;
        if (ay < 0 || ay >= LCD_HEIGHT) continue;
        for (int col = 0; col < LCD_WIDTH; col++) {
            if ((tile[(row / 8) * BUF_W + col] >> (row % 8)) & 1) {
                int idx = (ay * LCD_WIDTH + col) * 2;
                fb[idx]     = fh;
                fb[idx + 1] = fl;
            }
        }
    }
}

/* ── wx_sun_anim_frame ───────────────────────────────────────────────────────
 * Animated sunrise/sunset for Weather Panel 2.
 *   tube 0 (rising=true)  — sun rises from below horizon, pauses at top, loops
 *   tube 4 (rising=false) — sun holds briefly at top then descends, loops
 *
 * The U8g2 frame buffer is 128×64 rows; the tube LCD is 80×160.  Three passes
 * cover the full tube height; each pass translates tube-absolute coordinates
 * into buffer coordinates by subtracting the pass start row (y0).  U8g2 clips
 * any disc/ray pixel whose buffer coord falls outside the window.
 *
 * Rendering layers per pass (matching the user's Arduino/U8g2 sketch):
 *   1. Full sun disc (U8G2_DRAW_ALL) — clipped when outside this pass window
 *   2. Seven rays at π … 2π (left → up → right)
 *   3. Black mask erasing everything below the horizon line
 *   4. Horizon line
 *   5. Two mountain triangles (pass 1 only — tube rows 64–127 contain them)
 *
 * fg: theme foreground colour (sun disc, rays, horizon, mountains).
 * bg: theme blank.jpg decoded RGB565 image (or NULL → solid black).  Passed
 *     through to ht_blit_at(); where U8g2 bit=0 the background image pixel is
 *     shown, giving the sky area the theme's texture.                         */
static void wx_sun_anim_frame(int tube, bool rising, uint16_t fg,
                               const uint8_t *bg)
{
    /* ── Shared animation state — both tubes driven by a single position ────
     *
     * s_pos is the distance (px) the suns have travelled from their start:
     *   Sunrise (tube 0): sy = 150 − s_pos   (bottom → top as s_pos grows)
     *   Sunset  (tube 4): sy =  40 + s_pos   (top → bottom as s_pos grows)
     *
     * At any frame: rise_y + set_y = 190 — the two suns are always at
     * complementary heights.  They cross the horizon (HY=110) simultaneously
     * at s_pos ≈ 40, so one sun emerges from the mountains exactly as the
     * other disappears behind them.
     *
     * State is advanced only on the rising=true call (tube 0) so that both
     * tubes read the same s_pos within the same render frame.               */
    if (rising) {                /* advance exactly once per render frame */
        if (s_sun_ph == 0) {
            s_sun_pos += 2.25f;
            if (s_sun_pos >= 110.0f) { s_sun_pos = 110.0f; s_sun_ph = 1; s_sun_cnt = 0; }
        } else {
            if (++s_sun_cnt >= 40) { s_sun_ph = 0; s_sun_pos = 30.0f; }
        }
    }

    const int HY = 110;      /* tube-absolute Y of horizon line */
    const int SR = 10;       /* sun disc radius                 */
    /* Derive this tube's sun position from the shared travel distance */
    float sy = rising ? (150.0f - s_sun_pos) : (40.0f + s_sun_pos);

    /* ── 3-pass render covering the full 160-px tube ── */
    static const int STARTS[3] = {0,  64, 128};
    static const int ROWS  [3] = {64, 64,  32};

    for (int p = 0; p < 3; p++) {
        int y0   = STARTS[p];
        int nrow = ROWS[p];

        int bSun = (int)sy - y0;   /* sun centre in buffer rows (may be negative) */
        int bHor = HY      - y0;   /* horizon    in buffer rows                   */

        u8g2_ClearBuffer(&s_u8g2);
        u8g2_SetDrawColor(&s_u8g2, 1);

        /* ── 1. Sun disc ──────────────────────────────────────────────────────
         * IMPORTANT: bSun may be negative when the sun centre is above this
         * pass's buffer window.  Casting a small negative int to u8g2_uint_t
         * (uint16_t with U8G2_16BIT) yields a value near 65535; adding the
         * disc radius then wraps back into [0, SR], so DrawDisc renders
         * phantom rows — the "downward lines" artifact.
         *
         * Safe rule:
         *   bSun >= 0        → DrawDisc works correctly (large positive coords
         *                      produced by disc top rows are ≥ 64 → clipped).
         *   -SR < bSun < 0   → Only the bottom cap [0 .. bSun+SR-1] is visible.
         *                      Draw each row with DrawHLine using signed math.
         *   bSun <= -SR      → Disc entirely above this pass; skip.            */
        if (bSun >= 0) {
            u8g2_DrawDisc(&s_u8g2, 40, (u8g2_uint_t)bSun, (u8g2_uint_t)SR,
                          U8G2_DRAW_ALL);
        } else if (bSun > -SR) {
            /* Sun centre is |bSun| rows above the buffer top.
             * Visible rows: 0 … (bSun + SR - 1).  Use signed dy to avoid cast. */
            int visible = bSun + SR;   /* > 0 because bSun > -SR */
            for (int row = 0; row < visible; row++) {
                int dy = row - bSun;   /* always positive: row >= 0, bSun < 0 */
                int hw = (int)sqrtf((float)(SR * SR - dy * dy));
                u8g2_DrawHLine(&s_u8g2,
                               (u8g2_uint_t)(40 - hw),
                               (u8g2_uint_t)row,
                               (u8g2_uint_t)(2 * hw + 1));
            }
        }
        /* bSun <= -SR: disc entirely above this pass — nothing to draw */

        /* ── 2. Seven rays: angles π … 2π (left → up → right) ────────────
         * All rays have sin(ang) ≤ 0 (upward in screen coords), so both
         * endpoints are at or above bSun.  When bSun < 0 every endpoint is
         * also negative → all out of range.  Only draw when bSun >= 0.
         *
         * CRITICAL: even when bSun >= 0, upward ray endpoints can be negative
         * when bSun < (SR+11) ≈ 21.  A negative int cast to u8g2_uint_t
         * (uint16_t) becomes ~65000+.  U8g2's Bresenham DrawLine then treats
         * this as a very large positive y, and if the other endpoint is in-range
         * (e.g. y=2), draws a line from y=2 all the way down to the buffer
         * edge — exactly the "downward lines from the sun" artifact.
         * Fix: skip any ray whose endpoints are outside [0, nrow).            */
        if (bSun >= 0) {
            /* i=1..5 skips the two horizontal rays (i=0: left, i=6: right).
             * Horizontal rays cast a wide dark shadow band at the sun's centre y,
             * producing a visible "shadow line" in the sky. */
            for (int i = 1; i < 6; i++) {
                float ang = (float)M_PI + (float)i * ((float)M_PI / 6.0f);
                float ca  = cosf(ang), sa = sinf(ang);
                int rx0 = 40   + (int)(ca * (float)(SR + 3));
                int ry0 = bSun + (int)(sa * (float)(SR + 3));
                int rx1 = 40   + (int)(ca * (float)(SR + 11));
                int ry1 = bSun + (int)(sa * (float)(SR + 11));
                /* Skip only if ray is entirely outside the pass window.
                 * Clamp negative endpoints to 0 (safe uint16_t cast);
                 * u8g2 clips the >= nrow end naturally within its buffer. */
                if ((ry0 < 0 && ry1 < 0) || (ry0 >= nrow && ry1 >= nrow)) continue;
                if (ry0 < 0) ry0 = 0;
                if (ry1 < 0) ry1 = 0;
                u8g2_DrawLine(&s_u8g2,
                              (u8g2_uint_t)rx0, (u8g2_uint_t)ry0,
                              (u8g2_uint_t)rx1, (u8g2_uint_t)ry1);
            }
        }

        /* ── 3. Mask — erase everything below the horizon ─────────────────
         * Draw-colour 0 sets bits to 0.  Where bg != NULL, ht_blit_at maps
         * zero-bits to the theme background, so the "sky" region above the
         * horizon shows the theme texture while below-horizon stays black
         * only because the mask is applied AFTER the disc/rays (overwriting
         * any disc pixels that extended past the horizon).                  */
        u8g2_SetDrawColor(&s_u8g2, 0);
        if (bHor < 0) {
            /* Entire pass is below the horizon — blank the whole buffer */
            u8g2_DrawBox(&s_u8g2, 0, 0, 80, (u8g2_uint_t)nrow);
        } else if (bHor < nrow) {
            /* Partial pass — erase rows horizon+1 … nrow-1 */
            int mask_h = nrow - bHor - 1;
            if (mask_h > 0)
                u8g2_DrawBox(&s_u8g2, 0, (u8g2_uint_t)(bHor + 1),
                             80, (u8g2_uint_t)mask_h);
        }
        /* (bHor >= nrow: entire pass is above horizon — no mask needed) */
        u8g2_SetDrawColor(&s_u8g2, 1);

        /* ── 4. Horizon line ── */
        if (bHor >= 0 && bHor < nrow)
            u8g2_DrawHLine(&s_u8g2, 0, (u8g2_uint_t)bHor, 80);

        /* ── 5. Mountain silhouettes — only in pass 1 (tube rows 64–127) ──
         * Mountain left:  vertices (5,110)(20,90)(35,110) → buf (5,46)(20,26)(35,46)
         * Mountain right: vertices (40,110)(60,95)(80,110) → buf (40,46)(60,31)(80,46)
         * Bases coincide with the horizon line (buffer row 46 in pass 1).     */
        if (p == 1) {
            u8g2_DrawTriangle(&s_u8g2,
                               5,  (int16_t)(110 - y0),
                              20,  (int16_t)( 90 - y0),
                              35,  (int16_t)(110 - y0));
            u8g2_DrawTriangle(&s_u8g2,
                              40,  (int16_t)(110 - y0),
                              60,  (int16_t)( 95 - y0),
                              80,  (int16_t)(110 - y0));
        }

        /* Blit: fg-colour where U8g2 bit=1, bg image where bit=0 (bg=NULL → black) */
        ht_blit_at(tube, u8g2_GetBufferPtr(&s_u8g2), nrow, y0, fg, bg);
    }
}

/* Buffer-path variant of wx_sun_anim_frame for the WeatherLive sky case.
 * Runs the same 3-pass draw loop but composites directly into fb (RGB565)
 * instead of issuing 3 partial LCD window writes.  Shares s_sun_pos/s_sun_ph/
 * s_sun_cnt with wx_sun_anim_frame so state is never double-advanced.
 * Caller must call display_show_digit() for the single SPI blit.           */
static void wx_sun_anim_frame_buf(uint8_t *fb, bool rising, uint16_t fg)
{
    if (rising) {
        if (s_sun_ph == 0) {
            s_sun_pos += 2.25f;
            if (s_sun_pos >= 110.0f) { s_sun_pos = 110.0f; s_sun_ph = 1; s_sun_cnt = 0; }
        } else {
            if (++s_sun_cnt >= 40) { s_sun_ph = 0; s_sun_pos = 30.0f; }
        }
    }

    const int HY = 110;
    const int SR = 10;
    float sy = rising ? (150.0f - s_sun_pos) : (40.0f + s_sun_pos);

    static const int STARTS[3] = {0,  64, 128};
    static const int ROWS  [3] = {64, 64,  32};

    for (int p = 0; p < 3; p++) {
        int y0   = STARTS[p];
        int nrow = ROWS[p];
        int bSun = (int)sy - y0;
        int bHor = HY      - y0;

        u8g2_ClearBuffer(&s_u8g2);
        u8g2_SetDrawColor(&s_u8g2, 1);

        if (bSun >= 0) {
            u8g2_DrawDisc(&s_u8g2, 40, (u8g2_uint_t)bSun, (u8g2_uint_t)SR,
                          U8G2_DRAW_ALL);
        } else if (bSun > -SR) {
            int visible = bSun + SR;
            for (int row = 0; row < visible; row++) {
                int dy = row - bSun;
                int hw = (int)sqrtf((float)(SR * SR - dy * dy));
                u8g2_DrawHLine(&s_u8g2, (u8g2_uint_t)(40 - hw), (u8g2_uint_t)row,
                               (u8g2_uint_t)(2 * hw + 1));
            }
        }

        if (bSun >= 0) {
            /* i=1..5: skip horizontal rays (i=0 left, i=6 right) — their shadow
             * bloom creates a wide dark band at the sun centre y that reads as a
             * "shadow line" in the sky whenever the disc passes that level. */
            for (int i = 1; i < 6; i++) {
                float ang = (float)M_PI + (float)i * ((float)M_PI / 6.0f);
                float ca  = cosf(ang), sa = sinf(ang);
                int rx0 = 40   + (int)(ca * (float)(SR + 3));
                int ry0 = bSun + (int)(sa * (float)(SR + 3));
                int rx1 = 40   + (int)(ca * (float)(SR + 11));
                int ry1 = bSun + (int)(sa * (float)(SR + 11));
                if ((ry0 < 0 && ry1 < 0) || (ry0 >= nrow && ry1 >= nrow)) continue;
                if (ry0 < 0) ry0 = 0;
                if (ry1 < 0) ry1 = 0;
                u8g2_DrawLine(&s_u8g2, (u8g2_uint_t)rx0, (u8g2_uint_t)ry0,
                              (u8g2_uint_t)rx1, (u8g2_uint_t)ry1);
            }
        }

        u8g2_SetDrawColor(&s_u8g2, 0);
        if (bHor < 0) {
            u8g2_DrawBox(&s_u8g2, 0, 0, 80, (u8g2_uint_t)nrow);
        } else if (bHor < nrow) {
            int mask_h = nrow - bHor - 1;
            if (mask_h > 0)
                u8g2_DrawBox(&s_u8g2, 0, (u8g2_uint_t)(bHor + 1),
                             80, (u8g2_uint_t)mask_h);
        }
        u8g2_SetDrawColor(&s_u8g2, 1);

        if (bHor >= 0 && bHor < nrow)
            u8g2_DrawHLine(&s_u8g2, 0, (u8g2_uint_t)bHor, 80);

        /* Shadow bloom #1 — sun disc + rays only (mountains not drawn yet).
         * Covers the sky zone (fy < HY-20) with full 360° bloom.
         * ny < y0 clip prevents shadow from bleeding backward into a previous
         * pass's already-blitted fb region (fixes the visible "shadow line"
         * that appeared each time the disc crossed the pass-0/pass-1 boundary
         * at tube y=64). */
        if (s_wl_shadow) {
            const uint8_t *tile = u8g2_GetBufferPtr(&s_u8g2);
            for (int ry = 0; ry < nrow; ry++) {
                int fy = y0 + ry;
                if (fy >= HY - 20) continue;  /* sky zone only; mountain zone handled below */
                for (int rx = 0; rx < LCD_WIDTH; rx++) {
                    if (!((tile[(ry / 8) * 128 + rx] >> (ry % 8)) & 1)) continue;
                    for (int dy = -2; dy <= 2; dy++) {
                        int ny = fy + dy;
                        if (ny < y0 || ny >= LCD_HEIGHT) continue;
                        for (int dx = -2; dx <= 2; dx++) {
                            int d2 = dx * dx + dy * dy;
                            if (d2 == 0 || d2 > 5) continue;
                            int nx = rx + dx;
                            if (nx < 0 || nx >= LCD_WIDTH) continue;
                            wl_blend_px(fb + (ny * LCD_WIDTH + nx) * 2,
                                        s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b,
                                        (d2 <= 2) ? 180 : 90);
                        }
                    }
                }
            }
        }

        /* Draw mountains after sun shadow so the mountain fill sits on top of
         * the sun's glow (correct z-order: sun glow behind mountains). */
        if (p == 1) {
            u8g2_DrawTriangle(&s_u8g2,
                               5,  (int16_t)(110 - y0),
                              20,  (int16_t)( 90 - y0),
                              35,  (int16_t)(110 - y0));
            u8g2_DrawTriangle(&s_u8g2,
                              40,  (int16_t)(110 - y0),
                              60,  (int16_t)( 95 - y0),
                              80,  (int16_t)(110 - y0));
        }

        /* Shadow bloom #2 — mountain zone (fy >= HY-20, fy < HY), pass 1 only.
         * dy starts at -1 (1px upward) to give the peaks a thin outline shadow
         * without pushing a visible band high into the open sky. */
        if (s_wl_shadow && p == 1) {
            int mry0 = HY - 20 - y0;
            if (mry0 < 0) mry0 = 0;
            const uint8_t *tile = u8g2_GetBufferPtr(&s_u8g2);
            for (int ry = mry0; ry < nrow; ry++) {
                int fy = y0 + ry;
                if (fy >= HY) continue;
                for (int rx = 0; rx < LCD_WIDTH; rx++) {
                    if (!((tile[(ry / 8) * 128 + rx] >> (ry % 8)) & 1)) continue;
                    for (int dy = -1; dy <= 2; dy++) {
                        int ny = fy + dy;
                        if (ny < y0 || ny >= LCD_HEIGHT) continue;
                        for (int dx = -2; dx <= 2; dx++) {
                            int d2 = dx * dx + dy * dy;
                            if (d2 == 0 || d2 > 5) continue;
                            int nx = rx + dx;
                            if (nx < 0 || nx >= LCD_WIDTH) continue;
                            wl_blend_px(fb + (ny * LCD_WIDTH + nx) * 2,
                                        s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b,
                                        (d2 <= 2) ? 180 : 90);
                        }
                    }
                }
            }
        }

        wl_blit_tile_into_fb(fb, u8g2_GetBufferPtr(&s_u8g2), nrow, y0, fg);
    }
}

/* ── wx_sun_draw_time ────────────────────────────────────────────────────────
 * Render "HH:MM" time centred both axes in a full 80×160 tube (logisoso28).
 * bg: decoded RGB565 background (LCD_WIDTH × LCD_HEIGHT); NULL = solid black. */
static void wx_sun_draw_time(int tube, const char *timestr, uint16_t fg,
                              const uint8_t *bg)
{
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFont(&s_u8g2, u8g2_font_logisoso24_tf);
    int ascent  = (int)u8g2_GetAscent(&s_u8g2);
    int descent = (int)u8g2_GetDescent(&s_u8g2);   /* negative */
    int glyph_h = ascent - descent;
    u8g2_uint_t tw = u8g2_GetStrWidth(&s_u8g2, timestr);
    int tx = ((int)LCD_WIDTH - (int)tw) / 2;
    if (tx < 0) tx = 0;
    u8g2_DrawStr(&s_u8g2, (u8g2_uint_t)tx, (u8g2_uint_t)ascent, timestr);

    int y_tube = (LCD_HEIGHT - glyph_h) / 2;
    ht_blit_at(tube, u8g2_GetBufferPtr(&s_u8g2), glyph_h, y_tube, fg, bg);
}

/* Buffer-path variant of wx_sun_draw_time for the WeatherLive sky case.
 * Composites the time string into fb so the caller can do a single
 * display_show_digit() instead of the double-write (full sky then text
 * overlay) that causes text to flicker at 20 Hz over an animated sky.     */
static void wx_sun_draw_time_buf(uint8_t *fb, const char *timestr, uint16_t fg)
{
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFont(&s_u8g2, u8g2_font_logisoso24_tf);
    int ascent  = (int)u8g2_GetAscent(&s_u8g2);
    int descent = (int)u8g2_GetDescent(&s_u8g2);
    int glyph_h = ascent - descent;
    u8g2_uint_t tw = u8g2_GetStrWidth(&s_u8g2, timestr);
    int tx = ((int)LCD_WIDTH - (int)tw) / 2;
    if (tx < 0) tx = 0;
    u8g2_DrawStr(&s_u8g2, (u8g2_uint_t)tx, (u8g2_uint_t)ascent, timestr);
    int y0_t = (LCD_HEIGHT - glyph_h) / 2;
    if (s_wl_shadow) {
        const uint8_t *tile = u8g2_GetBufferPtr(&s_u8g2);
        for (int ry = 0; ry < glyph_h; ry++) {
            int fy = y0_t + ry;
            for (int rx = 0; rx < LCD_WIDTH; rx++) {
                if (!((tile[(ry / 8) * 128 + rx] >> (ry % 8)) & 1)) continue;
                for (int dy = -2; dy <= 2; dy++) {
                    int ny = fy + dy;
                    if (ny < 0 || ny >= LCD_HEIGHT) continue;
                    for (int dx = -2; dx <= 2; dx++) {
                        int d2 = dx * dx + dy * dy;
                        if (d2 == 0 || d2 > 5) continue;
                        int nx = rx + dx;
                        if (nx < 0 || nx >= LCD_WIDTH) continue;
                        wl_blend_px(fb + (ny * LCD_WIDTH + nx) * 2,
                                    s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b,
                                    (d2 <= 2) ? 180 : 90);
                    }
                }
            }
        }
    }
    wl_blit_tile_into_fb(fb, u8g2_GetBufferPtr(&s_u8g2), glyph_h, y0_t, fg);
}

/* ── render_weather_sun ──────────────────────────────────────────────────────
 * Weather panel 2 — Sunrise & Sunset:
 *   tube 0 : animated sunrise (sun rises from mountains, loops)
 *   tube 1 : sunrise time "HH:MM"
 *   tube 2 : blank
 *   tube 3 : blank
 *   tube 4 : animated sunset  (sun descends into mountains, loops)
 *   tube 5 : sunset  time "HH:MM"
 *
 * anim_only=false (full redraw): renders all 6 tubes.  Called on first draw,
 *   mode/theme/time changes, or panel switch.
 * anim_only=true  (animation tick): only advances and blits tubes 0 and 4.
 *   Called every 50 ms tick while the sun panel is active, keeping the static
 *   tubes (1, 2, 3, 5) stable without unnecessary SPI writes.
 *
 * Solar times from NOAA algorithm; "--:--" fallback while geocoding pending.  */

/* WeatherLive background helpers for weather-mode panels.
 * Paint the live sky into wl_fb() for the given tube and blit to the LCD.
 * wl_tube_sky  → sky only (blank slot replacement).
 * wl_tube_str  → sky + centred text in the current WL font colour (digit/symbol replacement). */
static void wl_tube_sky(int tube)
{
    if (!s_wl_scene_valid) { display_fill(tube, 0x0000); return; }
    uint8_t *fb = wl_fb();
    if (!fb) { display_fill(tube, 0x0000); return; }
    wl_paint_background(fb, tube, &s_wl_last_scene);
    display_show_digit(tube, fb, LCD_WIDTH, LCD_HEIGHT);
}
static void wl_tube_str(int tube, const uint8_t *font, const char *str, int by)
{
    if (!s_wl_scene_valid) { display_fill(tube, 0x0000); return; }
    uint8_t *fb = wl_fb();
    if (!fb) { display_fill(tube, 0x0000); return; }
    wl_paint_background(fb, tube, &s_wl_last_scene);
    /* Single ASCII characters use the same 2× bilinear glyph path as the clock,
     * so digits/symbols in additional modes match the clock's visual scale.
     * Multi-byte or multi-char strings (e.g. "°C") use the 1× text path. */
    if (str[0] != '\0' && str[1] == '\0')
        wl_glyph(fb, str[0]);
    else
        wl_text(fb, 40, by, font, str, s_wl_font_r, s_wl_font_g, s_wl_font_b, 0);
    display_show_digit(tube, fb, LCD_WIDTH, LCD_HEIGHT);
}

/* When Custom clock face is active with a real theme background, assets such as
 * blank.jpg, digit images, and icons are loaded from custom_bg instead of theme. */
static const char *effective_bg_theme(const nextube_config_t *cfg)
{
    if (cfg->clock_face[0] != '\0' &&
        strcmp(cfg->clock_face, "custom") == 0 &&
        cfg->custom_bg[0] != '\0' &&
        strncmp(cfg->custom_bg, "WeatherLive", 11) != 0)
        return cfg->custom_bg;
    return cfg->theme;
}

static void render_weather_sun(const nextube_config_t *cfg, const struct tm *t,
                                bool anim_only)
{
    const char *th = effective_bg_theme(cfg);
    bool wl_sky = cx_is_wl_sky(cfg) && s_wl_scene_valid;
    uint8_t *wl_bgfb = wl_sky ? wl_fb() : NULL;

    uint16_t fg;
    const uint8_t *bg = NULL;
    char bg_path[256];
    if (wl_sky) {
        fg = wl_rgb565(s_wl_font_r, s_wl_font_g, s_wl_font_b);
    } else {
        fg = ht_sample_theme_color(th);
        snprintf(bg_path, sizeof(bg_path), "/images/themes/%s/AMPM/blank.jpg", th);
        int bg_w = 0, bg_h = 0;
        bg = img_cache_get(bg_path, &bg_w, &bg_h);
        if (bg_w != LCD_WIDTH || bg_h != LCD_HEIGHT) bg = NULL;
    }

    if (!anim_only) {
        char rise_str[8] = "--:--";
        char set_str[8]  = "--:--";

        float lat = 0.0f, lon = 0.0f;
        if (weather_get_location(&lat, &lon)) {
            int rise_min = 0, set_min = 0;
            solar_calc(lat, lon, t, &rise_min, &set_min);
            if (rise_min >= 0)
                snprintf(rise_str, sizeof(rise_str), "%02d:%02d",
                         (rise_min / 60) % 24, rise_min % 60);
            if (set_min >= 0)
                snprintf(set_str, sizeof(set_str), "%02d:%02d",
                         (set_min / 60) % 24, set_min % 60);
        }

        if (wl_sky && wl_bgfb) {
            /* Single LCD write per tube: composite text into buffer first,
             * then push sky+text in one shot — no 20 Hz text flicker.      */
            wl_paint_background(wl_bgfb, 1, &s_wl_last_scene);
            wx_sun_draw_time_buf(wl_bgfb, rise_str, fg);
            display_show_digit(1, wl_bgfb, LCD_WIDTH, LCD_HEIGHT);
            wl_tube_sky(2);
            wl_tube_sky(3);
            wl_paint_background(wl_bgfb, 5, &s_wl_last_scene);
            wx_sun_draw_time_buf(wl_bgfb, set_str, fg);
            display_show_digit(5, wl_bgfb, LCD_WIDTH, LCD_HEIGHT);
        } else {
            if (bg) {
                display_show_image(1, bg_path);
                display_show_image(5, bg_path);
            } else {
                display_fill(1, 0x0000);
                display_fill(5, 0x0000);
            }
            wx_sun_draw_time(1, rise_str, fg, bg);
            display_show_ampm(2, "blank", th);
            display_show_ampm(3, "blank", th);
            wx_sun_draw_time(5, set_str,  fg, bg);
        }
    }

    /* ── Animation tubes — rendered on every frame regardless of anim_only ── */
    if (wl_sky && wl_bgfb) {
        /* Pre-composite all 3 passes into the sky buffer, then one LCD write
         * per tube — eliminates 3 partial SPI windows and rolling-tear artefact. */
        wl_paint_background(wl_bgfb, 0, &s_wl_last_scene);
        wx_sun_anim_frame_buf(wl_bgfb, /*rising=*/true,  fg);
        display_show_digit(0, wl_bgfb, LCD_WIDTH, LCD_HEIGHT);
        wl_paint_background(wl_bgfb, 4, &s_wl_last_scene);
        wx_sun_anim_frame_buf(wl_bgfb, /*rising=*/false, fg);
        display_show_digit(4, wl_bgfb, LCD_WIDTH, LCD_HEIGHT);
    } else {
        wx_sun_anim_frame(0, /*rising=*/true,  fg, bg);
        wx_sun_anim_frame(4, /*rising=*/false, fg, bg);
    }
}

/* render_weather_wind – panel 3: wind speed.
 *
 * 2-digit layout (val < 100):
 *   [blank][wind-glyph][tens/blank][units][unit-label][blank]
 *      0        1           2         3        4          5
 *
 * 3-digit layout (val >= 100) — glyph shifts left to tube 0:
 *   [wind-glyph][hundreds][tens][units][unit-label][blank]
 *        0          1        2     3        4          5
 *
 * Glyph (tube 0 or 1) and unit-label (tube 4) are rendered procedurally into
 * the shared PSRAM framebuffer; digit tubes use standard theme images. */
static void render_weather_wind(const nextube_config_t *cfg)
{
    const char *th = effective_bg_theme(cfg);
    const weather_data_t *w = weather_get();
    int wind_kph = (w && w->valid) ? (int)lroundf(w->wind_kph) : 0;
    const char *unit = cfg->wind_unit[0] ? cfg->wind_unit : "km/h";

    int val; const char *label;
    if (strcmp(unit, "mph") == 0) {
        val = (int)lroundf((float)wind_kph * 0.621371f); label = "mph";
    } else if (strcmp(unit, "m/s") == 0) {
        val = (int)lroundf((float)wind_kph / 3.6f);       label = "m/s";
    } else {
        val = wind_kph;                                   label = "km/h";
    }
    if (val > 999) val = 999;

    bool three_digit = (val >= 100);
    int  glyph_tube  = three_digit ? 0 : 1;
    char path[128];
    bool wl_sky = cx_is_wl_sky(cfg) && s_wl_scene_valid;

    /* Tube 0: blank (2-digit only) */
    if (!three_digit) {
        if (wl_sky) wl_tube_sky(0);
        else display_show_ampm(0, "blank", th);
    }

    /* Wind-streak glyph — 3 horizontal lines with right-hand curls, centred
     * vertically in the 80×160 canvas; pushed to tube 0 on 3-digit values. */
    uint8_t *fb = wl_fb();
    if (fb) {
        if (wl_sky) {
            wl_paint_background(fb, glyph_tube, &s_wl_last_scene);
        } else {
            display_path_ampm(path, sizeof(path), th, "blank");
            seed_fb_blank(fb, path);
        }
        static const int wst[3][4] = { { 16, 50, 60, 7 },
                                        { 10, 58, 80, 8 },
                                        { 18, 46, 100, 7 } };
        /* Shadow pass: expanded geometry drawn first. */
        if (s_wl_shadow) {
            for (int s = 0; s < 3; s++) {
                int x0 = wst[s][0], x1 = wst[s][1], y = wst[s][2], cr = wst[s][3];
                for (int yy = y - 2; yy <= y + 2; yy++) {
                    if (yy < 0 || yy >= LCD_HEIGHT) continue;
                    for (int x = x0 - 1; x <= x1 + 1; x++) {
                        if (x < 0 || x >= LCD_WIDTH) continue;
                        wl_blend_px(fb + (yy * LCD_WIDTH + x) * 2,
                                    s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b, 150);
                    }
                }
                int ccx = x1, ccy = y - cr;
                for (float a = 1.57f; a <= 7.0f; a += 0.05f) {
                    for (int t = 0; t < 2; t++) {
                        int rr = cr + 1 + t;
                        int x  = ccx + (int)((float)rr * cosf(a) + 0.5f);
                        int yy = ccy + (int)((float)rr * sinf(a) + 0.5f);
                        if (x >= 0 && x < LCD_WIDTH && yy >= 0 && yy < LCD_HEIGHT)
                            wl_blend_px(fb + (yy * LCD_WIDTH + x) * 2,
                                        s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b, 150);
                    }
                }
            }
        }
        for (int s = 0; s < 3; s++) {
            int x0 = wst[s][0], x1 = wst[s][1], y = wst[s][2], cr = wst[s][3];
            for (int yy = y - 1; yy <= y + 1; yy++) {
                if (yy < 0 || yy >= LCD_HEIGHT) continue;
                for (int x = x0; x <= x1; x++) {
                    if (x < 0 || x >= LCD_WIDTH) continue;
                    wl_blend_px(fb + (yy * LCD_WIDTH + x) * 2, 255, 255, 255, 255);
                }
            }
            int ccx = x1, ccy = y - cr;
            for (float a = 1.57f; a <= 7.0f; a += 0.05f) {
                for (int tt = 0; tt < 2; tt++) {
                    int rr = cr - tt;
                    int x  = ccx + (int)((float)rr * cosf(a) + 0.5f);
                    int yy = ccy + (int)((float)rr * sinf(a) + 0.5f);
                    if (x >= 0 && x < LCD_WIDTH && yy >= 0 && yy < LCD_HEIGHT)
                        wl_blend_px(fb + (yy * LCD_WIDTH + x) * 2, 255, 255, 255, 255);
                }
            }
        }
        display_show_digit(glyph_tube, fb, LCD_WIDTH, LCD_HEIGHT);
    } else {
        if (wl_sky) wl_tube_sky(glyph_tube);
        else display_show_ampm(glyph_tube, "blank", th);
    }

    /* Digits — 3-digit: hundreds on tube 1, tens on tube 2.
     *           2-digit: tens (or blank) on tube 2, units on tube 3. */
    if (wl_sky) {
        char ds[8];
        if (three_digit) {
            snprintf(ds, sizeof(ds), "%d", val / 100);
            wl_tube_str(1, u8g2_font_logisoso46_tf, ds, 100);
            snprintf(ds, sizeof(ds), "%d", (val / 10) % 10);
            wl_tube_str(2, u8g2_font_logisoso46_tf, ds, 100);
        } else {
            if (val < 10) wl_tube_sky(2);
            else { snprintf(ds, sizeof(ds), "%d", val / 10); wl_tube_str(2, u8g2_font_logisoso46_tf, ds, 100); }
        }
        snprintf(ds, sizeof(ds), "%d", val % 10);
        wl_tube_str(3, u8g2_font_logisoso46_tf, ds, 100);
    } else {
        if (three_digit) {
            display_path_number(path, sizeof(path), th, val / 100);
            display_show_image(1, path);
            display_path_number(path, sizeof(path), th, (val / 10) % 10);
            display_show_image(2, path);
        } else {
            if (val < 10) {
                display_show_ampm(2, "blank", th);
            } else {
                display_path_number(path, sizeof(path), th, val / 10);
                display_show_image(2, path);
            }
        }
        display_path_number(path, sizeof(path), th, val % 10);
        display_show_image(3, path);
    }

    /* Tube 4: unit label (km/h · mph · m/s) */
    if (fb) {
        if (wl_sky) {
            wl_paint_background(fb, 4, &s_wl_last_scene);
        } else {
            display_path_ampm(path, sizeof(path), th, "blank");
            seed_fb_blank(fb, path);
        }
        wl_text(fb, 40, 88, u8g2_font_logisoso20_tf, label, 210, 220, 235, 0);
        display_show_digit(4, fb, LCD_WIDTH, LCD_HEIGHT);
    } else {
        if (wl_sky) wl_tube_sky(4);
        else display_show_ampm(4, "blank", th);
    }

    /* Tube 5: blank */
    if (wl_sky) wl_tube_sky(5);
    else display_show_ampm(5, "blank", th);
}

/* render_weather_hilo – panel 4: daily Hi (show_hi=true) or Lo (false).
 *
 * Layout mirrors the temperature panel — tube 0 is replaced by a procedural
 * coloured arrow instead of a blank, tubes 1-4 carry sign/digits/degree:
 *
 *   positive 1-digit:  [arrow] [blank] [blank]  [units] [°C/F] [blank]
 *   positive 2-digit:  [arrow] [blank] [tens]   [units] [°C/F] [blank]
 *   negative 1-digit:  [arrow] [blank] [minus]  [units] [°C/F] [blank]
 *   negative 2-digit:  [arrow] [minus] [tens]   [units] [°C/F] [blank]
 *
 * Red ↑ for HI, blue ↓ for LO.  Caps at ±99° (no real temperature exceeds this). */
typedef struct { bool negative; int value; } temp_val_t;
static temp_val_t temp_sign_magnitude(float celsius, bool use_fahrenheit)
{
    float f = to_display_temp(celsius, use_fahrenheit);
    bool neg = (f < -0.5f);
    int  v   = (int)(neg ? -f + 0.5f : f + 0.5f);
    if (v > 99) v = 99;
    return (temp_val_t){ .negative = neg, .value = v };
}

static void render_weather_hilo(const nextube_config_t *cfg, bool show_hi)
{
    const char *th = effective_bg_theme(cfg);
    const weather_data_t *w = weather_get();
    char path[128];

    bool wl_sky = cx_is_wl_sky(cfg) && s_wl_scene_valid;

    if (!w || !w->valid || !w->day_range_valid) {
        for (int i = 0; i < LCD_COUNT; i++) {
            if (wl_sky) wl_tube_sky(i);
            else display_show_ampm(i, "blank", th);
        }
        return;
    }

    bool fahrenheit = (strncmp(cfg->temp_format, "Fahrenheit", 10) == 0);
    float raw_c = show_hi ? w->day_max_c : w->day_min_c;
    temp_val_t tv = temp_sign_magnitude(raw_c, fahrenheit);
    bool negative     = tv.negative;
    int  val          = tv.value;
    bool single_digit = (val < 10);
    const char *unit = fahrenheit ? "degreef" : "degreec";

    /* Tube 0: procedural arrow — red ↑ (HI) or blue ↓ (LO), centred in 80×160. */
    uint8_t *fb = wl_fb();
    if (fb) {
        if (wl_sky) {
            wl_paint_background(fb, 0, &s_wl_last_scene);
        } else {
            display_path_ampm(path, sizeof(path), th, "blank");
            seed_fb_blank(fb, path);
        }
        uint8_t ar = show_hi ? 255 : 80,
                ag = show_hi ?  70 : 140,
                ab = show_hi ?  70 : 255;
        /* Arrow geometry: total height 80 px centred in 160 px (y=40..120).
         * Head = 40 px tall, max half-width 20 px.  Shaft = 40 px, half-width 7 px. */
        /* Shadow pass: same arrow geometry expanded 2 px all round. */
        if (s_wl_shadow) {
            if (show_hi) {
                /* UP: shadow head rows 38..82, then shadow shaft rows 78..122. */
                for (int row = 38; row <= 82; row++) {
                    if (row < 0 || row >= LCD_HEIGHT) continue;
                    float frac = (float)(row - 40) / 40.0f;
                    if (frac < 0.0f) frac = 0.0f;
                    if (frac > 1.0f) frac = 1.0f;
                    int hw = (int)(frac * 20.0f + 2.5f);
                    for (int x = 40 - hw; x <= 40 + hw; x++)
                        if (x >= 0 && x < LCD_WIDTH)
                            wl_blend_px(fb + (row * LCD_WIDTH + x) * 2,
                                        s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b, 150);
                }
                for (int row = 78; row <= 122; row++) {
                    if (row < 0 || row >= LCD_HEIGHT) continue;
                    for (int x = 31; x <= 49; x++)
                        if (x >= 0 && x < LCD_WIDTH)
                            wl_blend_px(fb + (row * LCD_WIDTH + x) * 2,
                                        s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b, 150);
                }
            } else {
                /* DOWN: shadow shaft rows 38..82, then shadow head rows 78..122. */
                for (int row = 38; row <= 82; row++) {
                    if (row < 0 || row >= LCD_HEIGHT) continue;
                    for (int x = 31; x <= 49; x++)
                        if (x >= 0 && x < LCD_WIDTH)
                            wl_blend_px(fb + (row * LCD_WIDTH + x) * 2,
                                        s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b, 150);
                }
                for (int row = 78; row <= 122; row++) {
                    if (row < 0 || row >= LCD_HEIGHT) continue;
                    float frac = (float)(120 - row) / 40.0f;
                    if (frac < 0.0f) frac = 0.0f;
                    if (frac > 1.0f) frac = 1.0f;
                    int hw = (int)(frac * 20.0f + 2.5f);
                    for (int x = 40 - hw; x <= 40 + hw; x++)
                        if (x >= 0 && x < LCD_WIDTH)
                            wl_blend_px(fb + (row * LCD_WIDTH + x) * 2,
                                        s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b, 150);
                }
            }
        }
        if (show_hi) {
            /* UP: head at top (y 40-80), shaft below (y 80-120). */
            for (int row = 40; row <= 80; row++) {
                float frac = (float)(row - 40) / 40.0f;
                int hw = (int)(frac * 20.0f + 0.5f);
                for (int x = 40 - hw; x <= 40 + hw; x++)
                    if (x >= 0 && x < LCD_WIDTH)
                        wl_blend_px(fb + (row * LCD_WIDTH + x) * 2, ar, ag, ab, 255);
            }
            for (int row = 80; row <= 120; row++)
                for (int x = 33; x <= 47; x++)
                    wl_blend_px(fb + (row * LCD_WIDTH + x) * 2, ar, ag, ab, 255);
        } else {
            /* DOWN: shaft at top (y 40-80), head below (y 80-120). */
            for (int row = 40; row <= 80; row++)
                for (int x = 33; x <= 47; x++)
                    wl_blend_px(fb + (row * LCD_WIDTH + x) * 2, ar, ag, ab, 255);
            for (int row = 80; row <= 120; row++) {
                float frac = (float)(120 - row) / 40.0f;
                int hw = (int)(frac * 20.0f + 0.5f);
                for (int x = 40 - hw; x <= 40 + hw; x++)
                    if (x >= 0 && x < LCD_WIDTH)
                        wl_blend_px(fb + (row * LCD_WIDTH + x) * 2, ar, ag, ab, 255);
            }
        }
        display_show_digit(0, fb, LCD_WIDTH, LCD_HEIGHT);
    } else {
        if (wl_sky) wl_tube_sky(0);
        else display_show_ampm(0, "blank", th);
    }

    char ds[16];

    /* Tube 1: minus (2-digit negative) or blank */
    if (negative && !single_digit) {
        if (wl_sky) wl_tube_str(1, u8g2_font_logisoso46_tf, "-", 100);
        else { display_path_temperature(path, sizeof(path), th, "minus"); display_show_image(1, path); }
    } else {
        if (wl_sky) wl_tube_sky(1);
        else display_show_ampm(1, "blank", th);
    }

    /* Tube 2: minus (1-digit negative), tens digit (2-digit positive), or blank */
    if (negative && single_digit) {
        if (wl_sky) wl_tube_str(2, u8g2_font_logisoso46_tf, "-", 100);
        else { display_path_temperature(path, sizeof(path), th, "minus"); display_show_image(2, path); }
    } else if (!single_digit) {
        if (wl_sky) { snprintf(ds, sizeof(ds), "%d", val / 10); wl_tube_str(2, u8g2_font_logisoso46_tf, ds, 100); }
        else { display_path_number(path, sizeof(path), th, val / 10); display_show_image(2, path); }
    } else {
        if (wl_sky) wl_tube_sky(2);
        else display_show_ampm(2, "blank", th);
    }

    /* Tube 3: units digit */
    if (wl_sky) { snprintf(ds, sizeof(ds), "%d", val % 10); wl_tube_str(3, u8g2_font_logisoso46_tf, ds, 100); }
    else { display_path_number(path, sizeof(path), th, val % 10); display_show_image(3, path); }

    /* Tube 4: °C / °F symbol */
    if (wl_sky) {
        wl_tube_str(4, u8g2_font_logisoso28_tf, fahrenheit ? "\xc2\xb0""F" : "\xc2\xb0""C", 91);
    } else {
        display_path_temperature(path, sizeof(path), th, unit);
        display_show_image(4, path);
    }

    /* Tube 5: blank */
    if (wl_sky) wl_tube_sky(5);
    else display_show_ampm(5, "blank", th);
}

/* render_weather – panel 0 = temperature + icon, panel 1 = humidity + icon,
 *                  panel 2 = sunrise + sunset times.
 *
 * Three-panel layout (auto-cycles in the display task):
 *
 *  Panel 0 — temperature (tubes 0-indexed):
 *    positive 1-digit:  0=blank  1=blank  2=blank  3=units  4=°C/°F
 *    positive 2-digit:  0=blank  1=blank  2=tens   3=units  4=°C/°F
 *    negative 1-digit:  0=blank  1=blank  2=minus  3=units  4=°C/°F
 *    negative 2-digit:  0=blank  1=minus  2=tens   3=units  4=°C/°F
 *                     tube 5 = weather icon
 *
 *  Panel 1 — humidity:
 *    [droplet glyph] [blank] [hum_tens/blank] [hum_units] [%] [icon]
 *
 *  Panel 2 — sunrise + sunset:
 *    [rise_icon] [rise_time] [blank] [blank] [set_icon] [set_time]
 */
static void render_weather(const nextube_config_t *cfg, int panel, bool anim_only,
                           int hilo_phase)
{
    if (cx_is_wl_sky(cfg)) wl_ensure_scene(cfg);   /* keep anim_t current */
    const char *th = effective_bg_theme(cfg);
    const weather_data_t *w = weather_get();
    bool wl_sky = cx_is_wl_sky(cfg) && s_wl_scene_valid;
    char path[128];
    char ds[16];

    if (!w || !w->valid) {
        for (int i = 0; i < LCD_COUNT; i++) {
            if (wl_sky) wl_tube_str(i, u8g2_font_logisoso46_tf, ".", 100);
            else display_show_ampm(i, "dot", th);
        }
        return;
    }

    /* Panel 4 — Hi/Lo — dedicated renderer; hilo_phase 0 = HI, 1 = LO */
    if (panel == WEATHER_PANEL_HILO) {
        render_weather_hilo(cfg, hilo_phase == 0);
        return;
    }

    /* Panel 3 — wind speed — handled by a dedicated renderer */
    if (panel == WEATHER_PANEL_WIND) {
        render_weather_wind(cfg);
        return;
    }

    /* Panel 2 — sunrise/sunset — handled by a dedicated renderer */
    if (panel == WEATHER_PANEL_SUN) {
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        render_weather_sun(cfg, &lt, anim_only);
        return;
    }

    /* Temperature in the configured unit */
    bool fahrenheit = (strncmp(cfg->temp_format, "Fahrenheit", 10) == 0);
    temp_val_t tv = temp_sign_magnitude(w->temp_c, fahrenheit);
    bool negative = tv.negative;
    int  temp     = tv.value;

    int hum = (int)(w->humidity + 0.5f);
    if (hum < 0)  hum = 0;
    if (hum > 99) hum = 99;

    const char *unit = fahrenheit ? "degreef" : "degreec";
    const char *icon = (w->icon[0] != '\0') ? w->icon : "sun";

    /* Tube 5 (weather icon) — sky fill for WeatherLive (no icon assets) */
    if (wl_sky) {
        wl_tube_sky(5);
    } else {
        display_path_weather(path, sizeof(path), th, icon);
        display_show_image(5, path);
    }

    /* ── Panel 1: humidity ─────────────────────────────────────────── */
    /* Layout: 0=droplet glyph  1=blank  2=tens/blank  3=units  4=%  5=icon */
    if (panel == 1) {
        /* Tube 0: procedural water-droplet glyph */
        {
            uint8_t *fb = wl_fb();
            if (fb) {
                if (wl_sky) {
                    wl_paint_background(fb, 0, &s_wl_last_scene);
                } else {
                    display_path_ampm(path, sizeof(path), th, "blank");
                    seed_fb_blank(fb, path);
                }

                const int tip = 36;
                const int cx  = 40;
                const int bcy = 100;
                const int rad = 22;

                /* Shadow pass: same droplet shape expanded 2 px all round. */
                if (s_wl_shadow) {
                    const int SH = 2;
                    for (int sy = tip - SH; sy <= bcy + rad + SH; sy++) {
                        if (sy < 0 || sy >= LCD_HEIGHT) continue;
                        float w;
                        if (sy < tip) {
                            float dist = (float)(tip - sy);
                            w = (dist <= (float)SH) ? ((float)SH - dist) : 0.0f;
                        } else if (sy <= bcy) {
                            float t = (float)(sy - tip) / (float)(bcy - tip);
                            w = (float)rad * t * (2.0f - t) + (float)SH;
                        } else {
                            float dsy = (float)(sy - bcy);
                            float v   = (float)(rad * rad) - dsy * dsy;
                            w = (v > 0.0f) ? sqrtf(v) + (float)SH : 0.0f;
                        }
                        int iw = (int)(w + 0.5f);
                        for (int sx = cx - iw; sx <= cx + iw; sx++) {
                            if (sx < 0 || sx >= LCD_WIDTH) continue;
                            wl_blend_px(fb + (sy * LCD_WIDTH + sx) * 2,
                                        s_wl_shadow_r, s_wl_shadow_g, s_wl_shadow_b, 160);
                        }
                    }
                }
                for (int y = tip; y <= bcy + rad; y++) {
                    if (y < 0 || y >= LCD_HEIGHT) continue;
                    int cone_hw = 0;
                    if (y <= bcy) {
                        float t = (float)(y - tip) / (float)(bcy - tip);
                        cone_hw = (int)((float)rad * t * (2.0f - t) + 0.5f);
                    }
                    for (int x = cx - rad; x <= cx + rad; x++) {
                        if (x < 0 || x >= LCD_WIDTH) continue;
                        int dx = x - cx, dy = y - bcy;
                        bool in_circle = (dx * dx + dy * dy <= rad * rad);
                        bool in_cone   = (y <= bcy && (dx < 0 ? -dx : dx) <= cone_hw);
                        if (in_circle || in_cone)
                            wl_blend_px(fb + (y * LCD_WIDTH + x) * 2, 80, 160, 255, 255);
                    }
                }
                for (int y = bcy - rad + 4; y <= bcy - rad + 12; y++) {
                    if (y < 0 || y >= LCD_HEIGHT) continue;
                    for (int x = cx - 12; x <= cx - 2; x++) {
                        if (x < 0 || x >= LCD_WIDTH) continue;
                        int dx = x - (cx - 7), dy = y - (bcy - rad + 8);
                        if (dx * dx + dy * dy <= 16)
                            wl_blend_px(fb + (y * LCD_WIDTH + x) * 2, 220, 240, 255, 180);
                    }
                }
                display_show_digit(0, fb, LCD_WIDTH, LCD_HEIGHT);
            } else {
                if (wl_sky) wl_tube_sky(0);
                else display_show_ampm(0, "blank", th);
            }
        }

        if (wl_sky) wl_tube_sky(1);
        else display_show_ampm(1, "blank", th);

        /* Tube 2: tens digit of humidity (blank if < 10) */
        if (wl_sky) {
            if (hum / 10 == 0) wl_tube_sky(2);
            else { snprintf(ds, sizeof(ds), "%d", hum / 10); wl_tube_str(2, u8g2_font_logisoso46_tf, ds, 100); }
        } else {
            if (hum / 10 == 0) display_show_ampm(2, "blank", th);
            else { display_path_number(path, sizeof(path), th, hum / 10); display_show_image(2, path); }
        }

        /* Tube 3: units digit of humidity */
        if (wl_sky) { snprintf(ds, sizeof(ds), "%d", hum % 10); wl_tube_str(3, u8g2_font_logisoso46_tf, ds, 100); }
        else { display_path_number(path, sizeof(path), th, hum % 10); display_show_image(3, path); }

        /* Tube 4: humidity % symbol.
         * Single char → wl_tube_str would route through wl_glyph (2× full-tube
         * scale, logisoso42 hardcoded), making % fill the tube like a digit.
         * Use wl_text directly for the correct 1× accent-sized rendering.       */
        if (wl_sky) {
            uint8_t *pfb = wl_fb();
            if (pfb) {
                wl_paint_background(pfb, 4, &s_wl_last_scene);
                wl_text(pfb, 40, 91, u8g2_font_logisoso28_tf, "%",
                        s_wl_font_r, s_wl_font_g, s_wl_font_b, 0);
                display_show_digit(4, pfb, LCD_WIDTH, LCD_HEIGHT);
            } else {
                wl_tube_sky(4);
            }
        } else { display_path_humidity(path, sizeof(path), th, "humidity"); display_show_image(4, path); }

        return;
    }

    /* ── Panel 0: temperature ──────────────────────────────────────── */
    if (!wl_sky) flip_prime_blank(4, th);

    bool single_digit = (temp < 10);

    /* Tube 0: always blank */
    if (wl_sky) wl_tube_sky(0);
    else display_show_ampm(0, "blank", th);

    /* Tube 1: minus (2-digit negative) or blank */
    if (negative && !single_digit) {
        if (wl_sky) wl_tube_str(1, u8g2_font_logisoso46_tf, "-", 100);
        else { display_path_temperature(path, sizeof(path), th, "minus"); display_show_image(1, path); }
    } else {
        if (wl_sky) wl_tube_sky(1);
        else display_show_ampm(1, "blank", th);
    }

    /* Tube 2: minus (1-digit negative), tens digit (2-digit), or blank */
    if (negative && single_digit) {
        if (wl_sky) wl_tube_str(2, u8g2_font_logisoso46_tf, "-", 100);
        else { display_path_temperature(path, sizeof(path), th, "minus"); display_show_image(2, path); }
    } else if (!single_digit) {
        if (wl_sky) { snprintf(ds, sizeof(ds), "%d", temp / 10); wl_tube_str(2, u8g2_font_logisoso46_tf, ds, 100); }
        else { display_path_number(path, sizeof(path), th, temp / 10); display_show_image(2, path); }
    } else {
        if (wl_sky) wl_tube_sky(2);
        else display_show_ampm(2, "blank", th);
    }

    /* Tube 3: units digit */
    if (wl_sky) { snprintf(ds, sizeof(ds), "%d", temp % 10); wl_tube_str(3, u8g2_font_logisoso46_tf, ds, 100); }
    else { display_path_number(path, sizeof(path), th, temp % 10); display_show_image(3, path); }

    /* Tube 4: °C / °F symbol */
    if (wl_sky) wl_tube_str(4, u8g2_font_logisoso28_tf, fahrenheit ? "\xc2\xb0""F" : "\xc2\xb0""C", 91);
    else { display_path_temperature(path, sizeof(path), th, unit); display_show_image(4, path); }
}

/* ── Timer state ────────────────────────────────────────────────────── */
static TickType_t s_timer_start       = 0;
static bool       s_pomo_in_break     = false;
static bool       s_timer_paused      = false;
static uint32_t   s_paused_elapsed_ms = 0;   /* elapsed frozen at pause moment */

void display_timer_reset(void)
{
    if (s_timer_mutex) xSemaphoreTake(s_timer_mutex, portMAX_DELAY);
    s_timer_start        = xTaskGetTickCount();
    s_pomo_in_break      = false;
    s_timer_paused       = false;
    s_paused_elapsed_ms  = 0;
    if (s_timer_mutex) xSemaphoreGive(s_timer_mutex);
}

/* Toggle countdown / pomodoro timer between running and paused.
 * When pausing  : freeze elapsed_ms so the display stops counting.
 * When resuming : shift s_timer_start forward so elapsed resumes
 *                 from the frozen point without any jump. */
void display_timer_toggle(void)
{
    if (!s_timer_mutex) return;
    xSemaphoreTake(s_timer_mutex, portMAX_DELAY);
    if (s_timer_paused) {
        /* Resume: reconstruct start tick so elapsed_ms picks up from freeze */
        s_timer_start  = xTaskGetTickCount() - pdMS_TO_TICKS(s_paused_elapsed_ms);
        s_timer_paused = false;
    } else {
        /* Pause: freeze current elapsed */
        s_paused_elapsed_ms = (uint32_t)pdTICKS_TO_MS(
                                  xTaskGetTickCount() - s_timer_start);
        s_timer_paused      = true;
    }
    xSemaphoreGive(s_timer_mutex);
}

/* ── render_ticker ───────────────────────────────────────────────────
 * Called once per 200 ms tick while an MQTT ticker is active.
 *
 * The text scrolls right-to-left across all 6 tubes using logisoso28.
 * For tube i, the text is drawn at x = s_ticker_state.x_start − i×80
 * in the U8g2 local coordinate space.  With U8G2_16BIT, negative values
 * cause U8g2 to clip correctly: glyphs whose right edge ≤ 0 are skipped,
 * and glyphs that straddle x=0 are rendered with their left columns
 * clipped — exactly the "partial entry from left" behaviour required.
 * ht_blit_at reads only columns 0..79 from the 128-wide U8g2 buffer, so
 * text drawn beyond column 79 is automatically invisible on that tube.
 *
 * Vertical layout: logisoso28 ascent ≈28 descent ≈−6 → text band ~34 px.
 * The buffer (64 rows) is centred in the 160-row tube at y_tube = 48. */
/* 2× pixel-scaled blit of the ticker text band for one tube.
 * Reads LCD_WIDTH/TICKER_SCALE = 40 source columns and 64 source rows from the
 * U8g2 buffer and scales each pixel to a 2×2 block → 80×128 output, written at
 * a TICKER_Y_MARGIN-row top offset.  Non-lit pixels are solid black; the 16-row
 * top/bottom margins are left untouched (blanked once at ticker start), so the
 * band stays centred in the 160-row tube without repainting the margins each
 * tick.  Mirrors the scaling loop in pin_draw_tube(). */
static void ht_blit_ticker_2x(int tube, const uint8_t *tile_buf, uint16_t fg)
{
    /* Per-tube brightness + gamma on fg (bg is always black → stays black). */
    {
        uint8_t  br     = s_tube_brightness[tube];
        bool     do_br  = (br < 100);
        bool     do_gam = s_gamma_lut_active[tube];
        if (do_br || do_gam) {
            uint32_t r = (fg >> 11) & 0x1Fu;
            uint32_t g = (fg >>  5) & 0x3Fu;
            uint32_t b =  fg        & 0x1Fu;
            if (do_br)  { r = r * br / 100u; g = g * br / 100u; b = b * br / 100u; }
            if (do_gam) { r = s_gamma_lut_5bit[tube][r];
                          g = s_gamma_lut_6bit[tube][g];
                          b = s_gamma_lut_5bit[tube][b]; }
            fg = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
    uint8_t fg_hi = (uint8_t)(fg >> 8);
    uint8_t fg_lo = (uint8_t)(fg & 0xFF);
    const int BUF_W = 128;

    select_tube(tube);
    uint8_t ox = (uint8_t)((int)LCD_OFFSET_X + (int)s_burnin_shift_x
                            + (int)s_col_offsets[tube]);
    uint8_t oy = (uint8_t)((int)LCD_OFFSET_Y + (int)s_row_offsets[tube]
                            + TICKER_Y_MARGIN);
    open_lcd_window(ox, oy, (uint8_t)LCD_WIDTH, (uint8_t)TICKER_OUT_H);

    uint8_t chunk[LCD_WIDTH * 2 * DISP_CHUNK_ROWS];   /* 1280 B — SRAM stack */
    for (int out_row = 0; out_row < TICKER_OUT_H; out_row += DISP_CHUNK_ROWS) {
        int rows = (out_row + DISP_CHUNK_ROWS <= TICKER_OUT_H) ? DISP_CHUNK_ROWS
                                                               : TICKER_OUT_H - out_row;
        for (int r = 0; r < rows; r++) {
            int src_row  = (out_row + r) / TICKER_SCALE;
            int tile_row = src_row / 8;
            int bit      = src_row % 8;
            for (int src_col = 0; src_col < LCD_WIDTH / TICKER_SCALE; src_col++) {
                bool    lit = (tile_buf[tile_row * BUF_W + src_col] >> bit) & 1;
                uint8_t hi  = lit ? fg_hi : 0x00;
                uint8_t lo  = lit ? fg_lo : 0x00;
                int     oc  = src_col * TICKER_SCALE;
                chunk[(r * LCD_WIDTH + oc    ) * 2]     = hi;
                chunk[(r * LCD_WIDTH + oc    ) * 2 + 1] = lo;
                chunk[(r * LCD_WIDTH + oc + 1) * 2]     = hi;
                chunk[(r * LCD_WIDTH + oc + 1) * 2 + 1] = lo;
            }
        }
        spi_transaction_t t = { .length = (size_t)(rows * LCD_WIDTH * 2) * 8,
                                 .tx_buffer = chunk };
        spi_device_polling_transmit(spi_dev, &t);
    }
    deselect_all();
}

static void render_ticker(const nextube_config_t *cfg)
{
    uint16_t fg = ht_sample_theme_color(cfg->theme);

    u8g2_SetFont(&s_u8g2, u8g2_font_logisoso28_tf);
    int ascent  = (int)u8g2_GetAscent(&s_u8g2);
    int descent = (int)u8g2_GetDescent(&s_u8g2);
    /* Baseline that centres the glyph band in the 64-row buffer; the 2× blit
     * then doubles it into the 128-row output band. */
    int y_base = (64 + ascent + descent) / 2;
    if (y_base < ascent) y_base = ascent;
    if (y_base > 63)     y_base = 63;

    for (int tube = 0; tube < LCD_COUNT; tube++) {
        u8g2_ClearBuffer(&s_u8g2);
        /* The U8g2 buffer is half the on-screen resolution (each buffer pixel
         * becomes a 2×2 on-screen block), so the per-tube draw offset is the
         * on-screen offset divided by TICKER_SCALE.  (x_start - tube*80) is
         * always a multiple of 4, so the /2 is exact even when negative.
         * Negative x_draw lets U8g2 clip glyphs scrolling off the left edge. */
        int x_draw = (s_ticker_state.x_start - tube * LCD_WIDTH) / TICKER_SCALE;
        u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)x_draw,
                      (u8g2_uint_t)y_base,
                      s_ticker_state.text);
        ht_blit_ticker_2x(tube, u8g2_GetBufferPtr(&s_u8g2), fg);
    }

    /* Advance scroll position (on-screen pixels; speed set via HA MQTT) */
    s_ticker_state.x_start -= s_ticker_scroll_px;

    /* Finished when the text's right edge has scrolled past tube 0's left edge.
     * text_px_w is the on-screen width (2× the native glyph width). */
    if (s_ticker_state.x_start + s_ticker_state.text_px_w < 0) {
        s_ticker_state.running = false;
        ha_mqtt_ticker_clear();
        /* Blank all tubes so the normal mode gets a clean canvas on the next tick */
        for (int _t = 0; _t < LCD_COUNT; _t++) display_fill(_t, 0x0000);
        ESP_LOGI(TAG, "Ticker scroll complete");
    }
}

/* ── Ticker speed API ────────────────────────────────────────────────
 * Set/get the marquee scroll speed in on-screen pixels per 200 ms tick.
 * Driven by the Home Assistant MQTT "Ticker Speed" number entity.
 * A plain int read/write is atomic on the ESP32, so no lock is needed:
 * the display task reads s_ticker_scroll_px once per tick and the MQTT
 * task writes it from the event handler. */
void display_set_ticker_speed(int px)
{
    if (px < TICKER_SCROLL_MIN) px = TICKER_SCROLL_MIN;
    if (px > TICKER_SCROLL_MAX) px = TICKER_SCROLL_MAX;
    s_ticker_scroll_px = px;
    ESP_LOGI(TAG, "Ticker speed set to %d px/tick", px);
}

int display_get_ticker_speed(void)
{
    return s_ticker_scroll_px;
}

/* ── Main display task ──────────────────────────────────────────────── */
static void display_task(void *arg)
{
    s_timer_mutex   = xSemaphoreCreateMutex();
    s_cx_push_mutex = xSemaphoreCreateMutex();
    s_timer_start  = xTaskGetTickCount();

    /* Pre-allocate shared PSRAM buffers used by the image cache and flip
     * animation.  Doing this here (rather than in display_init) so the
     * macros and statics are already in scope. */
    s_jpeg_work_buf  = PSRAM_MALLOC(JPEG_WORK_BUF_SIZE);
    s_flip_frame_buf = PSRAM_MALLOC(FLIP_FRAME_BYTES);
    if (!s_jpeg_work_buf || !s_flip_frame_buf)
        ESP_LOGW(TAG, "Failed to pre-allocate decode buffers — performance degraded");

    /* Boot splash: show wait screen on all tubes while the rest of the system
     * initialises (WiFi, NTP, weather, etc.).  The first normal render cycle
     * (first = true) overwrites this as soon as real content is ready. */
    for (int _i = 0; _i < LCD_COUNT; _i++) {
        display_show_image(_i, "/images/system/wait.jpg");
    }

    /* Per-render state for change detection */
    struct tm     last_t        = {0};
    time_t        last_display_epoch = 0;   /* time_t of the last rendered clock second;
                                             * used to clamp per-tick advance so large NTP
                                             * jumps animate rather than teleport. */
    app_mode_t    last_mode     = (app_mode_t)-1;
    char          last_theme[32]      = {0};
    char          last_clock_face[32] = {0};   /* track clockface changes alongside theme */
    /* Custom-face settings — checked independently of theme so colour / font /
     * background changes apply immediately without a mode or theme switch. */
    char          last_custom_bg[32]          = {0};
    uint8_t       last_custom_glyph_color[3]  = {0};
    uint8_t       last_custom_font_color[3]   = {0};
    bool          last_custom_shadow          = false;
    uint8_t       last_custom_shadow_color[3] = {0};
    char          last_custom_font[64]        = {0};
    char          last_time_type[8]   = {0};
    uint32_t      last_subs     = UINT32_MAX;
    uint32_t      last_insta    = UINT32_MAX;
    uint32_t      last_tiktok   = UINT32_MAX;
    uint32_t      last_mastodon = UINT32_MAX;
    int32_t       last_remain_s = INT32_MAX;  /* countdown/pomodoro change detection */
    float         last_temp_c   = -9999.0f;   /* weather change detection */
    float         last_hum      = -1.0f;
    bool          last_wx_valid = false;       /* detect when data first arrives */
    int           last_wx_min   = -1;          /* solar time change detection (minute) */
    bool          last_leading_zero = false;    /* leading-zero change detection */
    bool          last_cx_dual  = false;       /* 24H_CX dual-panel toggle change detection */
    bool          last_bl_on    = true;        /* backlight on/off tracking */
    uint8_t       last_bl_brt   = 255;         /* sentinel: force-apply on first tick */
    TickType_t    album_switch        = 0;
    TickType_t    rotation_tick       = 0;     /* tick when current mode started */
    TickType_t    theme_rotation_tick = 0;     /* tick when current theme started */
    bool          last_mode_rot_en    = false; /* mode-rotation enable edge tracker  */
    bool          last_theme_rot_en   = false; /* theme-rotation enable edge tracker */
    int           weather_panel      = WEATHER_PANEL_TEMP;
    TickType_t    weather_panel_tick = 0;      /* tick of last panel switch */
    int           hilo_phase        = 0;       /* 0 = show HI, 1 = show LO (panel 4 only) */
    bool          first              = true;
    int           last_hour             = -1;  /* for hourly burn-in shift update */
    int           burnin_last_run_yday  = -1;  /* day-of-year the scheduled burn-in last ran */
    /* AP PIN exit guard: set true each AP-PIN tick so the FIRST normal-mode
     * tick after a client connects does a blank fill instead of a JPEG read.
     * Reason: WPA2 4-way handshake runs AES ops on Core 0 at a high interrupt
     * level; if Core 1 issues a SPI flash IPC at that moment Core 0 can't ACK
     * it in time and the IWDT fires.  One 500 ms blank tick is enough for the
     * handshake to complete before the first colon/ampm JPEG is loaded. */
    bool          ap_pin_transition     = false;
    uint8_t       last_burnin_snap      = 0;    /* (s_burnin_mask|s_snow_mask) on previous tick */
    bool          last_ntp_synced       = false; /* detect boot-NTP sync transition; reset clamp */
    bool          last_time_valid       = false; /* detect invalid→valid time transition (RTC or NTP) */

    TickType_t wake = xTaskGetTickCount();
    rotation_tick       = wake;
    theme_rotation_tick = wake;

    /* Config snapshot — static so it lives in BSS rather than on the task
     * stack.  nextube_config_t is ~1900 bytes; keeping it on the stack
     * consumed nearly a quarter of the 12 KB budget and pushed the total
     * frame depth (cfg_snap + JPEG decode call chain) over the limit,
     * producing a truncated panic with no backtrace.  The display task is
     * single-instance and single-threaded, so a static local is safe. */
    static nextube_config_t cfg_snap;
    while (1) {
        /* OTA park request — honour it here, at the loop boundary, where no
         * SPI transaction is open and no buffers are mid-render.  The task
         * stays suspended until reboot (OTA always restarts). */
        if (s_park_req) {
            s_parked = true;
            vTaskSuspend(NULL);
        }

        config_lock();
        cfg_snap = *config_get();
        config_unlock();
        const nextube_config_t *cfg = &cfg_snap;
        app_mode_t mode = cfg->current_mode;
        bool mode_changed  = (mode != last_mode);
        bool theme_changed = (strcmp(cfg->theme,      last_theme)      != 0) ||
                             (strcmp(cfg->clock_face, last_clock_face) != 0);
        /* Custom face settings that don't touch theme or clock_face but still
         * need an immediate repaint (colour, shadow, font, background swap). */
        bool custom_changed = (strcmp(cfg->custom_bg,          last_custom_bg)          != 0) ||
                              (memcmp(cfg->custom_glyph_color,  last_custom_glyph_color,  3) != 0) ||
                              (memcmp(cfg->custom_font_color,   last_custom_font_color,   3) != 0) ||
                              (cfg->custom_shadow              != last_custom_shadow)           ||
                              (memcmp(cfg->custom_shadow_color, last_custom_shadow_color, 3) != 0) ||
                              (strcmp(cfg->custom_font,         last_custom_font)         != 0);

        /* ── Forced full repaint ─────────────────────────────────────────────
         * display_invalidate() (called automatically by display_apply_tube_offsets
         * and available to other callers) sets s_full_repaint_request to signal
         * that all tubes must be redrawn at their current settings this tick.
         *
         * Without this, a tube whose content hasn't changed (e.g. the hours digit
         * in 24H_NS / 24H_CX mode) won't pick up a new col/row offset until its
         * digit finally ticks over — leaving right-edge static visible for up to
         * an hour.  Handled inside the display task so there is no SPI bus
         * contention with the calling task (web server).
         *
         * Setting mode_changed = true propagates into every downstream render
         * condition (first || mode_changed || ...) and fires the state-reset block
         * below, which resets album / weather-panel / timer as on a normal mode
         * switch.  The explicit sentinel resets cover render paths that don't check
         * mode_changed (e.g. YouTube subscriber count, weather data values). */
        if (s_full_repaint_request) {
            s_full_repaint_request = false;
            for (int _t = 0; _t < LCD_COUNT; _t++) display_fill(_t, 0x0000);
            mode_changed  = true;
            last_t        = (struct tm){0};
            last_subs     = UINT32_MAX;
            last_insta    = UINT32_MAX;
            last_tiktok   = UINT32_MAX;
            last_mastodon = UINT32_MAX;
            last_temp_c   = -9999.0f;
            last_hum      = -1.0f;
            last_wx_valid = false;
            last_wx_min   = -1;
        }
        /* Config saved via WebUI: force mode_changed so every render path
         * (including WeatherLive / Custom face) picks up new custom_* values
         * on this tick, bypassing any stale change-detection cursor.  No tube
         * blank fill — the renders themselves overwrite the full tube content. */
        if (s_settings_saved) {
            s_settings_saved = false;
            mode_changed  = true;
        }

        /* ── Mode rotation ───────────────────────────────────────────
         * Only fires when rotation_enabled is true.  Any mode change
         * (UI, button, or previous rotation step) resets the timer so
         * the new mode gets its full weighted dwell before advancing.
         *
         * Effective dwell = rotation_interval_s × rotation_weights[mode].
         * A weight of 1 (default) gives the base interval; a weight of 10
         * keeps the current mode on-screen 10× longer than a weight-1 mode. */
        /* Reset the baseline when mode rotation is first enabled, so the first
         * switch waits a full interval instead of firing immediately off a
         * stale (boot-time) tick. */
        if (cfg->rotation_enabled && !last_mode_rot_en)
            rotation_tick = xTaskGetTickCount();
        last_mode_rot_en = cfg->rotation_enabled;

        if (mode_changed) {
            rotation_tick = xTaskGetTickCount();
        } else if (cfg->rotation_enabled && !first) {
            uint16_t interval = cfg->rotation_interval_s ? cfg->rotation_interval_s : 60;
            uint8_t  weight   = cfg->rotation_weights[cfg->current_mode];
            if (weight < 1) weight = 1;
            uint64_t dwell_ms = (uint64_t)interval * weight * 1000u;
            uint32_t elapsed_ms = (uint32_t)pdTICKS_TO_MS(
                                      xTaskGetTickCount() - rotation_tick);
            if ((uint64_t)elapsed_ms >= dwell_ms) {
                config_advance_mode();   /* updates cfg->current_mode + saves */
                rotation_tick = xTaskGetTickCount();
            }
        }

        /* ── Theme rotation ─────────────────────────────────────────────
         * Fires independently of mode rotation.  Any theme change (web UI
         * save or a previous rotation step) resets the timer so each theme
         * gets its full interval.  advance_theme() is RAM-only; the cache
         * flush fires on the next tick when theme_changed becomes true.  */
        /* Reset the baseline when theme rotation is first enabled, so the first
         * theme switch waits a full interval instead of firing immediately off
         * a stale (boot-time) tick. */
        if (cfg->theme_rotation_enabled && !last_theme_rot_en)
            theme_rotation_tick = xTaskGetTickCount();
        last_theme_rot_en = cfg->theme_rotation_enabled;

        if (theme_changed) {
            theme_rotation_tick = xTaskGetTickCount();
        } else if (cfg->theme_rotation_enabled && !first) {
            uint16_t t_int = cfg->theme_rotation_interval_s
                             ? cfg->theme_rotation_interval_s : 300;
            uint32_t t_ms  = (uint32_t)pdTICKS_TO_MS(
                                 xTaskGetTickCount() - theme_rotation_tick);
            if (t_ms >= (uint32_t)t_int * 1000u) {
                /* Diagnostic: prints the interval actually used at runtime so a
                 * mismatch with the configured value is visible in the log. */
                ESP_LOGI(TAG, "Theme rotation fired: interval=%u s, elapsed=%u ms",
                         (unsigned)t_int, (unsigned)t_ms);
                advance_theme(cfg->theme,
                              cfg->theme_rotation_count,
                              (const char (*)[THEME_NAME_MAX_ROT])cfg->theme_rotation_themes);
                theme_rotation_tick = xTaskGetTickCount();
            }
        }

        if (theme_changed) {
            img_cache_flush();       /* evict stale paths from the old theme */
            s_theme_error[0] = '\0'; /* clear any decode error from the previous theme */
        }

        if (mode_changed || theme_changed || first) {
            /* Reset album and weather panel state on mode/theme switch */
            s_album_loaded = false; s_album_index = 0; album_switch = 0;
            weather_panel  = WEATHER_PANEL_TEMP; weather_panel_tick = 0; hilo_phase = 0;
            last_display_epoch = 0;   /* clear clock smoothing state so first fresh render
                                       * uses the true system time without clamping */
        }
        if (mode_changed || first) {
            /* Reset countdown/pomodoro timer only on mode switch — NOT on theme change,
             * so a running countdown survives theme rotation. */
            last_remain_s = INT32_MAX;
            display_timer_reset();
        }

        /* Apply backlight on/off whenever the config changes.
         * Default to primary lcd_brightness, overridden by Night Mode if enabled. */
        uint8_t target_brt = cfg->lcd_brightness;
        struct tm now_tm;

        if (cfg->auto_brightness && ntp_has_valid_time()) {
            ntp_get_local(&now_tm);
            int hr = now_tm.tm_hour;
            bool is_night = false;
            uint8_t start = cfg->night_start_hour;
            uint8_t end   = cfg->night_end_hour;

            if (start < end) {
                if (hr >= start && hr < end) is_night = true;
            } else {
                /* Wraps around midnight (e.g. 22:00 to 07:00) */
                if (hr >= start || hr < end) is_night = true;
            }
            if (is_night) target_brt = cfg->night_brightness;
        }

        if (first || cfg->backlight_on != last_bl_on ||
                     target_brt != last_bl_brt) {
            display_set_brightness(cfg->backlight_on ? target_brt : 0);
            last_bl_on  = cfg->backlight_on;
            last_bl_brt = target_brt;
        }

        /* ── Anti burn-in: hourly pixel shift + scheduled trigger ──────────
         * Read current hour (NTP local time) and update s_burnin_shift_x
         * whenever it changes.  Formula: hour%5 - 2 → {-2,-1,0,+1,+2}.
         * Self-correcting from NTP; no step counter needed.
         * Also checks at midnight whether the scheduled burn-in interval
         * (weekly = Sunday, monthly = 1st of month) has been met, and
         * fires display_set_burnin_mask() automatically if so.
         * Skipped on the very first tick (first==true). */
        if (!first) {
            struct tm burn_tm; ntp_get_local(&burn_tm);

            /* Hourly pixel-shift update.
             * Guard on ntp_has_valid_time(): without this, epoch time (hour=0)
             * would set s_burnin_shift_x = 0%5-2 = -2 on the very first tick
             * before NTP or RTC provides the real hour, causing every tube to
             * appear shifted -2 px on most boot-ups until the real hour arrives. */
            if (ntp_has_valid_time() && burn_tm.tm_hour != last_hour) {
                last_hour        = burn_tm.tm_hour;
                s_burnin_shift_x = (int8_t)(last_hour % 5 - 2);
                ESP_LOGI(TAG, "burn-in shift: hour=%d  x=%+d px",
                         last_hour, (int)s_burnin_shift_x);
            }

            /* ── Scheduled burn-in: midnight trigger ─────────────────────
             * Only evaluates at midnight (tm_hour == 0).
             * burnin_last_run_yday tracks the last day-of-year that was
             * checked to prevent re-triggering every 200 ms tick while
             * still within the same midnight window.  Any day stamped as
             * "checked" is skipped on subsequent ticks that same day, so
             * only one check per calendar day is ever performed.
             * An already-active session (s_burnin_mask != 0) is not
             * interrupted — a manual session takes precedence. */
            if (cfg->burnin_auto_enabled && !s_burnin_mask &&
                    burn_tm.tm_hour == (int)cfg->burnin_auto_hour &&
                    burn_tm.tm_yday != burnin_last_run_yday) {

                burnin_last_run_yday = burn_tm.tm_yday; /* stamp regardless of fire */

                bool should_fire = false;
                if (strcmp(cfg->burnin_auto_interval, "monthly") == 0)
                    should_fire = (burn_tm.tm_mday == 1);
                else                                       /* "weekly" = every Sunday */
                    should_fire = (burn_tm.tm_wday == 0);

                if (should_fire) {
                    if (strcmp(cfg->burnin_auto_mode, "snow") == 0)
                        display_set_snow_mask(cfg->burnin_auto_mask,
                                              cfg->burnin_auto_duration_s);
                    else
                        display_set_burnin_mask(cfg->burnin_auto_mask,
                                                cfg->burnin_auto_duration_s);
                    ESP_LOGI(TAG,
                             "Scheduled burn-in fired (%s, %s): mask=0x%02X  dur=%lus",
                             cfg->burnin_auto_interval,
                             cfg->burnin_auto_mode,
                             (unsigned)cfg->burnin_auto_mask,
                             (unsigned long)cfg->burnin_auto_duration_s);
                }
            }
        }

        /* ── S1 — AP PIN takeover ─────────────────────────────────────
         * Whenever the setup AP is broadcasting AND no client is associated,
         * the tubes show the WPA2 PIN instead of the user's selected mode.
         * As soon as a client connects the PIN auto-hides and normal-mode
         * rendering resumes on the next tick. */
        if (wifi_manager_ap_pin_visible()) {
            display_set_brightness(100);
            render_ap_pin(cfg);
            /* Keep last_theme in sync with the current theme so that
             * theme_changed evaluates to FALSE on subsequent AP-PIN ticks.
             * Without this, the `strcmp(cfg->theme, last_theme)` at the top
             * of the loop is always non-zero (last_theme never gets written
             * because the continue below skips line 2167), causing
             * img_cache_flush() to fire every 200 ms — evicting the freshly
             * decoded digit JPEGs and forcing a full LittleFS re-read each
             * tick.  Keeping last_theme current stops the spurious flush and
             * lets the cache warm across PIN render cycles. */
            strncpy(last_theme,      cfg->theme,      sizeof(last_theme)      - 1);
            last_theme[sizeof(last_theme) - 1]           = '\0';
            strncpy(last_clock_face, cfg->clock_face, sizeof(last_clock_face) - 1);
            last_clock_face[sizeof(last_clock_face) - 1] = '\0';
            strncpy(last_custom_bg,  cfg->custom_bg,  sizeof(last_custom_bg)  - 1);
            last_custom_bg[sizeof(last_custom_bg) - 1]  = '\0';
            memcpy(last_custom_glyph_color,  cfg->custom_glyph_color,  3);
            memcpy(last_custom_font_color,   cfg->custom_font_color,   3);
            last_custom_shadow = cfg->custom_shadow;
            memcpy(last_custom_shadow_color, cfg->custom_shadow_color, 3);
            strncpy(last_custom_font, cfg->custom_font, sizeof(last_custom_font) - 1);
            last_custom_font[sizeof(last_custom_font) - 1] = '\0';
            /* Force the remaining change-detection state to "no last frame"
             * so when the AP closes (or a client connects) the next
             * normal-mode tick re-renders from scratch — otherwise the
             * equality checks below would skip the redraw and leave PIN
             * digits on screen. */
            last_mode     = (app_mode_t)-1;
            last_t        = (struct tm){0};
            last_subs     = UINT32_MAX;
            last_insta    = UINT32_MAX;
            last_tiktok   = UINT32_MAX;
            last_mastodon = UINT32_MAX;
            last_remain_s = INT32_MAX;
            last_temp_c   = -9999.0f;
            last_hum      = -1.0f;
            last_wx_valid = false;
            last_wx_min   = -1;
            first         = true;
            ap_pin_transition = true;   /* arm the exit guard for the next tick */
            vTaskDelayUntil(&wake, pdMS_TO_TICKS(200));
            continue;
        }

        /* ── AP PIN exit guard ──────────────────────────────────────────
         * First normal-mode tick after a client connects: fill tubes black
         * and wait 500 ms before touching SPI flash.  During WPA2 4-way
         * handshake the WiFi driver runs AES ops on Core 0 at a high
         * interrupt level; a SPI flash IPC sent at that moment won't get
         * an ACK and Core 1 waits with interrupts disabled until the IWDT
         * fires.  display_fill() is pure SPI-to-LCD (no flash read), so
         * it completes instantly without involving Core 0's IPC path.
         *
         * Also resets the active mode to Clock (RAM-only, no flash write)
         * so the first rendered frame after connection is always the clock
         * face — confirming NTP time is working and giving the user a clean
         * entry point regardless of what mode was active before setup. */
        if (ap_pin_transition) {
            ap_pin_transition = false;
            s_pin_last_scroll = -1;   /* marquee repaints from scratch on re-entry */
            for (int _t = 0; _t < LCD_COUNT; _t++) display_fill(_t, 0x0000);
            config_set_mode(APP_MODE_CLOCK);
            ESP_LOGI(TAG, "AP PIN exit — mode reset to Clock");
            vTaskDelayUntil(&wake, pdMS_TO_TICKS(500));
            continue;
        }

        /* ── Burn-in / snow: timer expiry pre-check ─────────────────────────
         * Expire masks BEFORE the mode render so the same tick's render_*
         * calls see mask=0 and write JPEGs immediately.  Without this the
         * render skips JPEG writes (mask still set), the expiry fires after,
         * and the last solid colour sits on screen until the next render tick
         * — up to 2 s for clock modes; indefinitely for YouTube
         * (which only re-render on data changes, not on every tick).
         *
         * burnin_force_render: true on any tick where the combined mask just
         * transitioned non-zero → zero (either timed expiry above, or a Stop
         * command sent from the web UI between ticks).  Added to each mode's
         * render condition so restoration is immediate even when normal change
         * detection would otherwise skip the draw. */
        {
            time_t _exp_now = time(NULL);
            /* Hold mutex for both pairs of field clears so display_set_burnin_mask()
             * called concurrently from the web-server task cannot observe a torn state
             * (mask cleared, end_time still non-zero). */
            xSemaphoreTake(s_timer_mutex, portMAX_DELAY);
            if (s_burnin_mask && s_burnin_end_time != 0 &&
                    _exp_now >= s_burnin_end_time) {
                s_burnin_mask     = 0;
                s_burnin_end_time = 0;
                ESP_LOGI(TAG, "burn-in timer expired — restoring normal display");
            }
            if (s_snow_mask && s_snow_end_time != 0 &&
                    _exp_now >= s_snow_end_time) {
                s_snow_mask     = 0;
                s_snow_end_time = 0;
                ESP_LOGI(TAG, "snow timer expired — restoring normal display");
            }
            xSemaphoreGive(s_timer_mutex);
        }
        uint8_t cur_burnin_snap     = (uint8_t)(s_burnin_mask | s_snow_mask);
        bool    burnin_force_render = (last_burnin_snap != 0) && (cur_burnin_snap == 0);
        last_burnin_snap = cur_burnin_snap;
        /* Set true inside APP_MODE_WEATHER when the sun animation panel is
         * active; used below to drive FAST tick and per-frame anim updates. */
        bool    sun_anim            = false;

        /* ── MQTT ticker overlay ────────────────────────────────────────────
         * When a message is published to nextube/<host>/ticker/set the text
         * scrolls across all 6 tubes at 4 px/200 ms (≈ 20 px/s).  While the
         * ticker is running we skip mode rendering, burn-in, and rotation so
         * the ticker has exclusive control of the display for its duration.
         * The delay is issued here so the loop runs at the normal 5 Hz rate.
         * Backlight and brightness (applied above) continue to take effect.  */
        {
            char ticker_buf[TICKER_MAX_LEN + 1];
            if (ha_mqtt_ticker_active(ticker_buf, sizeof(ticker_buf))) {
                /* New message or text changed — (re-)initialise scroll */
                if (!s_ticker_state.running ||
                    strcmp(ticker_buf, s_ticker_state.text) != 0) {
                    strncpy(s_ticker_state.text, ticker_buf, TICKER_MAX_LEN);
                    s_ticker_state.text[TICKER_MAX_LEN] = '\0';
                    u8g2_SetFont(&s_u8g2, u8g2_font_logisoso28_tf);
                    /* On-screen width = native glyph width × TICKER_SCALE,
                     * since render_ticker blits the buffer at 2× scale. */
                    s_ticker_state.text_px_w = TICKER_SCALE *
                        (int)u8g2_GetUTF8Width(&s_u8g2, s_ticker_state.text);
                    /* Start with text fully off the right edge of the display */
                    s_ticker_state.x_start  = LCD_COUNT * LCD_WIDTH;
                    s_ticker_state.running  = true;
                    /* Blank every tube to solid black once, before the first
                     * scroll frame.  render_ticker() only repaints the 128-row
                     * text band (rows 16–143); without this one-time fill the
                     * 16-row top/bottom margins would keep whatever the previous
                     * mode drew.  The ticker has exclusive control of the display
                     * while running, so the black margins persist for the whole
                     * scroll — no need to refill every tick. */
                    for (int _t = 0; _t < LCD_COUNT; _t++)
                        display_fill(_t, 0x0000);
                    ESP_LOGI(TAG, "Ticker starting: \"%s\" (%d px)",
                             s_ticker_state.text, s_ticker_state.text_px_w);
                }
                render_ticker(cfg);
                /* Hold change-detection state "stale" while the ticker owns the
                 * display, so the first normal-mode tick after it ends repaints
                 * from scratch (the ticker blanked every tube).  Crucially,
                 * reset last_display_epoch so the clock does NOT fast-forward
                 * 1 s/tick (5×) through the ticker's whole duration on resume —
                 * that catch-up is what made the colon blink rapidly.  Same
                 * approach as the AP-PIN exit handling below. */
                last_mode          = (app_mode_t)-1;
                last_t             = (struct tm){0};
                last_display_epoch = 0;
                first              = true;
                strncpy(last_theme,      cfg->theme,      sizeof(last_theme)      - 1);
                last_theme[sizeof(last_theme) - 1]           = '\0';
                strncpy(last_clock_face, cfg->clock_face, sizeof(last_clock_face) - 1);
                last_clock_face[sizeof(last_clock_face) - 1] = '\0';
                strncpy(last_custom_bg,  cfg->custom_bg,  sizeof(last_custom_bg)  - 1);
                last_custom_bg[sizeof(last_custom_bg) - 1]  = '\0';
                memcpy(last_custom_glyph_color,  cfg->custom_glyph_color,  3);
                memcpy(last_custom_font_color,   cfg->custom_font_color,   3);
                last_custom_shadow = cfg->custom_shadow;
                memcpy(last_custom_shadow_color, cfg->custom_shadow_color, 3);
                strncpy(last_custom_font, cfg->custom_font, sizeof(last_custom_font) - 1);
                last_custom_font[sizeof(last_custom_font) - 1] = '\0';
                vTaskDelayUntil(&wake, pdMS_TO_TICKS(DISPLAY_TICK_MS_SLOW));
                continue;   /* skip mode switch, burn-in, rotation this tick */
            } else {
                /* Ticker not active — ensure running flag is clear */
                s_ticker_state.running = false;
            }
        }

        /* True when the sky background itself actually animates this tick.
         * Custom clockface with a static (non-WL) custom_bg uses the WL
         * renderer but its background is a fixed theme image — no 20 Hz
         * sky redraws needed; let it run on the slow tick like any JPEG theme. */
        bool wl_sky_animates = cx_is_wl_sky(cfg) &&
                               !(s_wl_is_custom && s_wl_bg_theme[0] != '\0'
                                 && strncmp(s_wl_bg_theme, "WeatherLive", 11) != 0);
        bool wl_anim_tick = cfg->wlive_animate && wl_sky_animates && s_wl_scene_valid;

        /* Pre-compute the live timer value for countdown/pomodoro so the anim
         * block always renders the current second, not the 1-tick-stale
         * last_remain_s.  Both loops (anim block + main switch) now see the
         * same value, eliminating the "2 loops fighting" phase offset. */
        int32_t cd_remain_now        = (last_remain_s == INT32_MAX) ? 0 : last_remain_s;
        bool    cd_pomo_break_now    = s_pomo_in_break;
        if ((mode == APP_MODE_COUNTDOWN || mode == APP_MODE_POMODORO) &&
                !first && !mode_changed) {
            xSemaphoreTake(s_timer_mutex, portMAX_DELAY);
            uint32_t cd_elapsed = s_timer_paused
                ? s_paused_elapsed_ms
                : (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - s_timer_start);
            cd_pomo_break_now = s_pomo_in_break;
            xSemaphoreGive(s_timer_mutex);
            if (mode == APP_MODE_COUNTDOWN) {
                int32_t total = (int32_t)cfg->countdown_minutes * 60;
                cd_remain_now = total - (int32_t)(cd_elapsed / 1000);
                if (cd_remain_now < 0) cd_remain_now = 0;
            } else {
                int32_t period = cd_pomo_break_now
                    ? (int32_t)cfg->pomodoro_break * 60
                    : (int32_t)cfg->pomodoro_work  * 60;
                cd_remain_now = period - (int32_t)(cd_elapsed / 1000);
                if (cd_remain_now < 0) cd_remain_now = 0;
            }
        }

        /* ── Continuous WeatherLive background render ────────────────────────
         * When the sky is animated this single block acts as one unified
         * background loop that renders sky + cached mode content every tick,
         * independent of per-mode data-change conditions in the switch below.
         * The switch cases update tracking state and handle non-WL renders;
         * they only repaint on first/mode-change ticks when WL is animated. */
        if (wl_anim_tick && !first && !mode_changed) {
            /* Advance precipitation particles when not in clock mode.
             * render_weatherlive() owns this step during clock ticks; here we
             * cover all other animated modes so rain/snow visibly falls. */
            if (s_wl_last_scene.precip) {
                static int64_t s_wl_part_tick_us = 0;
                int64_t pnow = esp_timer_get_time();
                float pdt = (s_wl_part_tick_us > 0)
                    ? (float)(pnow - s_wl_part_tick_us) / 1e6f : 0.05f;
                if (pdt < 0.0f) pdt = 0.0f;
                if (pdt > 0.25f) pdt = 0.25f;
                s_wl_part_tick_us = pnow;
                if (!s_wl_part_init) {
                    for (int i = 0; i < WL_NPART; i++) {
                        int pt = esp_random() % LCD_COUNT;
                        s_wl_part[i].x = (float)(pt * WL_TUBE_STRIDE + (int)(esp_random() % LCD_WIDTH));
                        s_wl_part[i].y = (float)(esp_random() % LCD_HEIGHT);
                        s_wl_part[i].vy = 0;
                        s_wl_part[i].drift = (int8_t)((esp_random() % 3) - 1);
                    }
                    s_wl_part_init = true;
                }
                float pvy = (s_wl_last_scene.precip == 2) ? 32.0f : 165.0f;
                for (int i = 0; i < WL_NPART; i++) {
                    s_wl_part[i].y += pvy * pdt;
                    if (s_wl_last_scene.precip == 2)
                        s_wl_part[i].x += ((float)s_wl_part[i].drift * 6.0f
                                           + s_wl_last_scene.wind * 34.0f) * pdt;
                    if (s_wl_part[i].y >= LCD_HEIGHT) {
                        int pt = esp_random() % LCD_COUNT;
                        s_wl_part[i].x = (float)(pt * WL_TUBE_STRIDE + (int)(esp_random() % LCD_WIDTH));
                        s_wl_part[i].y -= LCD_HEIGHT;
                        s_wl_part[i].drift = (int8_t)((esp_random() % 3) - 1);
                    }
                }
            }
            switch (mode) {
            case APP_MODE_YOUTUBE:
                if (cfg->youtube_id[0] == '\0') render_followers_blank(cfg, "youtube");
                else                             render_followers(cfg, last_subs, "youtube");
                break;
            case APP_MODE_INSTAGRAM:
                if (cfg->instagram_user[0] == '\0') render_followers_blank(cfg, "instagram");
                else                                 render_followers(cfg, last_insta, "instagram");
                break;
            case APP_MODE_TIKTOK:
                if (cfg->tiktok_user[0] == '\0') render_followers_blank(cfg, "tiktok");
                else                              render_followers(cfg, last_tiktok, "tiktok");
                break;
            case APP_MODE_MASTODON:
                if (cfg->mastodon_user[0] == '\0' || cfg->mastodon_instance[0] == '\0')
                    render_followers_blank(cfg, "mastodon");
                else
                    render_followers(cfg, last_mastodon, "mastodon");
                break;
            case APP_MODE_DATE:
                render_date(cfg, &last_t);
                break;
            case APP_MODE_COUNTDOWN:
                render_countdown_display(cfg, cd_remain_now);
                break;
            case APP_MODE_POMODORO:
                render_pomodoro_display(cfg, cd_remain_now, cd_pomo_break_now);
                break;
            case APP_MODE_WEATHER:
                render_weather(cfg, weather_panel, false, hilo_phase);
                break;
            default: break;
            }
        }

        switch (mode) {

        case APP_MODE_CLOCK: {
            /* ── Time-valid guard ────────────────────────────────────────────
             * When the RTC seed is absent or too old (< 2025-01-01) AND NTP has
             * not yet completed its first sync, time() returns seconds since boot
             * starting from epoch 0.  Without this guard the clock tubes show
             * "00:00:XX" counting up every second — indistinguishable from a
             * stopwatch.
             *
             * Behaviour:
             *   • Invalid time → tubes go black; no render; log once on entry.
             *   • Invalid → valid transition → reset last_display_epoch so the
             *     very next tick (which falls through to render) shows the true
             *     time immediately with no clamp hold or fast-forward. */
            {
                bool time_valid = ntp_has_valid_time();
                if (!time_valid) {
                    /* Only issue the fill when ENTERING the invalid state —
                     * i.e. on the first tick after losing/not-yet-having valid
                     * time (last_time_valid was true coming in, or first/mode
                     * changed).  Subsequent ticks stay blank with no SPI write. */
                    if (first || mode_changed || last_time_valid) {
                        for (int _t = 0; _t < LCD_COUNT; _t++)
                            display_fill(_t, 0x0000);
                        ESP_LOGI(TAG, "Clock: no valid time source — tubes blanked");
                    }
                    last_time_valid = false;
                    break;   /* skip all render logic until time is known-good */
                }
                if (!last_time_valid) {
                    /* Time just became valid (RTC seeded or NTP synced).
                     * Reset the clamp reference so the first real render is
                     * instantaneous — no hold or fast-forward animation. */
                    last_display_epoch = 0;
                    ESP_LOGI(TAG, "Clock: valid time acquired — display clamp reset");
                }
                last_time_valid = true;
            }

            /* ── Per-tick time clamping: smooth NTP step corrections ────────────
             * The NTP layer uses SNTP_SYNC_MODE_IMMED: boot syncs always apply
             * a hard settimeofday(); periodic re-syncs within 60 s use adjtime()
             * to slew, while larger drifts keep the hard jump.  Without clamping
             * here, any hard settimeofday() would teleport the clock tubes by
             * 8+ digits at once.
             *
             * With clamping (CLOCK_MAX_STEP_S = 1):
             *   Forward jump  → tubes fast-count at 5× real speed (one second
             *                   per 200 ms tick) until display catches up.
             *                   An 8-second correction animates in ~1.6 s.
             *   Backward jump → tubes hold the current second; the system clock
             *                   marches forward from its new position and meets
             *                   the display after |delta| seconds, then resumes
             *                   normally.  No tube ever shows a decreasing digit.
             *
             * Clamping is bypassed on first/mode-change ticks so a fresh render
             * always shows the true system time immediately.
             *
             * Boot-NTP bypass: when the NTP layer completes its first sync it
             * may hard-set the clock by many hours (stale/missing RTC battery).
             * Detecting the false→true transition of ntp_time_synced() and
             * resetting last_display_epoch ensures the display jumps directly to
             * the correct NTP time rather than backward-holding for hours. */
#define CLOCK_MAX_STEP_S  1
            time_t now_epoch;
            time(&now_epoch);

            /* Detect first NTP sync — reset clamp reference so the next render
             * uses the true NTP time with no hold or fast-forward animation. */
            if (!last_ntp_synced && ntp_time_synced()) {
                last_ntp_synced    = true;
                last_display_epoch = 0;   /* bypass clamping on this tick */
                ESP_LOGI(TAG, "Boot NTP sync detected — display clamp reset");
            }

            /* Any no-seconds mode (24H_CX and 24H_NS) shows no seconds and —
             * when FlipClock is active — has no blinking colon either, so
             * nothing re-renders between minute boundaries to advance
             * last_display_epoch.  The per-tick clamp below would then pin the
             * time at +1 s and the minute would never roll over (clock appears
             * frozen) — so skip clamping for those combinations.
             *
             * dual_cx (24H_CX dual-panel) also has no colon blink regardless of
             * theme, and was the first case fixed; the nosec_flip path covers the
             * remaining no-seconds + FlipClock combinations. */
            bool dual_cx    = (strcmp(cfg->time_type, "24H_CX") == 0) && cfg->cx_dual_panel;
            bool nosec_flip = (strcmp(cfg->theme, "FlipClock") == 0) &&
                              (strcmp(cfg->time_type, "24H_CX") == 0 ||
                               strcmp(cfg->time_type, "24H_NS") == 0);
            if (last_display_epoch > 0 && !first && !mode_changed && !theme_changed &&
                !dual_cx && !nosec_flip) {
                time_t delta = now_epoch - last_display_epoch;
                if (delta > CLOCK_MAX_STEP_S) {
                    now_epoch = last_display_epoch + CLOCK_MAX_STEP_S; /* fast-forward */
                } else if (delta < 0) {
                    now_epoch = last_display_epoch;                    /* hold; never rewind */
                }
            }
            struct tm t;
            localtime_r(&now_epoch, &t);

            bool is_24ns  = (strcmp(cfg->time_type, "24H_NS") == 0);
            bool is_24cx  = (strcmp(cfg->time_type, "24H_CX") == 0);  /* dual_cx computed above */
            bool dual_changed = (dual_cx != last_cx_dual);
            bool is_nosec = is_24ns || is_24cx;
            bool is_flip  = (strcmp(cfg->theme, "FlipClock")  == 0);
            bool time_type_changed    = (strcmp(cfg->time_type, last_time_type) != 0);
            bool leading_zero_changed = (cfg->leading_zero != last_leading_zero);
            /* 24H_NS / 24H_CX show no seconds — only re-render on minute/hour change.
             * Standard 24H shows seconds and re-renders every second. */
            bool time_changed = (t.tm_hour != last_t.tm_hour ||
                                 t.tm_min  != last_t.tm_min);
            if (!is_nosec) time_changed |= (t.tm_sec != last_t.tm_sec);
            /* Colon blinks every other second — need a re-render on each
             * even/odd transition even when only the colon image changes. */
            bool colon_blink_changed = !is_flip && !dual_cx &&
                                       (t.tm_sec != last_t.tm_sec);

            /* 24H_CX: advance the info-panel rotation(s).  Tube 6 always rotates
             * its own enabled set; in dual mode tube 5 rotates its independent
             * set on the same timer.  Each index wraps modulo its own count. */
            bool panel_changed = false;
            if (is_24cx) {
                int cnt6 = (cfg->tube6_panel_weather  ? 1 : 0)
                         + (cfg->tube6_panel_weekdate ? 1 : 0)
                         + (cfg->tube6_panel_ht       ? 1 : 0)
                         + (cfg->tube6_panel_temp     ? 1 : 0)
                         + (cfg->tube6_panel_sunrise  ? 1 : 0)
                         + (cfg->tube6_panel_push     ? 1 : 0)
                         + (cfg->tube6_panel_humidity ? 1 : 0)
                         + (cfg->tube6_panel_wind     ? 1 : 0);
                if (cnt6 < 1) cnt6 = 1;  /* config enforces ≥1 */
                int cnt5 = (cfg->tube5_panel_weather  ? 1 : 0)
                         + (cfg->tube5_panel_weekdate ? 1 : 0)
                         + (cfg->tube5_panel_ht       ? 1 : 0)
                         + (cfg->tube5_panel_temp     ? 1 : 0)
                         + (cfg->tube5_panel_sunrise  ? 1 : 0)
                         + (cfg->tube5_panel_push     ? 1 : 0)
                         + (cfg->tube5_panel_humidity ? 1 : 0)
                         + (cfg->tube5_panel_wind     ? 1 : 0);
                if (cnt5 < 1) cnt5 = 1;
                uint32_t panel_ms = cfg->tube6_panel_ms < 1000 ? 5000
                                                                : cfg->tube6_panel_ms;
                int64_t  now_us = esp_timer_get_time();
                if (first || mode_changed || time_type_changed || dual_changed ||
                        s_cx_panel_start == 0) {
                    s_cx_panel = 0;  s_cx_panel5 = 0;
                    s_cx_panel_start = now_us;
                    s_cx_last_kind = -1;  s_cx_last_kind5 = -1;  /* force bg clear */
                    panel_changed    = true;
                } else if ((now_us - s_cx_panel_start) >= (int64_t)panel_ms * 1000LL) {
                    s_cx_panel_start = now_us;   /* always reset timer */
                    uint8_t n6 = (uint8_t)((s_cx_panel + 1) % cnt6);
                    if (n6 != s_cx_panel) { s_cx_panel = n6; panel_changed = true; }
                    if (dual_cx) {
                        uint8_t n5 = (uint8_t)((s_cx_panel5 + 1) % cnt5);
                        if (n5 != s_cx_panel5) { s_cx_panel5 = n5; panel_changed = true; }
                    }
                }
            }

            /* ── Clock digits (+ colon in single-panel layout) ───────────── */
            bool is_weatherlive = (strncmp(cfg->theme, "WeatherLive", 11) == 0) ||
                                  (strcmp(cfg->clock_face, "custom") == 0);
            if (is_weatherlive) {
                /* Procedural theme: draws all six tubes (sky, sun/moon, clouds,
                 * precip, glyphs, blinking colon) in one self-contained pass, and
                 * never falls through to the JPEG colon-blink / cx-panel paths.
                 * Realtime: re-render EVERY tick so the animation advances.
                 * Static: render only on a clock/layout change (the tick also
                 * drops to slow), so it's a still snapshot at near-zero load. */
                if (wl_anim_tick ||
                        first || mode_changed || theme_changed || custom_changed ||
                        time_type_changed || time_changed || leading_zero_changed ||
                        dual_changed || burnin_force_render) {
                    /* Colon cache: invalidate on any config/mode/theme/layout
                     * change so wl_render_colon_tube() rebuilds the ON/OFF
                     * pixel buffers with fresh colours and background.
                     * Normal second-tick renders (time_changed only) keep
                     * the existing cache and use the fast push path.          */
                    if (first || mode_changed || theme_changed || custom_changed ||
                            time_type_changed || leading_zero_changed ||
                            dual_changed || burnin_force_render)
                        s_wl_colon_cache_valid = false;
                    render_clock(cfg, &t);   /* → render_weatherlive() */
                    /* render_weatherlive() early-returns during a busy backoff
                     * (NVS save).  For static custom faces the colon would
                     * freeze for the entire save window (up to 3 s) because
                     * time_changed keeps hitting this branch rather than the
                     * else-if colon path below.  If the cache is still valid
                     * and no config rebuild is needed, push the colon state
                     * directly so it keeps blinking through the busy window. */
                    if (esp_timer_get_time() < s_busy_until_us &&
                            s_wl_colon_cache_valid &&
                            !first && !mode_changed && !theme_changed && !custom_changed)
                        wl_show_colon_blink(t.tm_sec % 2 == 0);
                    last_t = t;
                    last_display_epoch = now_epoch;
                } else if (colon_blink_changed) {
                    /* Only tube 2 needs updating — push it alone instead of
                     * re-rendering all six tubes.  On a static custom face the
                     * full render was too slow to finish within the 200 ms tick,
                     * which caused the blink cadence to drift and appear
                     * inconsistent.
                     *
                     * Exception: when the tube-5 info panel is due to rotate
                     * (24H_NS / 24H_CX static faces), wl_draw_panel inside
                     * render_weatherlive is the only thing that advances k6,
                     * so a full render is needed instead. */
                    bool wl_panel_due = false;
                    if (is_nosec && !cfg->wlive_animate) {
                        static int64_t s_wl_nspanel_last_us;
                        int64_t now_us_p = esp_timer_get_time();
                        int64_t rot_us   = (int64_t)(cfg->tube6_panel_ms >= 1000
                                            ? cfg->tube6_panel_ms : 5000) * 1000LL;
                        if (s_wl_nspanel_last_us == 0 ||
                                (now_us_p - s_wl_nspanel_last_us) >= rot_us) {
                            s_wl_nspanel_last_us = now_us_p;
                            wl_panel_due = true;
                        }
                    }
                    if (wl_panel_due) {
                        render_clock(cfg, &t);
                    } else if (s_wl_is_custom && s_wl_bg_theme[0] != '\0' &&
                                strncmp(s_wl_bg_theme, "WeatherLive", 11) != 0) {
                        wl_show_colon_blink(t.tm_sec % 2 == 0);
                    } else if (s_wl_scene_valid) {
                        wl_draw_tube(2, (t.tm_sec % 2 == 0) ? ':' : ' ', &s_wl_last_scene);
                    }
                    last_t = t;
                    last_display_epoch = now_epoch;
                }
            } else if (first || mode_changed || theme_changed || time_type_changed ||
                    time_changed || leading_zero_changed || dual_changed ||
                    burnin_force_render) {
                render_clock(cfg, &t);
                last_t = t;
                last_display_epoch = now_epoch;
            } else if (colon_blink_changed) {
                /* Only the colon image changed — update tube 2 alone.
                 * Avoids re-rendering all clock tubes every second in 24H_NS
                 * and 24H_CX where time_changed only fires on minute boundaries.
                 * In plain 24H the seconds digits change every second so
                 * time_changed fires first and this branch is never reached.
                 * FlipClock: colon_blink_changed is always false so also safe.
                 *
                 * Uses display_show_colon_blink() — a partial push of only the
                 * colon-dot diff box, not the full 80×160 tube — to minimise the
                 * per-second SPI burst that couples into the amplifier. */
                display_show_colon_blink(2, cfg->theme, t.tm_sec % 2 == 0);
                last_t = t;
                last_display_epoch = now_epoch;
            }

            /* ── 24H_CX info panel(s): tube 6 (LCD 5) always; tube 5 (LCD 4)
             * in dual mode.  Re-render only when a panel rotates, the data
             * changes, or a mode/theme/layout change forces a full redraw.
             * Colon blinks and second ticks do NOT affect panel content.
             * s_cx_last_t tracks the last time the panels were drawn. */
            if (is_24cx && !is_weatherlive) {
                /* A freshly pushed image (s_cx_push_seq bumped by the httpd task)
                 * forces a re-render so it appears without waiting for the next
                 * rotation/minute tick when its panel is currently on-screen. */
                static uint32_t last_push_seq = 0;
                uint32_t push_seq = s_cx_push_seq;
                bool push_changed = (push_seq != last_push_seq);
                last_push_seq = push_seq;
                bool cx_render =
                    first || mode_changed || theme_changed || time_type_changed ||
                    dual_changed || panel_changed || burnin_force_render || push_changed ||
                    t.tm_min  != s_cx_last_t.tm_min  ||  /* H/T: refresh each minute */
                    t.tm_mday != s_cx_last_t.tm_mday;    /* date panel: new day      */
                if (cx_render) {
                    const bool en6[8] = {
                        cfg->tube6_panel_weather, cfg->tube6_panel_weekdate,
                        cfg->tube6_panel_ht,      cfg->tube6_panel_temp,
                        cfg->tube6_panel_sunrise, cfg->tube6_panel_push,
                        cfg->tube6_panel_humidity, cfg->tube6_panel_wind,
                    };
                    render_cx_panel(cfg, &t, 5, en6, s_cx_panel, &s_cx_last_kind);
                    if (dual_cx) {
                        const bool en5[8] = {
                            cfg->tube5_panel_weather, cfg->tube5_panel_weekdate,
                            cfg->tube5_panel_ht,      cfg->tube5_panel_temp,
                            cfg->tube5_panel_sunrise, cfg->tube5_panel_push,
                            cfg->tube5_panel_humidity, cfg->tube5_panel_wind,
                        };
                        render_cx_panel(cfg, &t, 4, en5, s_cx_panel5, &s_cx_last_kind5);
                    }
                    s_cx_last_t = t;
                }
            }
            strncpy(last_time_type, cfg->time_type, sizeof(last_time_type) - 1);
            last_time_type[sizeof(last_time_type) - 1] = '\0';
            last_leading_zero = cfg->leading_zero;
            last_cx_dual      = dual_cx;
            break;
        }

        case APP_MODE_DATE: {
            /* Re-render on day change, first draw, mode/theme switch.
             * WL animated: pre-switch loop handles sky redraws every tick;
             * this case only renders on first/mode-change and non-WL paths. */
            struct tm t; ntp_get_local(&t);
            bool day_changed = (t.tm_mday != last_t.tm_mday ||
                                t.tm_mon  != last_t.tm_mon  ||
                                t.tm_year != last_t.tm_year);
            if (first || mode_changed || theme_changed || day_changed || burnin_force_render) {
                if (!wl_anim_tick || first || mode_changed)
                    render_date(cfg, &t);
                last_t = t;
            }
            break;
        }

        case APP_MODE_COUNTDOWN: {
            xSemaphoreTake(s_timer_mutex, portMAX_DELAY);
            uint32_t elapsed_ms = s_timer_paused
                ? s_paused_elapsed_ms
                : (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - s_timer_start);
            xSemaphoreGive(s_timer_mutex);
            int32_t total  = (int32_t)cfg->countdown_minutes * 60;
            int32_t remain = total - (int32_t)(elapsed_ms / 1000);
            if (remain < 0) remain = 0;
            if (first || mode_changed || theme_changed || remain != last_remain_s ||
                    burnin_force_render) {
                if (!wl_anim_tick || first || mode_changed)
                    render_countdown_display(cfg, remain);
                last_remain_s = remain;
            }
            break;
        }

        case APP_MODE_POMODORO: {
            xSemaphoreTake(s_timer_mutex, portMAX_DELAY);
            bool     paused     = s_timer_paused;
            uint32_t elapsed_ms = paused
                ? s_paused_elapsed_ms
                : (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - s_timer_start);
            bool in_break = s_pomo_in_break;
            xSemaphoreGive(s_timer_mutex);

            int32_t period = in_break ? (int32_t)cfg->pomodoro_break * 60
                                      : (int32_t)cfg->pomodoro_work  * 60;
            int32_t remain = period - (int32_t)(elapsed_ms / 1000);
            if (remain <= 0) {
                if (!paused) {
                    /* Auto-flip work↔break only while the timer is running */
                    xSemaphoreTake(s_timer_mutex, portMAX_DELAY);
                    s_pomo_in_break     = !s_pomo_in_break;
                    s_timer_start       = xTaskGetTickCount();
                    s_paused_elapsed_ms = 0;
                    in_break            = s_pomo_in_break;
                    xSemaphoreGive(s_timer_mutex);
                    remain = in_break ? (int32_t)cfg->pomodoro_break * 60
                                      : (int32_t)cfg->pomodoro_work  * 60;
                } else {
                    remain = 0;   /* frozen at zero while paused */
                }
            }
            if (first || mode_changed || theme_changed || remain != last_remain_s ||
                    burnin_force_render) {
                if (!wl_anim_tick || first || mode_changed)
                    render_pomodoro_display(cfg, remain, in_break);
                last_remain_s = remain;
            }
            break;
        }

        case APP_MODE_YOUTUBE: {
            const sub_count_t *sub = subscribers_get();
            bool uncfg = (cfg->youtube_id[0] == '\0');
            uint32_t count = (!uncfg && sub->valid) ? (uint32_t)sub->subscriber_count : 0;
            if (first || mode_changed || theme_changed || count != last_subs ||
                    burnin_force_render) {
                if (!wl_anim_tick || first || mode_changed) {
                    if (uncfg) render_followers_blank(cfg, "youtube");
                    else        render_followers(cfg, count, "youtube");
                }
                last_subs = count;
            }
            break;
        }

        case APP_MODE_INSTAGRAM: {
            const sub_count_t *s = instagram_get();
            bool uncfg = (cfg->instagram_user[0] == '\0');
            uint32_t count = (!uncfg && s->valid) ? (uint32_t)s->subscriber_count : 0;
            if (first || mode_changed || theme_changed || count != last_insta ||
                    burnin_force_render) {
                if (!wl_anim_tick || first || mode_changed) {
                    if (uncfg) render_followers_blank(cfg, "instagram");
                    else        render_followers(cfg, count, "instagram");
                }
                last_insta = count;
            }
            break;
        }

        case APP_MODE_TIKTOK: {
            const sub_count_t *s = tiktok_get();
            bool uncfg = (cfg->tiktok_user[0] == '\0');
            uint32_t count = (!uncfg && s->valid) ? (uint32_t)s->subscriber_count : 0;
            if (first || mode_changed || theme_changed || count != last_tiktok ||
                    burnin_force_render) {
                if (!wl_anim_tick || first || mode_changed) {
                    if (uncfg) render_followers_blank(cfg, "tiktok");
                    else        render_followers(cfg, count, "tiktok");
                }
                last_tiktok = count;
            }
            break;
        }

        case APP_MODE_MASTODON: {
            const sub_count_t *s = mastodon_get();
            bool uncfg = (cfg->mastodon_user[0] == '\0' ||
                          cfg->mastodon_instance[0] == '\0');
            uint32_t count = (!uncfg && s->valid) ? (uint32_t)s->subscriber_count : 0;
            if (first || mode_changed || theme_changed || count != last_mastodon ||
                    burnin_force_render) {
                if (!wl_anim_tick || first || mode_changed) {
                    if (uncfg) render_followers_blank(cfg, "mastodon");
                    else        render_followers(cfg, count, "mastodon");
                }
                last_mastodon = count;
            }
            break;
        }


        case APP_MODE_SPECTRUM:
            /* Audio changes every frame — always re-render at the display task rate. */
            render_spectrum(cfg);
            break;

        case APP_MODE_ALBUM:
            render_album(cfg, &album_switch,
                         first || mode_changed || theme_changed || burnin_force_render);
            break;

        case APP_MODE_WEATHER: {
            struct tm wx_tm; ntp_get_local(&wx_tm);
            const weather_data_t *w = weather_get();
            bool now_valid  = (w != NULL && w->valid);

            /* Panel auto-switch: cycle enabled panels on a timer.
             * Respects weather_panel0/1/2/3/4_en — panels not enabled are skipped.
             * Panel 4 (HILO) has an internal HI→LO sub-rotation: the first timer
             * fire flips hilo_phase 0→1; the second fire advances to the next panel.
             * If the currently active panel has been disabled, jumps to the next
             * enabled one immediately without waiting for the rotation timer.   */
            bool panel_flipped = false;
            {
                /* Build ordered list of enabled panel indices — always, not
                 * just when now_valid, so a disabled panel is corrected before
                 * the first weather fetch completes (fixes panel 0 showing
                 * persistently when only another panel is enabled). */
                bool pen[5] = { cfg->weather_panel0_en,
                                cfg->weather_panel1_en,
                                cfg->weather_panel2_en,
                                cfg->weather_panel3_en,
                                cfg->weather_panel4_en };
                int elist[5]; int ecnt = 0;
                for (int _i = 0; _i < 5; _i++)
                    if (pen[_i]) elist[ecnt++] = _i;
                if (ecnt == 0) {
                    /* Stale/corrupt config — enable all panels as fallback */
                    for (int _i = 0; _i < 5; _i++) elist[ecnt++] = _i;
                }

                /* If current panel was disabled, jump to first enabled */
                bool cur_ok = false;
                for (int _i = 0; _i < ecnt; _i++)
                    if (elist[_i] == weather_panel) { cur_ok = true; break; }
                if (!cur_ok) {
                    weather_panel = elist[0]; weather_panel_tick = 0;
                    hilo_phase = 0; panel_flipped = true;
                } else if (now_valid && (ecnt > 1 || weather_panel == WEATHER_PANEL_HILO)) {
                    /* Timer-based rotation — only when data is valid */
                    TickType_t now_t = xTaskGetTickCount();
                    if (weather_panel_tick == 0) {
                        weather_panel_tick = now_t;
                    } else if ((now_t - weather_panel_tick) >= pdMS_TO_TICKS(
                                   cfg->weather_panel_ms ? cfg->weather_panel_ms : 5000)) {
                        if (weather_panel == WEATHER_PANEL_HILO && hilo_phase == 0) {
                            /* HI shown — flip to LO before moving on */
                            hilo_phase         = 1;
                            weather_panel_tick = now_t;
                            panel_flipped      = true;
                        } else {
                            /* Advance to next main panel */
                            hilo_phase = 0;
                            int cur_pos = 0;
                            for (int _i = 0; _i < ecnt; _i++)
                                if (elist[_i] == weather_panel) { cur_pos = _i; break; }
                            weather_panel      = elist[(cur_pos + 1) % ecnt];
                            weather_panel_tick = now_t;
                            panel_flipped      = true;
                        }
                    }
                }
            }

            /* True when the animated sunrise/sunset panel is visible — drives
             * FAST tick (20 Hz) and per-frame animation blit of tubes 0 and 4. */
            sun_anim = (weather_panel == WEATHER_PANEL_SUN);  /* FAST tick whenever sun anim is on screen, regardless of panel2_en */

            /* Trigger re-render when: first draw, mode/theme change, new data
             * values arrived, validity flips, panel switched, or solar-time
             * minute changed (only relevant on panel 2). */
            bool wx_changed = false;
            if (now_valid && last_wx_valid) {
                bool fahrenheit = (strncmp(cfg->temp_format, "Fahrenheit", 10) == 0);
                temp_val_t cur  = temp_sign_magnitude(w->temp_c,   fahrenheit);
                temp_val_t last = temp_sign_magnitude(last_temp_c, fahrenheit);
                int  cur_t_i  = cur.value,  last_t_i  = last.value;
                bool cur_neg  = cur.negative, last_neg = last.negative;
                wx_changed = (cur_t_i != last_t_i || cur_neg != last_neg ||
                              (int)(w->humidity + 0.5f) != (int)(last_hum + 0.5f));
                if (!wx_changed && weather_panel == WEATHER_PANEL_SUN)
                    wx_changed = (wx_tm.tm_min != last_wx_min);
            }
            bool valid_changed = (now_valid != last_wx_valid);
            bool full_redraw   = (first || mode_changed || theme_changed || wx_changed ||
                                  valid_changed || panel_flipped || burnin_force_render);
            if (full_redraw || (sun_anim && !wl_anim_tick)) {
                /* wx_sun_anim_frame advances a static position counter exactly
                 * once per rendered frame (on the rising=true tube-0 call).
                 * The pre-switch loop already rendered this tick when
                 * wl_anim_tick is true and !first && !mode_changed, so skip
                 * the render here to avoid a double-advance and sun jump. */
                if (!wl_anim_tick || first || mode_changed) {
                    render_weather(cfg, weather_panel, /*anim_only=*/!full_redraw, hilo_phase);
                }
                /* Always update change-detection state so a skipped-render
                 * tick doesn't mask a data change on the next cycle. */
                if (full_redraw) {
                    if (now_valid) {
                        last_temp_c = w->temp_c;
                        last_hum    = w->humidity;
                        last_wx_min = wx_tm.tm_min;
                    }
                    last_wx_valid = now_valid;
                }
            }
            break;
        }

        default: break;
        }

        last_mode = mode;
        strncpy(last_theme,      cfg->theme,      sizeof(last_theme)      - 1);
        last_theme[sizeof(last_theme) - 1]           = '\0';
        strncpy(last_clock_face, cfg->clock_face, sizeof(last_clock_face) - 1);
        last_clock_face[sizeof(last_clock_face) - 1] = '\0';
        /* Advance the custom-face change-detection cursor only when the
         * WeatherLive / Custom render actually executed.  The busy hint raised
         * by a config save (display_busy_hint) makes render_weatherlive return
         * early without painting — if we advanced the cursor here anyway,
         * custom_changed would clear before the render ran and the
         * shadow/colour update would be lost until the next mode change. */
        bool wl_clock_was_busy =
            (mode == APP_MODE_CLOCK) &&
            ((strncmp(cfg->theme, "WeatherLive", 11) == 0) ||
             (strcmp(cfg->clock_face, "custom") == 0)) &&
            (esp_timer_get_time() < s_busy_until_us || s_park_req);
        if (!wl_clock_was_busy) {
            strncpy(last_custom_bg,  cfg->custom_bg,  sizeof(last_custom_bg)  - 1);
            last_custom_bg[sizeof(last_custom_bg) - 1]  = '\0';
            memcpy(last_custom_glyph_color,  cfg->custom_glyph_color,  3);
            memcpy(last_custom_font_color,   cfg->custom_font_color,   3);
            last_custom_shadow = cfg->custom_shadow;
            memcpy(last_custom_shadow_color, cfg->custom_shadow_color, 3);
            strncpy(last_custom_font, cfg->custom_font, sizeof(last_custom_font) - 1);
            last_custom_font[sizeof(last_custom_font) - 1] = '\0';
        }
        /* ── Anti burn-in: colour-cycle masked tubes ───────────────────────
         * Runs after normal mode render; unmasked tubes show live content.
         * Colour advances every BURNIN_COLOR_SECS seconds, cycling through
         * red→green→blue→white→black to stress every sub-pixel at both
         * voltage extremes.  Timer expiry is handled in the pre-check above. */
        if (s_burnin_mask) {
            time_t now_t = time(NULL);
            uint16_t col = s_burnin_colors[
                (size_t)(now_t / BURNIN_COLOR_SECS) % (size_t)BURNIN_COLOR_COUNT];
            for (int _t = 0; _t < LCD_COUNT; _t++) {
                if (s_burnin_mask & (1u << _t))
                    display_fill(_t, col);
            }
        }

        /* Static-snow burn-in: write random RGB565 pixels to masked tubes.
         * Runs independently of the colour-cycle above — both modes can be
         * active on different tube subsets simultaneously.
         * Timer expiry is handled in the pre-check above. */
        if (s_snow_mask) {
            for (int _t = 0; _t < LCD_COUNT; _t++) {
                if (s_snow_mask & (1u << _t))
                    display_fill_snow(_t);
            }
        }

        first = false;

        /* Spectrum and the Weather sun animation run at 20 Hz.
         * Spectrum matches the LED task refresh rate for snappy bar response.
         * The sun animation (Weather Panel 2) needs 20 Hz so the rising/setting
         * motion is smooth (1.5 px/frame × 20 Hz = 30 px/s).
         * All other modes use 5 Hz.
         *
         * FAST is used whenever weather mode is active AND panel 2 is enabled,
         * not only while the sun panel is currently displayed.  This eliminates
         * the "lag on panel switch" where the task was sleeping for a full SLOW
         * tick (200 ms) when the user switches from panel 0/1 to panel 2 — the
         * animation would not start until the next SLOW wake-up.  Keeping FAST
         * throughout weather mode means the panel switch is picked up within
         * 50 ms, giving an instant animation start.                            */
        bool weather_needs_fast = (mode == APP_MODE_WEATHER && cfg->weather_panel2_en);
        /* Static custom clockface with a non-WL background: the colon blink is
         * now a cheap diff-box push (<0.5 ms), so the full SLOW tick is no longer
         * needed — but 200 ms jitter makes the 1 Hz blink appear to stall or
         * double-blink.  Use the FAST tick so second transitions are caught within
         * 50 ms; only tube 2 is ever touched between full renders. */
        bool wl_colon_needs_fast = s_wl_is_custom &&
                                   s_wl_bg_theme[0] != '\0' &&
                                   strncmp(s_wl_bg_theme, "WeatherLive", 11) != 0;
        /* WeatherLive runs at the fast tick in realtime mode regardless of which
         * app mode is active — social-media, weather, etc. all animate the sky
         * while rotating.  Static mode stays on the slow tick. */
        bool weatherlive_anim = cfg->wlive_animate && wl_sky_animates;
        /* WeatherLive only needs the full 20 Hz when precipitation is active:
         * rain/snow particles move fast enough to step visibly at lower rates,
         * and thunderstorm lightning is mirrored to the LEDs.  With clear or
         * merely cloudy skies the only motion is slow star-twinkle and cloud
         * drift, which look identical at 10 Hz — so halve the frame rate and
         * return ~50% of this mode's CPU to httpd and the LED task, cutting
         * frame-overrun stutter.  Static (non-animated) WeatherLive stays slow. */
        bool wl_precip_active = weatherlive_anim && s_wl_scene_valid &&
                                s_wl_last_scene.precip;
        TickType_t tick_ms;
        if (mode == APP_MODE_SPECTRUM || sun_anim ||
                weather_needs_fast || wl_precip_active || wl_colon_needs_fast)
            tick_ms = pdMS_TO_TICKS(DISPLAY_TICK_MS_FAST);   /* 20 Hz */
        else if (weatherlive_anim)
            tick_ms = pdMS_TO_TICKS(DISPLAY_TICK_MS_MED);    /* 10 Hz */
        else
            tick_ms = pdMS_TO_TICKS(DISPLAY_TICK_MS_SLOW);   /* 5 Hz */

        /* Re-sync wake timer when we've fallen behind the current tick budget.
         *
         * Background: pixel blits use spi_device_polling_transmit() (CPU busy-
         * wait, no DMA ISR dependency).  In Spectrum mode (tick = 50 ms) a full
         * frame is 6 tubes × 160 row-transactions; the total render time can
         * occasionally exceed the tick budget.
         * When that happens vTaskDelayUntil's target is already in the past —
         * it returns immediately without sleeping, so IDLE1 on CPU 1 never runs
         * and the Task Watchdog fires after 5 s.
         *
         * The threshold must be tick_ms (the CURRENT tick budget), NOT the
         * hardcoded SLOW value.  Using SLOW (200 ms) as the threshold while
         * tick_ms = FAST (50 ms) means 30–40 consecutive no-sleep frames
         * accumulate before re-sync fires, starving IDLE1 for 2+ s per cycle.
         *
         * With tick_ms as threshold: any single render that overshoots its
         * budget causes an immediate re-sync, and the next vTaskDelayUntil
         * sleeps a full tick period.  IDLE1 always gets CPU within two ticks. */
        {
            TickType_t now_tick = xTaskGetTickCount();
            if ((TickType_t)(now_tick - wake) >= tick_ms)
                wake = now_tick;
        }
        vTaskDelayUntil(&wake, tick_ms);
    }
}

void display_task_start(void)
{
    xTaskCreatePinnedToCore(display_task, "display", DISPLAY_STACK_SIZE, NULL, 6,
                            &s_display_task_handle, 1);
    ESP_LOGI(TAG, "Display task started");
}

void display_busy_hint(uint32_t ms)
{
    int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    if (until > s_busy_until_us) s_busy_until_us = until;
}

void display_busy_clear(void)
{
    s_busy_until_us = 0;
}

void display_config_changed(void)
{
    s_busy_until_us = 0;      /* lift any pending busy backoff immediately */
    s_settings_saved = true;  /* force a mode_changed re-render next tick */
    if (s_display_task_handle)
        xTaskAbortDelay(s_display_task_handle);
}

void display_show_wait(void)
{
    /* COOPERATIVE park, not vTaskSuspend-from-outside: an asynchronous
     * suspend can land between spi_device_queue_trans() and its matching
     * get_trans_result inside spi_device_transmit() — the wait-screen draw
     * below (from the CALLER's task) then collects the display task's
     * orphaned transaction and trips the spi_master assert
     * "ret_trans == trans_desc" (observed during OTA with a clock render in
     * flight).  Instead, request a park and let the display task suspend
     * ITSELF at its loop boundary, where no SPI transaction can be open.
     * Flash operations always end in esp_restart(), so it never resumes. */
    if (s_display_task_handle) {
        s_park_req = true;
        /* Worst case: a full cold-cache clock render (6 JPEG decodes) is in
         * progress — allow several seconds before falling back. */
        for (int i = 0; i < 500 && !s_parked; i++)
            vTaskDelay(pdMS_TO_TICKS(10));
        if (!s_parked) {
            /* Task wedged mid-iteration — fall back to the hard suspend
             * (the pre-existing behaviour) rather than racing it forever. */
            ESP_LOGW(TAG, "display task did not park in 5 s — hard suspend");
            vTaskSuspend(s_display_task_handle);
        }
    }
    for (int i = 0; i < LCD_COUNT; i++) {
        display_show_image(i, "/images/system/wait.jpg");
    }
    ESP_LOGI(TAG, "Wait screen shown on all tubes — display task parked");
}
