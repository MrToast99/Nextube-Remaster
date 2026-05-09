#include "display.h"
#include "board_pins.h"
#include "esp_log.h"
#include "esp_timer.h"          /* esp_timer_get_time — AP PIN phase clock */
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "weather.h"
#include "wifi_manager.h"       /* AP PIN visibility (S1) */
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

/* Update indicator flag — set true to overlay 4 red rows at the physical
 * bottom of tube 5 on every frame.  Declared here (before display_show_digit)
 * so the function can read it.  Implementation: display_set_update_indicator(). */
static volatile bool s_update_indicator = false;

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
#define BURNIN_COLOR_SECS   30   /* seconds per step in the cycle */

static void lcd_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_LCD_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_polling_transmit(spi_dev, &t);
}

static void lcd_data(const uint8_t *data, int len)
{
    if (len <= 0) return;
    gpio_set_level(PIN_LCD_DC, 1);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    spi_device_polling_transmit(spi_dev, &t);
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

    /* Power Control – values typical for ST7735 IPS panels to improve contrast/vibrancy */
    lcd_cmd(0xC0); uint8_t pc1[] = {0xA2, 0x02, 0x84}; lcd_data(pc1, 3);
    lcd_cmd(0xC1); uint8_t pc2[] = {0xC5};             lcd_data(pc2, 1);
    lcd_cmd(0xC2); uint8_t pc3[] = {0x0A, 0x00};       lcd_data(pc3, 2);
    lcd_cmd(0xC3); uint8_t pc4[] = {0x8A, 0x2A};       lcd_data(pc4, 2);
    lcd_cmd(0xC4); uint8_t pc5[] = {0x8A, 0xEE};       lcd_data(pc5, 2);
    lcd_cmd(0xC5); uint8_t vcom[] = {0x0E};            lcd_data(vcom, 1);

    /* Gamma Correction – ensures correct luminance curve and prevents washed-out look */
    lcd_cmd(0xE0); uint8_t gm1[] = {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10}; lcd_data(gm1, 16);
    lcd_cmd(0xE1); uint8_t gm2[] = {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10}; lcd_data(gm2, 16);

    lcd_cmd(0x3A); lcd_data_byte(0x05);            /* COLMOD  RGB565 */
    lcd_cmd(0x36); lcd_data_byte(0xC8);            /* MADCTL  MY|MX|BGR = 180° rotation */
    uint8_t fr[] = {0x01, 0x2C, 0x2D};
    lcd_cmd(0xB1); lcd_data(fr, 3);                /* FRMCTR1 */
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

    /* Hardware reset is shared — pulse RST once to reset all 6 displays,
     * then send the init sequence to each tube individually. */
    gpio_set_level(PIN_LCD_RST, 0); vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));
    for (int i = 0; i < LCD_COUNT; i++) { st7735_init_one(i); display_fill(i, 0x0000); }
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

