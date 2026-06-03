#include "display.h"
#include "board_pins.h"
#include "u8g2.h"
#include "esp_log.h"
#include "esp_timer.h"          /* esp_timer_get_time — AP PIN phase clock */
#include "esp_heap_caps.h"      /* PSRAM_MALLOC / heap_caps_malloc */
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "weather.h"
#include "sht30.h"              /* sht30_get() — indoor H/T for 24H_CX panel */
#include "wifi_manager.h"       /* AP PIN visibility (S1) */
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
static uint8_t  s_cx_panel        = 0;   /* index into the enabled panel list */
static int64_t  s_cx_panel_start  = 0;   /* esp_timer_get_time() µs when panel began */
static struct tm s_cx_last_t;            /* last struct tm at which tube 6 was rendered */
static int8_t   s_cx_last_kind    = -1;  /* panel kind (0-3) last drawn; -1 = none rendered yet */

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
    io.pin_bit_mask = (1ULL << PIN_LCD_DC) | (1ULL << PIN_LCD_RST); gpio_config(&io);

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

    /* Hardware reset is shared — pulse RST once to reset all 6 displays,
     * then send the init sequence to each tube individually. */
    gpio_set_level(PIN_LCD_RST, 0); vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));
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

void display_invalidate(void)
{
    s_full_repaint_request = true;
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
        spi_device_transmit(spi_dev, &t);
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
        spi_transaction_t t = { .length = (size_t)(rows * w * 2) * 8, .tx_buffer = chunk };
        spi_device_transmit(spi_dev, &t);
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
            spi_device_transmit(spi_dev, &tr);
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
        spi_device_transmit(spi_dev, &t);
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
#define DISPLAY_TICK_MS_SLOW   200   /* all other modes — 5 Hz */

/* Weather panel indices — weather_panel local in display_task. */
#define WEATHER_PANEL_TEMP  0   /* temperature + icon */
#define WEATHER_PANEL_HUM   1   /* humidity */
#define WEATHER_PANEL_SUN   2   /* sunrise + sunset times */
/* Stack: config snapshot (~1900 B) + JPEG decode call chain (~3-4 KB).
 * 8 KB was too tight — panic handler couldn't print a backtrace. */
#define DISPLAY_STACK_SIZE   12288

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

void display_show_image(int tube, const char *path)
{
    if (tube < 0 || tube >= LCD_COUNT || !path) return;
    /* Skip any tube that is currently held by the colour-cycle or snow burn-in.
     * Both modes overwrite the tube in the display task after normal rendering
     * completes, so writing a JPEG here would be immediately discarded.
     * Skipping avoids the image-cache lookup, JPEG decode (on miss), and the
     * full ~8 ms SPI frame write — for no visible benefit on the masked tube. */
    if ((s_burnin_mask | s_snow_mask) & (1u << tube)) return;

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
            spi_device_transmit(spi_dev, &tr);
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
        spi_device_transmit(spi_dev, &tr);
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
void display_show_number(int tube, int digit, const char *theme)
{
    char p[256]; display_path_number(p, sizeof(p), theme, digit);
    display_show_image(tube, p);
}

void display_show_ampm(int tube, const char *name, const char *theme)
{
    char p[256]; display_path_ampm(p, sizeof(p), theme, name);
    /* Fall back to /images/system/{name}.jpg when the theme-specific asset is
     * absent (e.g. a custom theme that predates the Instagram/TikTok icons).
     * img_cache_get returns NULL without touching the SPI bus when the file
     * cannot be decoded; the system path is then substituted so
     * display_show_image gets a path that resolves to actual pixels. */
    {
        int _w = 0, _h = 0;
        if (!img_cache_get(p, &_w, &_h))
            snprintf(p, sizeof(p), "/images/system/%s.jpg", name);
    }
    display_show_image(tube, p);
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
        spi_device_transmit(spi_dev, &t);
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
        spi_device_transmit(spi_dev, &t);
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
        spi_device_transmit(spi_dev, &t);
    }

    /* ── Bottom black margin (16 rows) ── */
    memset(chunk, 0, sizeof(chunk));
    int bot = LCD_HEIGHT - MARGIN - OUT_H;   /* 160 - 16 - 128 = 16 rows */
    for (int r = 0; r < bot; r += DISP_CHUNK_ROWS) {
        int rows = (r + DISP_CHUNK_ROWS <= bot) ? DISP_CHUNK_ROWS : bot - r;
        spi_transaction_t t = { .length = (size_t)(rows * LCD_WIDTH * 2) * 8,
                                 .tx_buffer = chunk };
        spi_device_transmit(spi_dev, &t);
    }

    deselect_all();
}

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
    int d  = t->tm_mday;        /* 1-31  */
    int mo = t->tm_mon + 1;     /* 1-12  */
    int y  = t->tm_year % 100;  /* 0-99 (last two digits of year) */
    int digits[6];
    if (strcmp(cfg->date_format, "MM/DD/YY") == 0) {
        /* US format: month first */
        digits[0] = mo/10; digits[1] = mo%10;
        digits[2] = d/10;  digits[3] = d%10;
    } else {
        /* European format (default): day first */
        digits[0] = d/10;  digits[1] = d%10;
        digits[2] = mo/10; digits[3] = mo%10;
    }
    digits[4] = y/10; digits[5] = y%10;
    for (int i = 0; i < 6; i++)
        display_show_number(i, digits[i], cfg->theme);
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
        spi_device_transmit(spi_dev, &t);
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
        spi_device_transmit(spi_dev, &t);
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
static void ht_draw_label(const char *str, int y_tube, uint16_t fg,
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
    ht_blit_at(5, u8g2_GetBufferPtr(&s_u8g2), blit_h, y_tube, fg, bg);
}

/* Render a UTF-8 string centred horizontally (within LCD_WIDTH=80 px) and
 * vertically within a band of `height` rows placed at absolute tube row y_tube.
 * font: pointer to any compiled-in U8g2 font constant.
 * Blits at most 64 rows (the U8g2 buffer height limit).
 * bg: optional RGB565 background buffer (see ht_blit_at); NULL = solid black.*/
static void ht_draw_str_at(const char *str, int y_tube, int height,
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
    ht_blit_at(5, u8g2_GetBufferPtr(&s_u8g2), blit_h, y_tube, fg, bg);
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
static void ht_draw_suntime(const char *timestr, bool rising,
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
    ht_blit_at(5, u8g2_GetBufferPtr(&s_u8g2), 56, y_tube, fg, bg);
}

/* ── cx6_stamp_update_indicator ──────────────────────────────────────────────
 * Panels rendered via ht_blit_at (weekdate, H/T, sunrise/sunset) bypass
 * display_show_digit(), so they never trigger the 4-row red stripe that
 * display_show_digit() applies automatically.  Worse, the H/T humidity blit
 * and the weekdate date blit both extend to row 159, overwriting whatever
 * display_show_image() had drawn there.
 * Call this once after ALL blits for tube 5 are complete to re-stamp the
 * indicator when s_update_indicator is active.  No-op when inactive.          */
static void cx6_stamp_update_indicator(void)
{
    if (!s_update_indicator) return;
    const int tube = LCD_COUNT - 1;
    select_tube(tube);
    uint8_t ox = (uint8_t)((int)LCD_OFFSET_X + (int)s_burnin_shift_x
                            + (int)s_col_offsets[tube]);
    uint8_t oy = (uint8_t)((int)LCD_OFFSET_Y + (int)s_row_offsets[tube]);
    open_lcd_window(ox, (uint8_t)(oy + LCD_HEIGHT - 4), (uint8_t)LCD_WIDTH, 4);
    uint8_t redline[LCD_WIDTH * 2];
    for (int x = 0; x < LCD_WIDTH; x++) { redline[x*2] = 0xF8; redline[x*2+1] = 0x00; }
    for (int row = 0; row < 4; row++) {
        spi_transaction_t tr = { .length = sizeof(redline) * 8, .tx_buffer = redline };
        spi_device_transmit(spi_dev, &tr);
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
 * panel_id is an index into the ordered list [weather, weekdate, ht, temp, sunrise];
 * the caller resolves which concrete panel this maps to.                       */
static void render_cx_tube6(const nextube_config_t *cfg, const struct tm *t,
                             uint8_t panel_id)
{
    /* Resolve panel_id → concrete panel type.
     * Order: 0=weather, 1=weekdate, 2=indoor H/T, 3=outdoor H/T, 4=sunrise+sunset
     * We iterate through the ordered list and pick the panel_id-th enabled entry. */
    const bool enabled[5] = {
        cfg->tube6_panel_weather,
        cfg->tube6_panel_weekdate,
        cfg->tube6_panel_ht,
        cfg->tube6_panel_temp,
        cfg->tube6_panel_sunrise,
    };
    int kind = -1;   /* 0=weather icon, 1=weekdate, 2=indoor H/T, 3=outdoor H/T, 4=sunrise+sunset */
    int count = 0;
    for (int i = 0; i < 5; i++) {
        if (enabled[i]) {
            if (count == (int)panel_id) { kind = i; break; }
            count++;
        }
    }
    if (kind < 0) kind = 1;   /* fallback: weekdate */

    const int HALF = LCD_HEIGHT / 2;   /* 80 */

    if (kind == 0) {
        /* ── Weather icon panel ─────────────────────────────────────────────
         * Displays the current weather condition icon full-tube (80×160).
         * Falls back to black when the weather API has no valid data.       */
        const weather_data_t *w = weather_get();
        if (!w || !w->valid) {
            if (kind != s_cx_last_kind) display_fill(5, 0x0000);
            goto cx_tube6_done;
        }
        {
            const char *icon = (w->icon[0] != '\0') ? w->icon : "sun";
            char path[256];
            display_path_weather(path, sizeof(path), cfg->theme, icon);
            display_show_image(5, path);
        }

    } else if (kind == 1) {
        /* ── Week/Date panel — U8g2 text over blank.jpg background ──────────
         * Top half    (rows   0– 79) : day name — "Sun" … "Sat"
         *                             U8g2 logisoso28, centred in 64-row band
         *                             at tube rows 16–79
         * Bottom half (rows  80–159) : date "MM/DD"
         *                             U8g2 logisoso28, centred in 64-row band
         *                             at tube rows 96–159
         *
         * Background: theme's AMPM/blank.jpg decoded via image cache.  The
         * 8-px fringes at the top/bottom of each half are preserved from the
         * full-tube display_show_image() call made before the text blits, so
         * the entire 80×160 surface shows the theme background.
         * Text colour: auto-sampled from Numbers/0.jpg centre pixel.
         * Fallback: if blank.jpg is absent or wrong size, solid black fill.  */

        char bg_path[256];
        snprintf(bg_path, sizeof(bg_path),
                 "/images/themes/%s/AMPM/blank.jpg", cfg->theme);
        int bg_w = 0, bg_h = 0;
        const uint8_t *bg = img_cache_get(bg_path, &bg_w, &bg_h);
        /* Only use bg for compositing when dimensions match the tube exactly. */
        if (bg_w != LCD_WIDTH || bg_h != LCD_HEIGHT) bg = NULL;

        if (bg) {
            /* Write the full background so rows not covered by ht_draw_str_at
             * (the 8-px fringes) show the theme image rather than stale pixels. */
            display_show_image(5, bg_path);
        } else {
            /* No valid background — clear to black on panel switch only. */
            if (kind != s_cx_last_kind) display_fill(5, 0x0000);
        }

        uint16_t fg = ht_sample_theme_color(cfg->theme);

        /* Day name — top half, centred in 64-row U8g2 band (rows 8–71).
         * Localised per cfg->language (tube display language setting). */
        const char *day = weekday_abbrev(cfg->language, t->tm_wday);
        ht_draw_str_at(day, 16, 64, u8g2_font_logisoso28_tf, fg, bg);

        /* Date — bottom half, respects Network › Date format setting.
         * "MM/DD/YY": month first → MMDD   (US format)
         * "DD/MM/YY": day first   → DDMM   (international default) */
        {
            char buf[16];
            bool us_fmt = (strcmp(cfg->date_format, "MM/DD/YY") == 0);
            int  mo = t->tm_mon + 1;
            if (us_fmt)
                snprintf(buf, sizeof(buf), "%02d%02d", mo, t->tm_mday);
            else
                snprintf(buf, sizeof(buf), "%02d%02d", t->tm_mday, mo);
            ht_draw_str_at(buf, HALF + 16, 64, u8g2_font_logisoso28_tf, fg, bg);
        }

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
            if (kind != s_cx_last_kind) display_fill(5, 0x0000);
            goto cx_tube6_done;
        }

        /* Background: theme's AMPM/blank.jpg — same approach as weekdate panel. */
        {
            char bg_path[256];
            snprintf(bg_path, sizeof(bg_path),
                     "/images/themes/%s/AMPM/blank.jpg", cfg->theme);
            int bg_w = 0, bg_h = 0;
            const uint8_t *bg = img_cache_get(bg_path, &bg_w, &bg_h);
            if (bg_w != LCD_WIDTH || bg_h != LCD_HEIGHT) bg = NULL;
            if (bg) display_show_image(5, bg_path);
            else    display_fill(5, 0x0000);

            uint16_t fg = ht_sample_theme_color(cfg->theme);

            /* "In" label — rows 15–38 (HT_LABEL_H=24, shifted +15) */
            ht_draw_label(inout_label(cfg->language, true), 18, fg, bg);

            /* Indoor temperature — rows 39–94 (56-row band, logisoso28, shifted +15) */
            {
                bool  use_f = (strcmp(cfg->temp_format, "Fahrenheit") == 0);
                float ftemp = use_f ? (s->temp_c * 9.0f / 5.0f + 32.0f) : s->temp_c;
                int   temp  = (int)(ftemp + (ftemp >= 0.0f ? 0.5f : -0.5f));
                if (temp >  99) temp =  99;
                if (temp < -99) temp = -99;
                char buf[16];
                /* UTF-8 degree symbol U+00B0 = 0xC2 0xB0 (supported by _tf fonts) */
                snprintf(buf, sizeof(buf), "%d\xc2\xb0%s", temp, use_f ? "F" : "C");
                ht_draw_str_at(buf, HT_LABEL_H + 18, 56, u8g2_font_logisoso28_tf, fg, bg);
            }

            /* Indoor humidity — rows 95–158 (64-row blit centred in 80-px half,
             * shifted +15; rows 159 remain from the background image above). */
            {
                int hum = (int)(s->humidity + 0.5f);
                if (hum > 99) hum = 99;
                if (hum <  0) hum = 0;
                char buf[8];
                snprintf(buf, sizeof(buf), "%d%%", hum);
                ht_draw_str_at(buf, HALF + 18, 64, u8g2_font_logisoso28_tf, fg, bg);
            }
        }

    } else if (kind == 3) {
        /* ── Outdoor H/T panel — mirrors indoor layout with weather data ──── */
        /* Rows  10– 33 : "Out" label  (logisoso20, HT_LABEL_H=24 rows, +10 shift)
         * Rows  34– 89 : outdoor temperature  (logisoso28, 56-row band)
         * Rows  90–153 : outdoor humidity     (logisoso28, centred in 64-px blit)
         * Colour auto-sampled from the theme's Numbers/0.jpg centre pixel.
         * Falls back to black when weather API has no valid data.            */
        const weather_data_t *ow = weather_get();
        if (!ow || !ow->valid) {
            if (kind != s_cx_last_kind) display_fill(5, 0x0000);
            goto cx_tube6_done;
        }

        /* Background: theme's AMPM/blank.jpg — same approach as other H/T panels. */
        {
            char bg_path[256];
            snprintf(bg_path, sizeof(bg_path),
                     "/images/themes/%s/AMPM/blank.jpg", cfg->theme);
            int bg_w = 0, bg_h = 0;
            const uint8_t *bg = img_cache_get(bg_path, &bg_w, &bg_h);
            if (bg_w != LCD_WIDTH || bg_h != LCD_HEIGHT) bg = NULL;
            if (bg) display_show_image(5, bg_path);
            else    display_fill(5, 0x0000);

            uint16_t fg = ht_sample_theme_color(cfg->theme);

            /* "Out" label — rows 15–38 (HT_LABEL_H=24, shifted +15) */
            ht_draw_label(inout_label(cfg->language, false), 18, fg, bg);

            /* Outdoor temperature — rows 39–94 (56-row band, shifted +15) */
            {
                bool  use_f = (strcmp(cfg->temp_format, "Fahrenheit") == 0);
                float ftemp = use_f ? (ow->temp_c * 9.0f / 5.0f + 32.0f) : ow->temp_c;
                int   temp  = (int)(ftemp + (ftemp >= 0.0f ? 0.5f : -0.5f));
                if (temp >  99) temp =  99;
                if (temp < -99) temp = -99;
                char buf[16];
                snprintf(buf, sizeof(buf), "%d\xc2\xb0%s", temp, use_f ? "F" : "C");
                ht_draw_str_at(buf, HT_LABEL_H + 18, 56, u8g2_font_logisoso28_tf, fg, bg);
            }

            /* Outdoor humidity — rows 95–158 (64-row blit centred in 80-px half,
             * shifted +15; rows 159 remain from the background image above). */
            {
                int hum = (int)(ow->humidity + 0.5f);
                if (hum > 99) hum = 99;
                if (hum <  0) hum = 0;
                char buf[8];
                snprintf(buf, sizeof(buf), "%d%%", hum);
                ht_draw_str_at(buf, HALF + 18, 64, u8g2_font_logisoso28_tf, fg, bg);
            }
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
            char bg_path[256];
            snprintf(bg_path, sizeof(bg_path),
                     "/images/themes/%s/AMPM/blank.jpg", cfg->theme);
            int bg_w = 0, bg_h = 0;
            const uint8_t *bg = img_cache_get(bg_path, &bg_w, &bg_h);
            if (bg_w != LCD_WIDTH || bg_h != LCD_HEIGHT) bg = NULL;
            if (bg) display_show_image(5, bg_path);
            else    display_fill(5, 0x0000);

            uint16_t fg = ht_sample_theme_color(cfg->theme);

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
            ht_draw_suntime(rise_str, /*rising=*/true,  14,        fg, bg);
            ht_draw_suntime(set_str,  /*rising=*/false, HALF + 14, fg, bg);
        }
    }

    s_cx_last_kind = (int8_t)kind;   /* record which panel was just drawn */

cx_tube6_done:
    /* Re-stamp the update indicator after every tube-6 panel render.
     * ht_blit_at paths overwrite the bottom rows; display_fill misses it
     * entirely.  cx6_stamp_update_indicator() is a no-op when inactive. */
    cx6_stamp_update_indicator();
}

static void render_clock(const nextube_config_t *cfg, const struct tm *t)
{
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
        /* 24H no-seconds / 24H Custom: H1  H2  colon  M1  M2  [tube5]
         * For 24H_NS, tube5 is user-configurable: "blank" or "weather".
         * For 24H_CX, tube5 is rendered separately by render_cx_tube6()
         * so render_clock() leaves it alone. */
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
        /* is_24cx: tube 5 handled by render_cx_tube6() — do nothing here */
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

static void render_number6(uint32_t value, const char *theme,
                           const char *icon_tube0, const char *suffix_tube5)
{
    /* Tube 0: mode icon  |  tubes 1-4: digits  |  tube 5: suffix/blank */
    if (icon_tube0)
        display_show_ampm(0, icon_tube0, theme);
    else {
        uint8_t d0 = (value / 100000) % 10;
        display_show_number(0, d0, theme);
    }

    /* Tubes 1-4: suppress leading zeros with the theme blank image so
     * small counts don't show a row of "0" tiles before the real digits. */
    static const uint32_t div4[4] = { 10000, 1000, 100, 10 };
    bool leading = true;
    for (int i = 0; i < 4; i++) {
        uint8_t d = (value / div4[i]) % 10;
        if (leading && d == 0)
            display_show_ampm(i + 1, "blank", theme);
        else {
            display_show_number(i + 1, d, theme);
            leading = false;
        }
    }

    /* Tube 5: suffix symbol or units digit — always shown, never blanked */
    if (suffix_tube5)
        display_show_ampm(5, suffix_tube5, theme);
    else
        display_show_number(5, value % 10, theme);
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
    display_show_ampm(0, icon, cfg->theme);
    for (int t = 1; t <= 5; t++)
        display_show_ampm(t, "blank", cfg->theme);
}

static void render_followers(const nextube_config_t *cfg,
                             uint32_t count, const char *icon)
{
    if (count >= 1000000) {
        uint32_t int_m = count / 1000000;
        if (int_m < 10) {
            uint32_t dec1 = (count % 1000000) / 100000;
            uint32_t dec2 = (count % 100000)  / 10000;
            display_show_ampm(0, icon, cfg->theme);
            if (dec2) {
                display_show_number(1, (uint8_t)int_m, cfg->theme);
                display_show_ampm  (2, "dot",          cfg->theme);
                display_show_number(3, (uint8_t)dec1,  cfg->theme);
                display_show_number(4, (uint8_t)dec2,  cfg->theme);
            } else if (dec1) {
                display_show_ampm  (1, "blank",        cfg->theme);
                display_show_number(2, (uint8_t)int_m, cfg->theme);
                display_show_ampm  (3, "dot",          cfg->theme);
                display_show_number(4, (uint8_t)dec1,  cfg->theme);
            } else {
                display_show_ampm  (1, "blank",        cfg->theme);
                display_show_ampm  (2, "blank",        cfg->theme);
                display_show_ampm  (3, "blank",        cfg->theme);
                display_show_number(4, (uint8_t)int_m, cfg->theme);
            }
            display_show_ampm(5, "m-sub", cfg->theme);
        } else {
            render_number6(count / 100000, cfg->theme, icon, "m-sub");
        }
    } else if (count >= 1000) {
        uint32_t int_k = count / 1000;
        if (int_k < 10) {
            uint32_t dec1 = (count % 1000) / 100;
            uint32_t dec2 = (count % 100)  / 10;
            display_show_ampm(0, icon, cfg->theme);
            if (dec2) {
                display_show_number(1, (uint8_t)int_k, cfg->theme);
                display_show_ampm  (2, "dot",          cfg->theme);
                display_show_number(3, (uint8_t)dec1,  cfg->theme);
                display_show_number(4, (uint8_t)dec2,  cfg->theme);
            } else if (dec1) {
                display_show_ampm  (1, "blank",        cfg->theme);
                display_show_number(2, (uint8_t)int_k, cfg->theme);
                display_show_ampm  (3, "dot",          cfg->theme);
                display_show_number(4, (uint8_t)dec1,  cfg->theme);
            } else {
                display_show_ampm  (1, "blank",        cfg->theme);
                display_show_ampm  (2, "blank",        cfg->theme);
                display_show_ampm  (3, "blank",        cfg->theme);
                display_show_number(4, (uint8_t)int_k, cfg->theme);
            }
            display_show_ampm(5, "k-sub", cfg->theme);
        } else {
            render_number6(count / 100, cfg->theme, icon, "k-sub");
        }
    } else {
        /* Raw count < 1 K — use all five digit tubes, suppress leading zeros */
        display_show_ampm(0, icon, cfg->theme);
        static const uint32_t div5[5] = { 10000, 1000, 100, 10, 1 };
        bool leading = true;
        for (int i = 0; i < 5; i++) {
            uint8_t d = (count / div5[i]) % 10;
            if (leading && d == 0 && i < 4)
                display_show_ampm(i + 1, "blank", cfg->theme);
            else {
                display_show_number(i + 1, d, cfg->theme);
                leading = false;
            }
        }
    }
}

static void render_countdown_display(const nextube_config_t *cfg,
                                     int32_t remaining_s)
{
    if (remaining_s < 0) remaining_s = 0;
    int m = remaining_s / 60, s = remaining_s % 60;
    display_show_ampm(0, "countdown", cfg->theme);
    display_show_number(1, m / 10,  cfg->theme);
    display_show_number(2, m % 10,  cfg->theme);
    display_show_ampm  (3, "colon", cfg->theme);
    display_show_number(4, s / 10,  cfg->theme);
    display_show_number(5, s % 10,  cfg->theme);
}

static void render_pomodoro_display(const nextube_config_t *cfg,
                                    int32_t remaining_s, bool in_break)
{
    if (remaining_s < 0) remaining_s = 0;
    int m = remaining_s / 60, s = remaining_s % 60;
    display_show_ampm(0, "pomodoro", cfg->theme);
    display_show_number(1, m / 10, cfg->theme);
    display_show_number(2, m % 10, cfg->theme);
    display_show_ampm  (3, "colon", cfg->theme);
    display_show_number(4, s / 10, cfg->theme);
    display_show_ampm  (5, in_break ? "pomodorolb" : "pomodorosb", cfg->theme);
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

    const uint8_t br = cfg->spectrum_lcd_rgb[0];
    const uint8_t bg = cfg->spectrum_lcd_rgb[1];
    const uint8_t bb = cfg->spectrum_lcd_rgb[2];

    /* Update peak-hold and precompute lit segment count + peak dot for every band. */
    int  lit_count[LCD_COUNT * SPEC_BARS_PER_TUBE];
    int  peak_dot [LCD_COUNT * SPEC_BARS_PER_TUBE];
    bool peak_vis [LCD_COUNT * SPEC_BARS_PER_TUBE];
    for (int i = 0; i < LCD_COUNT * SPEC_BARS_PER_TUBE; i++) {
        float e = bands[i];
        if (e < 0.0f) e = 0.0f; else if (e > 1.0f) e = 1.0f;
        if (e >= s_spec_peak[i]) {
            s_spec_peak[i] = e;
        } else {
            s_spec_peak[i] -= 0.05f;
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
            spi_device_transmit(spi_dev, &t);
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
    static float s_pos = 0.0f;   /* 0..110: travel distance from start   */
    static int   s_ph  = 0;      /* 0 = moving, 1 = holding at end pos   */
    static int   s_cnt = 0;

    if (rising) {                /* advance exactly once per render frame */
        if (s_ph == 0) {
            s_pos += 1.5f;
            if (s_pos >= 110.0f) { s_pos = 110.0f; s_ph = 1; s_cnt = 0; }
        } else {
            if (++s_cnt >= 60) { s_ph = 0; s_pos = 0.0f; }
        }
    }

    const int HY = 110;      /* tube-absolute Y of horizon line */
    const int SR = 10;       /* sun disc radius                 */
    /* Derive this tube's sun position from the shared travel distance */
    float sy = rising ? (150.0f - s_pos) : (40.0f + s_pos);

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
            for (int i = 0; i < 7; i++) {
                float ang = (float)M_PI + (float)i * ((float)M_PI / 6.0f);
                float ca  = cosf(ang), sa = sinf(ang);
                int rx0 = 40   + (int)(ca * (float)(SR + 3));
                int ry0 = bSun + (int)(sa * (float)(SR + 3));
                int rx1 = 40   + (int)(ca * (float)(SR + 11));
                int ry1 = bSun + (int)(sa * (float)(SR + 11));
                /* Skip rays with any endpoint outside valid buffer rows.
                 * Negative y → cast to large uint16_t → DrawLine artifact. */
                if (ry0 < 0 || ry1 < 0 || ry0 >= nrow || ry1 >= nrow) continue;
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

/* ── wx_sun_draw_time ────────────────────────────────────────────────────────
 * Render "HH:MM" time centred both axes in a full 80×160 tube (logisoso28).
 * bg: decoded RGB565 background (LCD_WIDTH × LCD_HEIGHT); NULL = solid black. */
static void wx_sun_draw_time(int tube, const char *timestr, uint16_t fg,
                              const uint8_t *bg)
{
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFont(&s_u8g2, u8g2_font_logisoso28_tf);
    int ascent  = (int)u8g2_GetAscent(&s_u8g2);
    int descent = (int)u8g2_GetDescent(&s_u8g2);   /* negative */
    int glyph_h = ascent - descent;                  /* ~32 rows */
    u8g2_uint_t tw = u8g2_GetStrWidth(&s_u8g2, timestr);
    int tx = ((int)LCD_WIDTH - (int)tw) / 2;
    if (tx < 0) tx = 0;
    u8g2_DrawStr(&s_u8g2, (u8g2_uint_t)tx, (u8g2_uint_t)ascent, timestr);

    int y_tube = (LCD_HEIGHT - glyph_h) / 2;
    ht_blit_at(tube, u8g2_GetBufferPtr(&s_u8g2), glyph_h, y_tube, fg, bg);
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
static void render_weather_sun(const nextube_config_t *cfg, const struct tm *t,
                                bool anim_only)
{
    uint16_t fg = ht_sample_theme_color(cfg->theme);

    /* Load the theme background once — needed by both static and animation tubes.
     * img_cache_get is a fast cache lookup (no disk I/O on cache hits), so it is
     * cheap to call on every animation tick.                                      */
    char bg_path[256];
    snprintf(bg_path, sizeof(bg_path), "/images/themes/%s/AMPM/blank.jpg",
             cfg->theme);
    int bg_w = 0, bg_h = 0;
    const uint8_t *bg = img_cache_get(bg_path, &bg_w, &bg_h);
    if (bg_w != LCD_WIDTH || bg_h != LCD_HEIGHT) bg = NULL;

    if (!anim_only) {
        /* ── Static tubes: time strings + blank tubes ──
         * Animation tubes (0 and 4) are fully rendered by wx_sun_anim_frame()
         * on every tick; their background fill is handled there.               */
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

        if (bg) {
            display_show_image(1, bg_path);
            display_show_image(5, bg_path);
        } else {
            display_fill(1, 0x0000);
            display_fill(5, 0x0000);
        }
        wx_sun_draw_time(1, rise_str, fg, bg);
        display_show_ampm(2, "blank", cfg->theme);
        display_show_ampm(3, "blank", cfg->theme);
        wx_sun_draw_time(5, set_str,  fg, bg);
    }

    /* ── Animation tubes — rendered on every frame regardless of anim_only ── */
    wx_sun_anim_frame(0, /*rising=*/true,  fg, bg);
    wx_sun_anim_frame(4, /*rising=*/false, fg, bg);
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
 *    [blank] [blank] [blank] [hum_tens/blank] [hum_units] [icon]
 *
 *  Panel 2 — sunrise + sunset:
 *    [rise_icon] [rise_time] [blank] [blank] [set_icon] [set_time]
 */
static void render_weather(const nextube_config_t *cfg, int panel, bool anim_only)
{
    const weather_data_t *w = weather_get();
    char path[128];

    if (!w || !w->valid) {
        /* No weather data yet – show "·" (dot) on every tube. */
        for (int i = 0; i < LCD_COUNT; i++)
            display_show_ampm(i, "dot", cfg->theme);
        return;
    }

    /* Panel 2 — sunrise/sunset — handled by a dedicated renderer */
    if (panel == WEATHER_PANEL_SUN) {
        /* render_weather_sun needs local time — derive from time() here.
         * anim_only=true: only advance and blit the animation tubes (0 and 4).
         * anim_only=false: full redraw of all 6 tubes.                        */
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        render_weather_sun(cfg, &lt, anim_only);
        return;
    }

    /* Temperature in the configured unit */
    bool fahrenheit = (strncmp(cfg->temp_format, "Fahrenheit", 10) == 0);
    float temp_f = fahrenheit ? w->temp_c * 9.0f / 5.0f + 32.0f : w->temp_c;
    bool negative = (temp_f < -0.5f);
    int  temp     = (int)(negative ? -temp_f + 0.5f : temp_f + 0.5f);
    if (temp > 99) temp = 99;

    int hum = (int)(w->humidity + 0.5f);
    if (hum < 0)  hum = 0;
    if (hum > 99) hum = 99;

    const char *unit = fahrenheit ? "degreef" : "degreec";
    const char *icon = (w->icon[0] != '\0') ? w->icon : "sun";

    /* Tube 5 (weather icon) is the same on both panels */
    display_path_weather(path, sizeof(path), cfg->theme, icon);
    display_show_image(5, path);

    /* ── Panel 1: humidity ─────────────────────────────────────────── */
    /* Layout: 0=blank  1=blank  2=tens/blank  3=units  4=%  5=icon */
    if (panel == 1) {
        display_show_ampm(0, "blank", cfg->theme);
        display_show_ampm(1, "blank", cfg->theme);

        /* Tube 2: tens digit of humidity (blank if < 10) */
        if (hum / 10 == 0) {
            display_show_ampm(2, "blank", cfg->theme);
        } else {
            display_path_number(path, sizeof(path), cfg->theme, hum / 10);
            display_show_image(2, path);
        }

        /* Tube 3: units digit of humidity */
        display_path_number(path, sizeof(path), cfg->theme, hum % 10);
        display_show_image(3, path);

        /* Tube 4: humidity % symbol (full 80×160 image) */
        display_path_humidity(path, sizeof(path), cfg->theme, "humidity");
        display_show_image(4, path);

        return;
    }

    /* ── Panel 0: temperature ──────────────────────────────────────── */
    /* Layout:
     *   positive 1-digit :  [blank] [blank] [blank] [units] [°C/°F] [icon]
     *   positive 2-digit :  [blank] [blank] [tens]  [units] [°C/°F] [icon]
     *   negative 1-digit :  [blank] [blank] [minus] [units] [°C/°F] [icon]
     *   negative 2-digit :  [blank] [minus] [tens]  [units] [°C/°F] [icon]
     *
     * minus.jpg, degreec.jpg, and degreef.jpg are full 80×160 images —
     * displayed directly without blending. */

    /* Prime flip animation cache — degree symbol is always on tube 4 */
    flip_prime_blank(4, cfg->theme);

    bool single_digit = (temp < 10);

    /* Tube 0: always blank */
    display_show_ampm(0, "blank", cfg->theme);

    /* Tube 1: minus (2-digit negative) or blank */
    if (negative && !single_digit) {
        display_path_temperature(path, sizeof(path), cfg->theme, "minus");
        display_show_image(1, path);
    } else {
        display_show_ampm(1, "blank", cfg->theme);
    }

    /* Tube 2: minus (1-digit negative), tens digit (2-digit), or blank */
    if (negative && single_digit) {
        display_path_temperature(path, sizeof(path), cfg->theme, "minus");
        display_show_image(2, path);
    } else if (!single_digit) {
        display_path_number(path, sizeof(path), cfg->theme, temp / 10);
        display_show_image(2, path);
    } else {
        display_show_ampm(2, "blank", cfg->theme);
    }

    /* Tube 3: units digit */
    display_path_number(path, sizeof(path), cfg->theme, temp % 10);
    display_show_image(3, path);

    /* Tube 4: °C / °F symbol (full 80×160 image) */
    display_path_temperature(path, sizeof(path), cfg->theme, unit);
    display_show_image(4, path);
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
        spi_device_transmit(spi_dev, &t);
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
    s_timer_mutex  = xSemaphoreCreateMutex();
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
    char          last_theme[32]     = {0};
    char          last_time_type[8]  = {0};
    uint32_t      last_subs     = UINT32_MAX;
    int32_t       last_remain_s = INT32_MAX;  /* countdown/pomodoro change detection */
    float         last_temp_c   = -9999.0f;   /* weather change detection */
    float         last_hum      = -1.0f;
    bool          last_wx_valid = false;       /* detect when data first arrives */
    int           last_wx_min   = -1;          /* solar time change detection (minute) */
    bool          last_leading_zero = false;    /* leading-zero change detection */
    bool          last_bl_on    = true;        /* backlight on/off tracking */
    uint8_t       last_bl_brt   = 255;         /* sentinel: force-apply on first tick */
    TickType_t    album_switch        = 0;
    TickType_t    rotation_tick       = 0;     /* tick when current mode started */
    TickType_t    theme_rotation_tick = 0;     /* tick when current theme started */
    bool          last_mode_rot_en    = false; /* mode-rotation enable edge tracker  */
    bool          last_theme_rot_en   = false; /* theme-rotation enable edge tracker */
    int           weather_panel      = WEATHER_PANEL_TEMP;
    TickType_t    weather_panel_tick = 0;      /* tick of last panel switch */
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
        config_lock();
        cfg_snap = *config_get();
        config_unlock();
        const nextube_config_t *cfg = &cfg_snap;
        app_mode_t mode = cfg->current_mode;
        bool mode_changed  = (mode != last_mode);
        bool theme_changed = (strcmp(cfg->theme, last_theme) != 0);

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
            last_temp_c   = -9999.0f;
            last_hum      = -1.0f;
            last_wx_valid = false;
            last_wx_min   = -1;
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
            /* Reset album, timer, and weather panel state on mode/theme switch */
            s_album_loaded = false; s_album_index = 0; album_switch = 0;
            last_remain_s  = INT32_MAX;
            weather_panel  = WEATHER_PANEL_TEMP; weather_panel_tick = 0;
            display_timer_reset();
            last_display_epoch = 0;   /* clear clock smoothing state so first fresh render
                                       * uses the true system time without clamping */
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
            strncpy(last_theme, cfg->theme, sizeof(last_theme) - 1);
            last_theme[sizeof(last_theme) - 1] = '\0';
            /* Force the remaining change-detection state to "no last frame"
             * so when the AP closes (or a client connects) the next
             * normal-mode tick re-renders from scratch — otherwise the
             * equality checks below would skip the redraw and leave PIN
             * digits on screen. */
            last_mode     = (app_mode_t)-1;
            last_t        = (struct tm){0};
            last_subs     = UINT32_MAX;
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
                strncpy(last_theme, cfg->theme, sizeof(last_theme) - 1);
                last_theme[sizeof(last_theme) - 1] = '\0';
                vTaskDelayUntil(&wake, pdMS_TO_TICKS(DISPLAY_TICK_MS_SLOW));
                continue;   /* skip mode switch, burn-in, rotation this tick */
            } else {
                /* Ticker not active — ensure running flag is clear */
                s_ticker_state.running = false;
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

            if (last_display_epoch > 0 && !first && !mode_changed && !theme_changed) {
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
            bool is_24cx  = (strcmp(cfg->time_type, "24H_CX") == 0);
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
            bool colon_blink_changed = !is_flip &&
                                       (t.tm_sec % 2 != last_t.tm_sec % 2);

            /* 24H_CX: track tube-6 info panel rotation.
             * s_cx_panel is an index into the ordered list of enabled panels;
             * it advances once per tube6_panel_ms milliseconds. */
            bool panel_changed = false;
            if (is_24cx) {
                int cx_panel_count = (cfg->tube6_panel_weather  ? 1 : 0)
                                   + (cfg->tube6_panel_weekdate ? 1 : 0)
                                   + (cfg->tube6_panel_ht       ? 1 : 0)
                                   + (cfg->tube6_panel_temp     ? 1 : 0)
                                   + (cfg->tube6_panel_sunrise  ? 1 : 0);
                if (cx_panel_count < 1) cx_panel_count = 1;  /* config enforces ≥1 */
                uint32_t panel_ms = cfg->tube6_panel_ms < 1000 ? 5000
                                                                : cfg->tube6_panel_ms;
                int64_t  now_us = esp_timer_get_time();
                if (first || mode_changed || time_type_changed || s_cx_panel_start == 0) {
                    s_cx_panel       = 0;
                    s_cx_panel_start = now_us;
                    s_cx_last_kind   = -1;   /* force background clear on first draw */
                    panel_changed    = true;
                } else if ((now_us - s_cx_panel_start) >= (int64_t)panel_ms * 1000LL) {
                    uint8_t next = (uint8_t)((s_cx_panel + 1) % cx_panel_count);
                    s_cx_panel_start = now_us;   /* always reset timer */
                    if (next != s_cx_panel) {    /* only flag changed when index moves */
                        s_cx_panel    = next;
                        panel_changed = true;
                    }
                }
            }

            /* ── Tubes 0-4: clock digits + colon ─────────────────────────── */
            if (first || mode_changed || theme_changed || time_type_changed ||
                    time_changed || leading_zero_changed || burnin_force_render) {
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

            /* ── Tube 6: 24H_CX info panel ────────────────────────────────
             * Only re-render when the panel rotates, the displayed data
             * changes, or a mode/theme change forces a full redraw.
             * Colon blinks and second ticks do NOT affect tube 6 content.
             * s_cx_last_t tracks the last time tube 6 was actually drawn. */
            if (is_24cx) {
                bool cx6_render =
                    first || mode_changed || theme_changed || time_type_changed ||
                    panel_changed || burnin_force_render ||
                    t.tm_min  != s_cx_last_t.tm_min  ||  /* H/T: refresh each minute */
                    t.tm_mday != s_cx_last_t.tm_mday;    /* date panel: new day      */
                if (cx6_render) {
                    render_cx_tube6(cfg, &t, s_cx_panel);
                    s_cx_last_t = t;
                }
            }
            strncpy(last_time_type, cfg->time_type, sizeof(last_time_type) - 1);
            last_time_type[sizeof(last_time_type) - 1] = '\0';
            last_leading_zero = cfg->leading_zero;
            break;
        }

        case APP_MODE_DATE: {
            /* Custom Clock shows date (DD/MM/YY); only needs re-render when the
             * day changes, or on first draw / mode or theme switch. */
            struct tm t; ntp_get_local(&t);
            if (first || mode_changed || theme_changed ||
                t.tm_mday != last_t.tm_mday ||
                t.tm_mon  != last_t.tm_mon  ||
                t.tm_year != last_t.tm_year ||
                burnin_force_render) {
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
                if (uncfg) render_followers_blank(cfg, "youtube");
                else        render_followers(cfg, count, "youtube");
                last_subs = count;
            }
            break;
        }

        case APP_MODE_INSTAGRAM: {
            static uint32_t last_insta = UINT32_MAX;
            const sub_count_t *s = instagram_get();
            bool uncfg = (cfg->instagram_user[0] == '\0');
            uint32_t count = (!uncfg && s->valid) ? (uint32_t)s->subscriber_count : 0;
            if (first || mode_changed || theme_changed || count != last_insta ||
                    burnin_force_render) {
                if (uncfg) render_followers_blank(cfg, "instagram");
                else        render_followers(cfg, count, "instagram");
                last_insta = count;
            }
            break;
        }

        case APP_MODE_TIKTOK: {
            static uint32_t last_tiktok = UINT32_MAX;
            const sub_count_t *s = tiktok_get();
            bool uncfg = (cfg->tiktok_user[0] == '\0');
            uint32_t count = (!uncfg && s->valid) ? (uint32_t)s->subscriber_count : 0;
            if (first || mode_changed || theme_changed || count != last_tiktok ||
                    burnin_force_render) {
                if (uncfg) render_followers_blank(cfg, "tiktok");
                else        render_followers(cfg, count, "tiktok");
                last_tiktok = count;
            }
            break;
        }

        case APP_MODE_MASTODON: {
            static uint32_t last_mastodon = UINT32_MAX;
            const sub_count_t *s = mastodon_get();
            bool uncfg = (cfg->mastodon_user[0] == '\0' ||
                          cfg->mastodon_instance[0] == '\0');
            uint32_t count = (!uncfg && s->valid) ? (uint32_t)s->subscriber_count : 0;
            if (first || mode_changed || theme_changed || count != last_mastodon ||
                    burnin_force_render) {
                if (uncfg) render_followers_blank(cfg, "mastodon");
                else        render_followers(cfg, count, "mastodon");
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
             * Respects weather_panel0/1/2_en — panels not enabled are skipped.
             * If the currently active panel has been disabled, jumps to the next
             * enabled one immediately without waiting for the rotation timer.   */
            bool panel_flipped = false;
            if (now_valid) {
                bool pen[3] = { cfg->weather_panel0_en,
                                cfg->weather_panel1_en,
                                cfg->weather_panel2_en };

                /* Build ordered list of enabled panel indices */
                int elist[3]; int ecnt = 0;
                for (int _i = 0; _i < 3; _i++)
                    if (pen[_i]) elist[ecnt++] = _i;

                if (ecnt == 0) {
                    /* Nothing explicitly enabled — force temp panel */
                    elist[0] = WEATHER_PANEL_TEMP; ecnt = 1;
                }

                /* If current panel was disabled, jump to first enabled */
                bool cur_ok = false;
                for (int _i = 0; _i < ecnt; _i++)
                    if (elist[_i] == weather_panel) { cur_ok = true; break; }
                if (!cur_ok) {
                    weather_panel = elist[0]; weather_panel_tick = 0; panel_flipped = true;
                } else if (ecnt > 1) {
                    /* More than one panel enabled — rotate on the configured interval */
                    TickType_t now_t = xTaskGetTickCount();
                    if (weather_panel_tick == 0) {
                        weather_panel_tick = now_t;
                    } else if ((now_t - weather_panel_tick) >= pdMS_TO_TICKS(
                                   cfg->weather_panel_ms ? cfg->weather_panel_ms : 5000)) {
                        /* Advance to next panel in the enabled list */
                        int cur_pos = 0;
                        for (int _i = 0; _i < ecnt; _i++)
                            if (elist[_i] == weather_panel) { cur_pos = _i; break; }
                        weather_panel      = elist[(cur_pos + 1) % ecnt];
                        weather_panel_tick = now_t;
                        panel_flipped      = true;
                    }
                }
            }

            /* True when the animated sunrise/sunset panel is visible — drives
             * FAST tick (20 Hz) and per-frame animation blit of tubes 0 and 4. */
            sun_anim = (weather_panel == WEATHER_PANEL_SUN && cfg->weather_panel2_en);

            /* Trigger re-render when: first draw, mode/theme change, new data
             * values arrived, validity flips, panel switched, or solar-time
             * minute changed (only relevant on panel 2). */
            bool wx_changed = false;
            if (now_valid && last_wx_valid) {
                bool fahrenheit = (strncmp(cfg->temp_format, "Fahrenheit", 10) == 0);
                float cur_tf  = fahrenheit ? w->temp_c    * 9.0f / 5.0f + 32.0f : w->temp_c;
                float last_tf = fahrenheit ? last_temp_c  * 9.0f / 5.0f + 32.0f : last_temp_c;
                int cur_t_i   = (int)(cur_tf  < -0.5f ? -cur_tf  + 0.5f : cur_tf  + 0.5f);
                int last_t_i  = (int)(last_tf < -0.5f ? -last_tf + 0.5f : last_tf + 0.5f);
                bool cur_neg  = (cur_tf  < -0.5f);
                bool last_neg = (last_tf < -0.5f);
                wx_changed = (cur_t_i != last_t_i || cur_neg != last_neg ||
                              (int)(w->humidity + 0.5f) != (int)(last_hum + 0.5f));
                if (!wx_changed && weather_panel == WEATHER_PANEL_SUN)
                    wx_changed = (wx_tm.tm_min != last_wx_min);
            }
            bool valid_changed = (now_valid != last_wx_valid);
            bool full_redraw   = (first || mode_changed || theme_changed || wx_changed ||
                                  valid_changed || panel_flipped || burnin_force_render);
            if (full_redraw || sun_anim) {
                render_weather(cfg, weather_panel, /*anim_only=*/!full_redraw);
                /* Only update change-detection state on a full redraw so that
                 * a pure animation tick doesn't mask a real data change on the
                 * next full-redraw cycle.                                       */
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
        strncpy(last_theme, cfg->theme, sizeof(last_theme) - 1);
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
        TickType_t tick_ms = (mode == APP_MODE_SPECTRUM || sun_anim || weather_needs_fast)
                             ? pdMS_TO_TICKS(DISPLAY_TICK_MS_FAST)
                             : pdMS_TO_TICKS(DISPLAY_TICK_MS_SLOW);

        /* Re-sync wake timer when we've fallen behind the current tick budget.
         *
         * Background: pixel blits use spi_device_transmit() (interrupt/DMA path)
         * which yields the CPU while the DMA engine clocks out each chunk.
         * In Spectrum mode (tick = 50 ms) a full frame is 6 tubes ×
         * 160 row-transactions; even with CPU-yielding transfers the total
         * render time can occasionally exceed the tick budget.
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

void display_show_wait(void)
{
    /* Suspend the display task first so it cannot issue SPI transactions
     * while we write, and so it does not overwrite the wait screen after
     * we return.  Flash operations (OTA / LittleFS) always end in
     * esp_restart() so the task is never resumed. */
    if (s_display_task_handle) {
        vTaskSuspend(s_display_task_handle);
    }
    for (int i = 0; i < LCD_COUNT; i++) {
        display_show_image(i, "/images/system/wait.jpg");
    }
    ESP_LOGI(TAG, "Wait screen shown on all tubes — display task suspended");
}