void display_fill(int tube, uint16_t color)
{
    if (tube < 0 || tube >= LCD_COUNT) return;
    select_tube(tube);
    uint8_t ox = (uint8_t)((int)LCD_OFFSET_X + (int)s_burnin_shift_x);
    lcd_cmd(0x2A); uint8_t ca[] = {0,ox,0,(uint8_t)(ox+LCD_WIDTH-1)}; lcd_data(ca,4);
    lcd_cmd(0x2B); uint8_t ra[] = {0,LCD_OFFSET_Y,0,LCD_OFFSET_Y+LCD_HEIGHT-1}; lcd_data(ra,4);
    lcd_cmd(0x2C);
    gpio_set_level(PIN_LCD_DC, 1);
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
    uint8_t ox = (uint8_t)((int)LCD_OFFSET_X + (int)s_burnin_shift_x);
    lcd_cmd(0x2A); uint8_t ca[] = {0,ox,0,(uint8_t)(ox+w-1)}; lcd_data(ca,4);
    lcd_cmd(0x2B); uint8_t ra[] = {0,LCD_OFFSET_Y,0,LCD_OFFSET_Y+h-1}; lcd_data(ra,4);
    lcd_cmd(0x2C);
    gpio_set_level(PIN_LCD_DC, 1);
    /* `data` may be in PSRAM; ESP32 SPI DMA cannot access PSRAM directly.
     * Copy 8 rows at a time into a stack SRAM chunk buffer and send via
     * polling transmit.  8 rows reduces transaction count 160→20 per tube,
     * cutting per-transaction GPIO/controller overhead by ~8×. */
#define DISP_CHUNK_ROWS 8
    uint8_t chunk[LCD_WIDTH * 2 * DISP_CHUNK_ROWS];  /* 1280 B — always SRAM */
    for (int y = 0; y < h; y += DISP_CHUNK_ROWS) {
        int rows = (y + DISP_CHUNK_ROWS <= h) ? DISP_CHUNK_ROWS : h - y;
        memcpy(chunk, data + y * w * 2, (size_t)(rows * w * 2));
        spi_transaction_t t = { .length = (size_t)(rows * w * 2) * 8, .tx_buffer = chunk };
        spi_device_polling_transmit(spi_dev, &t);
    }
#undef DISP_CHUNK_ROWS

    /* ── Update indicator overlay ──────────────────────────────────────── */
    /* When s_update_indicator is set, paint 4 rows of solid red at the
     * physical bottom of tube 5.  Physical bottom = the last 4 rows of the
     * address window (LCD_OFFSET_Y + h - 4 .. LCD_OFFSET_Y + h - 1).
     * Red in RGB565 big-endian = 0xF800 → bytes {0xF8, 0x00}.
     * The SPI device is still selected (cs low) so no extra select call is
     * needed — we simply reissue CASET/RASET for the 4-row window. */
    if (tube == LCD_COUNT - 1 && s_update_indicator) {
        uint8_t ca2[] = {0, LCD_OFFSET_X, 0, (uint8_t)(LCD_OFFSET_X + w - 1)};
        lcd_cmd(0x2A); lcd_data(ca2, 4);
        uint8_t ra2[] = {0, (uint8_t)(LCD_OFFSET_Y + h - 4), 0, (uint8_t)(LCD_OFFSET_Y + h - 1)};
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
    s_burnin_mask     = mask & 0x3F;
    s_burnin_end_time = (mask && duration_s) ? time(NULL) + (time_t)duration_s : 0;
    ESP_LOGI(TAG, "burn-in mask: 0x%02X  duration: %s",
             (unsigned)s_burnin_mask,
             duration_s ? "timed" : "manual");
}

/* ════════════════════════════════════════════════════════════════════
 *  JPEG asset loader
 * ════════════════════════════════════════════════════════════════════ */
/* Forward declaration — flip_to_image() is defined after display_show_image()
 * but called from within it for FlipClock theme paths. */
static void flip_to_image(int tube, const uint8_t *new_buf, const char *path);

#include <stdio.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "jpeg_decoder.h"   /* espressif/esp_jpeg v1.x managed component */

/* Allocate decode buffer from PSRAM so we don't exhaust DRAM. */
#define PSRAM_MALLOC(sz)  heap_caps_malloc((sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/* TJpgDec workspace: ~3100 bytes for JD_FASTDECODE=0 – round up with margin. */
#define JPEG_WORK_BUF_SIZE  3200

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

/* Evict all cache entries (called on theme change so stale paths age out). */
static void img_cache_flush(void)
{
    for (int i = 0; i < IMG_CACHE_ENTRIES; i++) {
        s_img_cache[i].path[0] = '\0';
        s_img_cache[i].used_at = 0;
        /* Retain the data allocation — it will be overwritten on next use. */
    }
    s_cache_clock = 0;
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
    if (sz <= 0 || sz > 200000) { fclose(f); return NULL; }

    uint8_t *jpeg_buf = PSRAM_MALLOC(sz);
    if (!jpeg_buf) { fclose(f); return NULL; }
    fread(jpeg_buf, 1, sz, f);
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

void display_path_weekday(char *buf, size_t n, const char *theme, int wday)
{
    /* struct tm: 0=Sunday … 6=Saturday */
    const char *days[] = {"sunday","monday","tuesday","wednesday","thursday","friday","saturday"};
    snprintf(buf, n, "/images/themes/%s/MutiInfo/WeekDate/week/%s.jpg",
             theme, (wday >= 0 && wday <= 6) ? days[wday] : "monday");
}

void display_path_date_digit(char *buf, size_t n, const char *theme, int digit)
{ snprintf(buf, n, "/images/themes/%s/MutiInfo/WeekDate/date/%d.jpg", theme, digit); }

void display_path_system(char *buf, size_t n, const char *cat, const char *name)
{ snprintf(buf, n, "/images/system/%s/%s.jpg", cat, name); }

/* ── High-level helpers ────────────────────────────────────────────── */
void display_show_number(int tube, int digit, const char *theme)
{
    char p[256]; display_path_number(p, sizeof(p), theme, digit);
    display_show_image(tube, p);
}

void display_show_ampm(int tube, const char *name, const char *theme)
{
    char p[256]; display_path_ampm(p, sizeof(p), theme, name);
    display_show_image(tube, p);
}

/* ════════════════════════════════════════════════════════════════════
 *  Display task – full mode renderer
 * ════════════════════════════════════════════════════════════════════ */
#include "config_mgr.h"
#include "ntp_time.h"
#include "weather.h"
#include "youtube_bili.h"
#include "microphone.h"
#include "freertos/semphr.h"

/* ── Mode render helpers ────────────────────────────────────────────── */

/* ── S1 — AP PIN renderer ────────────────────────────────────────────
 * Called from the display task whenever the setup AP is broadcasting and
 * no client is associated.  Shows the 8-digit PIN across 6 tubes in two
 * 3-second phases:
 *
 *   Phase 0 (3 s):  tubes show pin[0..5]
 *   Phase 1 (3 s):  tubes show pin[2..7]
 *
 * The 4-digit overlap (pin[2..5] visible in both phases) gives the user a
 * stable anchor while their eyes piece the full PIN together.  Themes need
 * no new artwork — this leverages each theme's existing Numbers/0..9.jpg.
 *
 * Defined here (after the include of config_mgr.h above) rather than next
 * to display_show_number, because nextube_config_t is only visible from
 * this point in the translation unit forward.
 */
static void render_ap_pin(const nextube_config_t *cfg)
{
    const char *pin = wifi_manager_get_ap_pin();
    if (!pin || strlen(pin) < 8) return;

    /* Scrolling marquee: the virtual tape is 3 blanks followed by the 8 PIN
     * digits (11 characters total), repeating endlessly.  The 6-tube window
     * advances one position every 500 ms so digits scroll right-to-left —
     * new digits enter on the rightmost tube and exit on the left, with a
     * 3-blank gap between each repetition to give the eye a reset point.
     *
     * Example at scroll=3: [d0][d1][d2][d3][d4][d5]
     *           at scroll=8: [d5][d6][d7][ ][ ][ ]
     *           at scroll=0: [ ][ ][ ][d0][d1][d2]  */
    const int seq_len = 11;   /* 3 blanks + 8 digits */
    const int step_ms = 1000;

    int64_t now_ms = esp_timer_get_time() / 1000;
    int     scroll  = (int)((now_ms / step_ms) % seq_len);

    for (int tube = 0; tube < LCD_COUNT; tube++) {
        int pos = (scroll + tube) % seq_len;
        if (pos < 3) {
            display_fill(tube, 0x0000);   /* blank gap */
        } else {
            display_show_number(tube, pin[pos - 3] - '0', cfg->theme);
        }
    }
}

/* Custom Clock: shows the current date as DD  MM  YY across the 6 tubes.
 * Example: 15 March 2026 → [1][5][0][3][2][6]
 * This gives the user a dedicated date-display mode that is distinct from
 * the standard clock (which always shows time). */
static void render_date(const nextube_config_t *cfg, const struct tm *t)
{
    int d  = t->tm_mday;        /* 1-31  */
    int mo = t->tm_mon + 1;     /* 1-12  */
    int y  = t->tm_year % 100;  /* 0-99 (last two digits of year) */
    int digits[6] = { d/10, d%10, mo/10, mo%10, y/10, y%10 };
    for (int i = 0; i < 6; i++)
        display_show_number(i, digits[i], cfg->theme);
}

static void render_clock(const nextube_config_t *cfg, const struct tm *t)
{
    bool is_12h  = (strcmp(cfg->time_type, "12H")    == 0);
    bool is_24ns = (strcmp(cfg->time_type, "24H_NS") == 0);
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
    } else if (is_24ns) {
        /* 24H no-seconds: H1  H2  colon  M1  M2  [tube5]
         * tube5 is user-configurable: "blank" or "weather" */
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

static void render_subs(const nextube_config_t *cfg)
{
    const sub_count_t *s = youtube_bili_get();
    uint32_t count = s->valid ? (uint32_t)s->subscriber_count : 0;

    if (count >= 1000000) {
        render_number6(count / 1000, cfg->theme, "youtube", "m-sub");
    } else if (count >= 1000) {
        render_number6(count / 1000, cfg->theme, "youtube", "k-sub");
    } else {
        /* tube 0 = youtube icon, tubes 1-5 = 5-digit count, leading zeros blanked */
        display_show_ampm(0, "youtube", cfg->theme);
        static const uint32_t div5[5] = { 10000, 1000, 100, 10, 1 };
        bool leading = true;
        for (int i = 0; i < 5; i++) {
            uint8_t d = (count / div5[i]) % 10;
            /* Suppress leading zeros but always show the units digit (i == 4) */
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

static void render_scoreboard(const nextube_config_t *cfg)
{
    /* Show 6 zeros until score data is driven via a future API. */
    for (int i = 0; i < 6; i++) display_show_number(i, 0, cfg->theme);
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

        /* Open SPI window — identical to display_show_digit() preamble. */
        select_tube(tube);
        uint8_t ox = (uint8_t)((int)LCD_OFFSET_X + (int)s_burnin_shift_x);
        lcd_cmd(0x2A);
        uint8_t ca[] = {0, ox, 0, (uint8_t)(ox + LCD_WIDTH - 1)};
        lcd_data(ca, 4);
        lcd_cmd(0x2B);
        uint8_t ra[] = {0, LCD_OFFSET_Y, 0, LCD_OFFSET_Y + LCD_HEIGHT - 1};
        lcd_data(ra, 4);
        lcd_cmd(0x2C);
        gpio_set_level(PIN_LCD_DC, 1);

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

static void album_load_list(void)
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
    s_album_loaded = true;
}

static void render_album(const nextube_config_t *cfg,
                         TickType_t *last_switch, bool force)
{
    album_load_list();
    if (s_album_count == 0) {
        for (int i = 0; i < LCD_COUNT; i++) display_fill(i, 0x0000);
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
/* render_weather – panel 0 = temperature + icon, panel 1 = humidity + icon.
 *
 * Two-panel layout (auto-cycles in the display task):
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
 */
static void render_weather(const nextube_config_t *cfg, int panel)
{
    const weather_data_t *w = weather_get();
    char path[128];

    if (!w || !w->valid) {
        /* No weather data yet – show "·" (dot) on every tube. */
        for (int i = 0; i < LCD_COUNT; i++)
            display_show_ampm(i, "dot", cfg->theme);
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
static SemaphoreHandle_t s_timer_mutex = NULL;

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

    /* Per-render state for change detection */
    struct tm     last_t        = {0};
    app_mode_t    last_mode     = (app_mode_t)-1;
    char          last_theme[32]     = {0};
    char          last_time_type[8]  = {0};
    uint32_t      last_subs     = UINT32_MAX;
    int32_t       last_remain_s = INT32_MAX;  /* countdown/pomodoro change detection */
    float         last_temp_c   = -9999.0f;   /* weather change detection */
    float         last_hum      = -1.0f;
    bool          last_wx_valid = false;       /* detect when data first arrives */
    bool          last_leading_zero = false;    /* leading-zero change detection */
    bool          last_bl_on    = true;        /* backlight on/off tracking */
    uint8_t       last_bl_brt   = 255;         /* sentinel: force-apply on first tick */
    TickType_t    album_switch       = 0;
    TickType_t    rotation_tick      = 0;      /* tick when current mode started */
    int           weather_panel      = 0;      /* 0 = temp, 1 = humidity */
    TickType_t    weather_panel_tick = 0;      /* tick of last panel switch */
    bool          first              = true;
    int           last_hour             = -1;  /* for hourly burn-in shift update */
    int           burnin_last_run_yday  = -1;  /* day-of-year the scheduled burn-in last ran */

    TickType_t wake = xTaskGetTickCount();
    rotation_tick = wake;

    while (1) {
        nextube_config_t cfg_snap;
        config_lock();
        cfg_snap = *config_get();
        config_unlock();
        const nextube_config_t *cfg = &cfg_snap;
        app_mode_t mode = cfg->current_mode;
        bool mode_changed  = (mode != last_mode);
        bool theme_changed = (strcmp(cfg->theme, last_theme) != 0);

        /* ── Mode rotation ───────────────────────────────────────────
         * Only fires when rotation_enabled is true.  Any mode change
         * (UI, button, or previous rotation step) resets the timer so
         * every mode gets the full interval before auto-advancing.    */
        if (mode_changed) {
            rotation_tick = xTaskGetTickCount();
        } else if (cfg->rotation_enabled && !first) {
            uint16_t interval = cfg->rotation_interval_s ? cfg->rotation_interval_s : 60;
            uint32_t elapsed_ms = (uint32_t)pdTICKS_TO_MS(
                                      xTaskGetTickCount() - rotation_tick);
            if (elapsed_ms >= (uint32_t)interval * 1000u) {
                config_advance_mode();   /* updates cfg->current_mode + saves */
                rotation_tick = xTaskGetTickCount();
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
            weather_panel  = 0; weather_panel_tick = 0;
            display_timer_reset();
        }

        /* Apply backlight on/off whenever the config changes.
         * Default to primary lcd_brightness, overridden by Night Mode if enabled. */
        uint8_t target_brt = cfg->lcd_brightness;
        struct tm now_tm;

        if (cfg->auto_brightness && ntp_time_synced()) {
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

            /* Hourly pixel-shift update */
            if (burn_tm.tm_hour != last_hour) {
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
                    display_set_burnin_mask(cfg->burnin_auto_mask,
                                            cfg->burnin_auto_duration_s);
                    ESP_LOGI(TAG,
                             "Scheduled burn-in fired (%s): mask=0x%02X  dur=%lus",
                             cfg->burnin_auto_interval,
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
            render_ap_pin(cfg);
            /* Force the change-detection state to "no last frame" so when
             * the AP closes (or a client connects) the next normal-mode
             * tick re-renders from scratch — otherwise the equality checks
             * below would skip the redraw and leave PIN digits on screen. */
            last_mode     = (app_mode_t)-1;
            last_t        = (struct tm){0};
            last_subs     = UINT32_MAX;
            last_remain_s = INT32_MAX;
            last_temp_c   = -9999.0f;
            last_hum      = -1.0f;
            last_wx_valid = false;
            first         = true;
            vTaskDelayUntil(&wake, pdMS_TO_TICKS(200));
            continue;
        }

        switch (mode) {

        case APP_MODE_CLOCK: {
            struct tm t; ntp_get_local(&t);
            bool is_24ns = (strcmp(cfg->time_type, "24H_NS") == 0);
            bool is_flip = (strcmp(cfg->theme, "FlipClock")  == 0);
            bool time_type_changed  = (strcmp(cfg->time_type, last_time_type) != 0);
            bool leading_zero_changed = (cfg->leading_zero != last_leading_zero);
            /* 24H_NS shows no seconds — only re-render on minute/hour change.
             * Standard 24H shows seconds and re-renders every second. */
            bool time_changed = (t.tm_hour != last_t.tm_hour ||
                                 t.tm_min  != last_t.tm_min);
            if (!is_24ns) time_changed |= (t.tm_sec != last_t.tm_sec);
            /* Colon blinks every other second — need a re-render on each
             * even/odd transition even when only the colon image changes. */
            bool colon_blink_changed = !is_flip &&
                                       (t.tm_sec % 2 != last_t.tm_sec % 2);
            if (first || mode_changed || theme_changed || time_type_changed ||
                    time_changed || colon_blink_changed || leading_zero_changed) {
                render_clock(cfg, &t);
                last_t = t;
            }
            strlcpy(last_time_type, cfg->time_type, sizeof(last_time_type));
            last_leading_zero = cfg->leading_zero;
            break;
        }

        case APP_MODE_CUSTOM_CLOCK: {
            /* Custom Clock shows date (DD/MM/YY); only needs re-render when the
             * day changes, or on first draw / mode or theme switch. */
            struct tm t; ntp_get_local(&t);
            if (first || mode_changed || theme_changed ||
                t.tm_mday != last_t.tm_mday ||
                t.tm_mon  != last_t.tm_mon  ||
                t.tm_year != last_t.tm_year) {
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
            if (first || mode_changed || theme_changed || remain != last_remain_s) {
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
            if (first || mode_changed || theme_changed || remain != last_remain_s) {
                render_pomodoro_display(cfg, remain, in_break);
                last_remain_s = remain;
            }
            break;
        }

        case APP_MODE_YOUTUBE: {
            const sub_count_t *sub = youtube_bili_get();
            uint32_t count = sub->valid ? (uint32_t)sub->subscriber_count : 0;
            if (first || mode_changed || theme_changed || count != last_subs) {
                render_subs(cfg);
                last_subs = count;
            }
            break;
        }

        case APP_MODE_SCOREBOARD:
            if (first || mode_changed || theme_changed)
                render_scoreboard(cfg);
            break;

        case APP_MODE_SPECTRUM:
            /* Audio changes every frame — always re-render at the display task rate. */
            render_spectrum(cfg);
            break;

        case APP_MODE_ALBUM:
            render_album(cfg, &album_switch, first || mode_changed || theme_changed);
            break;

        case APP_MODE_WEATHER: {
            const weather_data_t *w = weather_get();
            bool now_valid  = (w != NULL && w->valid);

            /* Panel auto-switch: cycle temp (0) ↔ humidity (1) on a timer.
             * Respects weather_panel0_en / weather_panel1_en — if only one
             * panel is enabled it is shown exclusively; if both are enabled
             * they alternate every weather_panel_ms ms. */
            bool panel_flipped = false;
            if (now_valid) {
                bool p0 = cfg->weather_panel0_en;
                bool p1 = cfg->weather_panel1_en;

                /* If the currently active panel has been disabled, jump to the
                 * other one immediately without waiting for the rotation timer. */
                if (weather_panel == 0 && !p0 && p1) {
                    weather_panel = 1; weather_panel_tick = 0; panel_flipped = true;
                } else if (weather_panel == 1 && !p1 && p0) {
                    weather_panel = 0; weather_panel_tick = 0; panel_flipped = true;
                } else if (p0 && p1) {
                    /* Both enabled — rotate on the configured interval */
                    TickType_t now_t = xTaskGetTickCount();
                    if (weather_panel_tick == 0) {
                        weather_panel_tick = now_t;       /* arm timer on first valid frame */
                    } else if ((now_t - weather_panel_tick) >= pdMS_TO_TICKS(
                                   cfg->weather_panel_ms ? cfg->weather_panel_ms : 5000)) {
                        weather_panel      = 1 - weather_panel;
                        weather_panel_tick = now_t;
                        panel_flipped      = true;
                    }
                }
                /* Single-panel-only case: weather_panel is already correct (clamped above),
                 * timer is irrelevant — no further action needed. */
            }

            /* Trigger re-render when: first draw, mode/theme change, new data
             * values arrived, validity flips, OR the panel just switched. */
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
            }
            bool valid_changed = (now_valid != last_wx_valid);
            if (first || mode_changed || theme_changed || wx_changed || valid_changed || panel_flipped) {
                render_weather(cfg, weather_panel);
                if (now_valid) { last_temp_c = w->temp_c; last_hum = w->humidity; }
                last_wx_valid = now_valid;
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
         * voltage extremes.  Timer expiry auto-restores all tubes. */
        if (s_burnin_mask) {
            time_t now_t = time(NULL);

            /* Auto-restore when the requested duration has elapsed */
            if (s_burnin_end_time != 0 && now_t >= s_burnin_end_time) {
                s_burnin_mask     = 0;
                s_burnin_end_time = 0;
                ESP_LOGI(TAG, "burn-in timer expired — restoring normal display");
            } else {
                uint16_t col = s_burnin_colors[
                    (size_t)(now_t / BURNIN_COLOR_SECS) % (size_t)BURNIN_COLOR_COUNT];
                for (int _t = 0; _t < LCD_COUNT; _t++) {
                    if (s_burnin_mask & (1u << _t))
                        display_fill(_t, col);
                }
            }
        }

        first = false;

        /* Spectrum mode runs at 20 Hz (50 ms) to match the LED task refresh
         * rate and give snappy bar response.  All other modes use 5 Hz (200 ms). */
        TickType_t tick_ms = (mode == APP_MODE_SPECTRUM)
                             ? pdMS_TO_TICKS(50)
                             : pdMS_TO_TICKS(200);

        /* Re-sync wake timer when we've fallen behind (e.g. blocked on
         * SPIFFS while audio pre-buffers).  Cap at 200 ms regardless of
         * tick_ms so a spectrum stall doesn't cause a catch-up burst in
         * a slower mode after switching. */
        {
            TickType_t now_tick = xTaskGetTickCount();
            if ((TickType_t)(now_tick - wake) > pdMS_TO_TICKS(200))
                wake = now_tick;
        }
        vTaskDelayUntil(&wake, tick_ms);
    }
}

void display_task_start(void)
{
    xTaskCreatePinnedToCore(display_task, "display", 8192, NULL, 6, NULL, 1);
    ESP_LOGI(TAG, "Display task started");
}
