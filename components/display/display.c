#include "display.h"
#include "board_pins.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "weather.h"
#include <string.h>
#include <stdint.h>
#include <math.h>

static const char *TAG = "display";
static const int cs_pins[LCD_COUNT] = {
    PIN_LCD1_CS, PIN_LCD2_CS, PIN_LCD3_CS,
    PIN_LCD4_CS, PIN_LCD5_CS, PIN_LCD6_CS
};
static spi_device_handle_t spi_dev;

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
        .timer_num = LEDC_TIMER_0, .freq_hz = 20000, .clk_cfg = LEDC_AUTO_CLK,
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

void display_fill(int tube, uint16_t color)
{
    if (tube < 0 || tube >= LCD_COUNT) return;
    select_tube(tube);
    lcd_cmd(0x2A); uint8_t ca[] = {0,LCD_OFFSET_X,0,LCD_OFFSET_X+LCD_WIDTH-1}; lcd_data(ca,4);
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
    lcd_cmd(0x2A); uint8_t ca[] = {0,LCD_OFFSET_X,0,LCD_OFFSET_X+w-1}; lcd_data(ca,4);
    lcd_cmd(0x2B); uint8_t ra[] = {0,LCD_OFFSET_Y,0,LCD_OFFSET_Y+h-1}; lcd_data(ra,4);
    lcd_cmd(0x2C);
    gpio_set_level(PIN_LCD_DC, 1);
    /* Use interrupt-based transmit so the task yields during DMA, allowing
     * IDLE1 on CPU1 to reset the task watchdog. */
    spi_transaction_t t = { .length = w*h*2*8, .tx_buffer = data };
    spi_device_transmit(spi_dev, &t);
    deselect_all();
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

void display_show_image(int tube, const char *path)
{
    if (tube < 0 || tube >= LCD_COUNT || !path) return;

    /* ── 1. Read JPEG file from SPIFFS ─────────────────────────── */
    char full[320];
    snprintf(full, sizeof(full), "/spiffs%s", path);
    FILE *f = fopen(full, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Image not found: %s", full);
        display_fill(tube, 0x0000);
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 200000) { fclose(f); display_fill(tube, 0x0000); return; }

    uint8_t *jpeg_buf = PSRAM_MALLOC(sz);
    if (!jpeg_buf) { fclose(f); display_fill(tube, 0x0000); return; }
    fread(jpeg_buf, 1, sz, f);
    fclose(f);

    /* ── 2. Decode JPEG → RGB565 big-endian (for ST7735) ────────── */
    uint8_t *work_buf = PSRAM_MALLOC(JPEG_WORK_BUF_SIZE);
    size_t   out_sz   = LCD_WIDTH * LCD_HEIGHT * 2;
    uint8_t *rgb_buf  = PSRAM_MALLOC(out_sz);

    if (!work_buf || !rgb_buf) {
        free(work_buf); free(rgb_buf); free(jpeg_buf);
        display_fill(tube, 0x0000); return;
    }

    esp_jpeg_image_cfg_t dec_cfg = {0};
    dec_cfg.indata                       = jpeg_buf;
    dec_cfg.indata_size                  = (uint32_t)sz;
    dec_cfg.outbuf                       = rgb_buf;
    dec_cfg.outbuf_size                  = out_sz;
    dec_cfg.out_format                   = JPEG_IMAGE_FORMAT_RGB565;
    dec_cfg.out_scale                    = JPEG_IMAGE_SCALE_0;
    dec_cfg.flags.swap_color_bytes       = 1;   /* big-endian bytes for ST7735 */
    dec_cfg.advanced.working_buffer      = work_buf;
    dec_cfg.advanced.working_buffer_size = JPEG_WORK_BUF_SIZE;

    esp_jpeg_image_output_t out_img = {0};
    esp_err_t dec_err = esp_jpeg_decode(&dec_cfg, &out_img);

    free(work_buf);
    free(jpeg_buf);

    if (dec_err != ESP_OK) {
        ESP_LOGW(TAG, "JPEG decode failed %d: %s", dec_err, full);
        free(rgb_buf); display_fill(tube, 0x0000); return;
    }

    /* ── 3. Push RGB565 frame to LCD (with FlipClock animation) ────── */
    if ((int)out_img.width  == LCD_WIDTH  &&
        (int)out_img.height == LCD_HEIGHT &&
        strstr(path, "/FlipClock/") != NULL) {
        /* flip_to_image() runs the split-flap animation when the image
         * has changed, then pushes the final frame and updates the cache. */
        flip_to_image(tube, rgb_buf, path);
    } else {
        display_show_digit(tube, rgb_buf, (int)out_img.width, (int)out_img.height);
    }
    free(rgb_buf);
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

    if (path_changed && s_flip_prev[tube] != NULL) {
        /* Allocate a temporary working frame in PSRAM */
        uint8_t *frame = PSRAM_MALLOC(FLIP_FRAME_BYTES);
        if (frame) {
            for (int step = 0; step < FLIP_STEPS; step++) {
                flip_build_frame(frame, s_flip_prev[tube], new_buf, step);
                /* display_show_digit() SPI transmission (~8 ms) naturally
                 * paces the animation — no extra vTaskDelay() needed. */
                display_show_digit(tube, frame, LCD_WIDTH, LCD_HEIGHT);
            }
            free(frame);
        }
        /* If frame alloc failed: animation silently skipped; final frame
         * pushed below so the tube still shows the correct image. */
    }

    /* Always push the final (complete) new frame */
    display_show_digit(tube, new_buf, LCD_WIDTH, LCD_HEIGHT);

    /* Update per-tube cache -------------------------------------------- */
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
    if (strstr(s_flip_path[tube], "/AMPM/blank.jpg")      != NULL) return;
    if (strstr(s_flip_path[tube], "/Temperature/degree")  != NULL) return;

    /* ── Decode blank.jpg → PSRAM and store as the previous frame ── */
    char full[328];   /* 320 (blank_path max) + 7 ("/spiffs") + 1 (NUL) */
    snprintf(full, sizeof(full), "/spiffs%s", blank_path);
    FILE *f = fopen(full, "rb");
    if (!f) return;

    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 200000) { fclose(f); return; }

    uint8_t *jpeg_buf = PSRAM_MALLOC(sz);
    if (!jpeg_buf) { fclose(f); return; }
    fread(jpeg_buf, 1, sz, f);
    fclose(f);

    uint8_t *work_buf = PSRAM_MALLOC(JPEG_WORK_BUF_SIZE);
    uint8_t *rgb_buf  = PSRAM_MALLOC(FLIP_FRAME_BYTES);
    if (!work_buf || !rgb_buf) {
        free(work_buf); free(rgb_buf); free(jpeg_buf); return;
    }

    esp_jpeg_image_cfg_t dec_cfg = {0};
    dec_cfg.indata                       = jpeg_buf;
    dec_cfg.indata_size                  = (uint32_t)sz;
    dec_cfg.outbuf                       = rgb_buf;
    dec_cfg.outbuf_size                  = FLIP_FRAME_BYTES;
    dec_cfg.out_format                   = JPEG_IMAGE_FORMAT_RGB565;
    dec_cfg.out_scale                    = JPEG_IMAGE_SCALE_0;
    dec_cfg.flags.swap_color_bytes       = 1;
    dec_cfg.advanced.working_buffer      = work_buf;
    dec_cfg.advanced.working_buffer_size = JPEG_WORK_BUF_SIZE;

    esp_jpeg_image_output_t out_img = {0};
    esp_err_t err = esp_jpeg_decode(&dec_cfg, &out_img);

    free(work_buf);
    free(jpeg_buf);

    if (err == ESP_OK &&
        (int)out_img.width == LCD_WIDTH && (int)out_img.height == LCD_HEIGHT) {
        /* Store decoded blank frame as the "previous" for this tube */
        if (!s_flip_prev[tube])
            s_flip_prev[tube] = PSRAM_MALLOC(FLIP_FRAME_BYTES);
        if (s_flip_prev[tube])
            memcpy(s_flip_prev[tube], rgb_buf, FLIP_FRAME_BYTES);
        strncpy(s_flip_path[tube], blank_path, sizeof(s_flip_path[tube]) - 1);
        s_flip_path[tube][sizeof(s_flip_path[tube]) - 1] = '\0';
    }

    free(rgb_buf);
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

/* Legacy shim */
void display_show_time(int h, int m, int s, const char *theme)
{
    int digits[6] = {h/10, h%10, m/10, m%10, s/10, s%10};
    for (int i = 0; i < 6; i++) display_show_number(i, digits[i], theme);
}

/* ════════════════════════════════════════════════════════════════════
 *  Display task – full mode renderer
 * ════════════════════════════════════════════════════════════════════ */
#include "config_mgr.h"
#include "ntp_time.h"
#include "weather.h"
#include "youtube_bili.h"
#include "freertos/semphr.h"

/* Map OpenWeatherMap "main" condition string → asset filename */
__attribute__((unused)) static const char *weather_icon(const char *cond)
{
    if (!cond || !*cond) return "sun";
    if      (strcasecmp(cond, "Clear")       == 0) return "sun";
    else if (strcasecmp(cond, "Rain")        == 0) return "rain";
    else if (strcasecmp(cond, "Drizzle")     == 0) return "rain";
    else if (strcasecmp(cond, "Snow")        == 0) return "snow";
    else if (strcasecmp(cond, "Thunderstorm")== 0) return "thunderstorm";
    else if (strcasecmp(cond, "Fog")         == 0) return "fog";
    else if (strcasecmp(cond, "Mist")        == 0) return "fog";
    else if (strcasecmp(cond, "Haze")        == 0) return "fog";
    else if (strcasecmp(cond, "Clouds")      == 0) return "overcastClouds";
    else if (strcasecmp(cond, "Tornado")     == 0) return "tornado";
    else if (strcasecmp(cond, "Sand")        == 0) return "sand";
    else if (strcasecmp(cond, "Squall")      == 0) return "squalls";
    else if (strcasecmp(cond, "Ash")         == 0) return "volcanicAsh";
    return "sun";
}

/* ── Mode render helpers ────────────────────────────────────────────── */

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
    bool is_12h = (strcmp(cfg->time_type, "12H") == 0);
    int h = t->tm_hour, m = t->tm_min, s = t->tm_sec;
    char p[256];

    if (is_12h) {
        bool pm = (h >= 12);
        h = h % 12;
        if (h == 0) h = 12;
        /* tubes: H1  H2  colon  M1  M2  AM/PM  (no seconds in 12H) */
        if (h / 10 == 0)
            display_show_ampm  (0, "blank",          cfg->theme);
        else
            display_show_number(0, h / 10,           cfg->theme);
        display_show_number(1, h % 10,               cfg->theme);
        display_show_ampm  (2, "colon",              cfg->theme);
        display_show_number(3, m / 10,               cfg->theme);
        display_show_number(4, m % 10,               cfg->theme);
        display_show_ampm  (5, pm ? "pm" : "am",     cfg->theme);
    } else {
        /* 24H: all six tubes = H1 H2 M1 M2 S1 S2 */
        int d[6] = {h/10, h%10, m/10, m%10, s/10, s%10};
        for (int i = 0; i < 6; i++) display_show_number(i, d[i], cfg->theme);
    }
    (void)p;
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

/* Album: cycle through /images/album/ (jpg) files */
#include "dirent.h"
#define MAX_ALBUM      64
#define MAX_ALBUM_PATH 280   /* "/images/album/" (14) + d_name (255) + NUL */
static char s_album_files[MAX_ALBUM][MAX_ALBUM_PATH];
static int  s_album_count  = 0;
static int  s_album_index  = 0;
static bool s_album_loaded = false;

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
        /* Same image on all 6 tubes (each tube shows a crop / the full image). */
        for (int i = 0; i < LCD_COUNT; i++)
            display_show_image(i, s_album_files[s_album_index]);
        s_album_index = (s_album_index + 1) % s_album_count;
    }
}

/* ── Weather mode ───────────────────────────────────────────────────── */
/*
 * Layout: [TT][TT][unit][HH][HH][icon]
 *   Layout (see render_weather() for full table):
 *     positive              : [t/blank][units][C/F][h/blank][hum][icon]
 *     negative single-digit : [-][units][C/F][blank][blank][icon]
 *     negative double-digit : [-][tens][units][C/F][blank][icon]
 *   Leading zeros are blank.  Negative temps suppress humidity entirely.
 *   All blank slots use AMPM/blank.jpg for a theme-consistent appearance.
 *   Unit: Temperature/degreec.jpg or Temperature/degreef.jpg
 *   tube  5   : weather icon from MutiInfo/Weather/{icon}.jpg
 *
 * Actual SPIFFS filenames (must match exactly):
 *   Temperature/ : degreec  degreef  minus
 *   Weather/     : sun  fewClouds  overcastClouds  fog
 *                  rain  snow  squalls  thunderstorm
 *                  sand  tornado  volcanicAsh
 */
static void render_weather(const nextube_config_t *cfg)
{
    const weather_data_t *w = weather_get();
    char path[128];

    if (!w || !w->valid) {
        /* No weather data yet – show "-" on every tube so the user knows
         * weather mode is active but still waiting for the first fetch.
         * Once WiFi connects and the weather task fires (~10 s later) the
         * real data replaces these dashes automatically. */
        for (int i = 0; i < LCD_COUNT; i++) {
            display_path_temperature(path, sizeof(path), cfg->theme, "minus");
            display_show_image(i, path);
        }
        return;
    }

    /* Temperature in the configured unit */
    float temp_f = strncmp(cfg->temp_format, "Fahrenheit", 10) == 0
                       ? w->temp_c * 9.0f / 5.0f + 32.0f
                       : w->temp_c;
    bool negative = (temp_f < -0.5f);
    int  temp     = (int)(negative ? -temp_f + 0.5f : temp_f + 0.5f);
    if (temp > 99) temp = 99;

    int hum = (int)(w->humidity + 0.5f);
    if (hum < 0)  hum = 0;
    if (hum > 99) hum = 99;

    /* Tube layout:
     *
     *  Positive (0–99°): leading zero is BLANK, humidity leading zero is BLANK
     *    [t/blank][units][°C/F][h/blank][hum units][icon]
     *    e.g. 2°C 92%:   _  2  °C  9  2  ☁
     *    e.g. 15°C 8%:   1  5  °C  _  8  ☁
     *
     *  Negative single-digit (-1 to -9°): no humidity shown
     *    [-][units][°C/F][ _ ][ _ ][icon]
     *    e.g. -7°C:  -  7  °C  _  _  ☁
     *
     *  Negative double-digit (-10 to -99°): no humidity shown
     *    [-][tens][units][°C/F][ _ ][icon]
     *    e.g. -23°C:  -  2  3  °C  _  ☁
     *
     * Unit symbol: Temperature/degreec.jpg or Temperature/degreef.jpg
     */
    bool fahrenheit = (strncmp(cfg->temp_format, "Fahrenheit", 10) == 0);
    const char *unit = fahrenheit ? "degreef" : "degreec";
    const char *icon = (w->icon[0] != '\0') ? w->icon : "sun";

    /* Prime the flip animation cache for the degree-symbol tube so that the
     * animation transitions from blank.jpg, not from whatever the tube last
     * displayed (e.g. colon in clock mode).  No-op for non-FlipClock themes
     * or when the tube already holds a blank/degree image.
     * Negative double-digit puts the degree symbol on tube 3; all other
     * layouts put it on tube 2. */
    flip_prime_blank(negative && temp >= 10 ? 3 : 2, cfg->theme);

    if (!negative) {
        /* Tube 0: temperature tens — blank if zero (no leading zero) */
        if (temp / 10 == 0) {
            display_show_ampm(0, "blank", cfg->theme);
        } else {
            display_path_number(path, sizeof(path), cfg->theme, temp / 10);
            display_show_image(0, path);
        }
        /* Tube 1: temperature units */
        display_path_number(path, sizeof(path), cfg->theme, temp % 10);
        display_show_image(1, path);
        /* Tube 2: degree symbol (°C or °F) */
        display_path_temperature(path, sizeof(path), cfg->theme, unit);
        display_show_image(2, path);
        /* Tube 3: humidity tens — blank if zero */
        if (hum / 10 == 0) {
            display_show_ampm(3, "blank", cfg->theme);
        } else {
            display_path_number(path, sizeof(path), cfg->theme, hum / 10);
            display_show_image(3, path);
        }
        /* Tube 4: humidity units */
        display_path_number(path, sizeof(path), cfg->theme, hum % 10);
        display_show_image(4, path);
    } else if (temp <= 9) {
        /* Single-digit negative: [-][digit][°C/F][blank][blank][icon] */
        display_path_temperature(path, sizeof(path), cfg->theme, "minus");
        display_show_image(0, path);
        display_path_number(path, sizeof(path), cfg->theme, temp);
        display_show_image(1, path);
        display_path_temperature(path, sizeof(path), cfg->theme, unit);
        display_show_image(2, path);
        display_show_ampm(3, "blank", cfg->theme);
        display_show_ampm(4, "blank", cfg->theme);
    } else {
        /* Double-digit negative: [-][tens][units][°C/F][blank][icon] */
        display_path_temperature(path, sizeof(path), cfg->theme, "minus");
        display_show_image(0, path);
        display_path_number(path, sizeof(path), cfg->theme, temp / 10);
        display_show_image(1, path);
        display_path_number(path, sizeof(path), cfg->theme, temp % 10);
        display_show_image(2, path);
        display_path_temperature(path, sizeof(path), cfg->theme, unit);
        display_show_image(3, path);
        display_show_ampm(4, "blank", cfg->theme);
    }

    /* Tube 5: weather icon */
    display_path_weather(path, sizeof(path), cfg->theme, icon);
    display_show_image(5, path);
}

/* ── Timer state ────────────────────────────────────────────────────── */
static TickType_t s_timer_start   = 0;
static bool       s_pomo_in_break = false;
static SemaphoreHandle_t s_timer_mutex = NULL;

void display_timer_reset(void)
{
    if (s_timer_mutex) xSemaphoreTake(s_timer_mutex, portMAX_DELAY);
    s_timer_start   = xTaskGetTickCount();
    s_pomo_in_break = false;
    if (s_timer_mutex) xSemaphoreGive(s_timer_mutex);
}

/* ── Main display task ──────────────────────────────────────────────── */
static void display_task(void *arg)
{
    s_timer_mutex  = xSemaphoreCreateMutex();
    s_timer_start  = xTaskGetTickCount();

    /* Per-render state for change detection */
    struct tm     last_t        = {0};
    app_mode_t    last_mode     = (app_mode_t)-1;
    char          last_theme[32] = {0};
    uint32_t      last_subs     = UINT32_MAX;
    int32_t       last_remain_s = INT32_MAX;  /* countdown/pomodoro change detection */
    float         last_temp_c   = -9999.0f;   /* weather change detection */
    float         last_hum      = -1.0f;
    bool          last_wx_valid = false;       /* detect when data first arrives */
    bool          last_bl_on    = true;        /* backlight on/off tracking */
    uint8_t       last_bl_brt   = 255;         /* sentinel: force-apply on first tick */
    TickType_t    album_switch  = 0;
    TickType_t    rotation_tick = 0;           /* tick when current mode started */
    bool          first         = true;

    TickType_t wake = xTaskGetTickCount();
    rotation_tick = wake;

    while (1) {
        const nextube_config_t *cfg = config_get();
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

        if (mode_changed || theme_changed || first) {
            /* Reset album and timer state on mode/theme switch */
            s_album_loaded = false; s_album_index = 0; album_switch = 0;
            last_remain_s  = INT32_MAX;
            display_timer_reset();
        }

        /* Apply backlight on/off whenever the config changes.
         * This is the only place that translates cfg->backlight_on into
         * an actual LEDC duty cycle, so the middle touch button works. */
        if (first || cfg->backlight_on != last_bl_on ||
                     cfg->lcd_brightness != last_bl_brt) {
            display_set_brightness(cfg->backlight_on ? cfg->lcd_brightness : 0);
            last_bl_on  = cfg->backlight_on;
            last_bl_brt = cfg->lcd_brightness;
        }

        switch (mode) {

        case APP_MODE_CLOCK: {
            struct tm t; ntp_get_local(&t);
            if (first || mode_changed || theme_changed ||
                t.tm_sec  != last_t.tm_sec  ||
                t.tm_min  != last_t.tm_min  ||
                t.tm_hour != last_t.tm_hour) {
                render_clock(cfg, &t);
                last_t = t;
            }
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
            int32_t elapsed = (int32_t)pdTICKS_TO_MS(xTaskGetTickCount() - s_timer_start) / 1000;
            xSemaphoreGive(s_timer_mutex);
            int32_t total  = (int32_t)cfg->countdown_minutes * 60;
            int32_t remain = total - elapsed;
            if (remain < 0) remain = 0;
            if (first || mode_changed || theme_changed || remain != last_remain_s) {
                render_countdown_display(cfg, remain);
                last_remain_s = remain;
            }
            break;
        }

        case APP_MODE_POMODORO: {
            xSemaphoreTake(s_timer_mutex, portMAX_DELAY);
            int32_t elapsed = (int32_t)pdTICKS_TO_MS(xTaskGetTickCount() - s_timer_start) / 1000;
            bool in_break   = s_pomo_in_break;
            xSemaphoreGive(s_timer_mutex);

            int32_t period = in_break ? (int32_t)cfg->pomodoro_break * 60
                                      : (int32_t)cfg->pomodoro_work  * 60;
            int32_t remain = period - elapsed;
            if (remain <= 0) {
                /* Flip work/break */
                xSemaphoreTake(s_timer_mutex, portMAX_DELAY);
                s_pomo_in_break = !s_pomo_in_break;
                s_timer_start   = xTaskGetTickCount();
                in_break        = s_pomo_in_break;
                xSemaphoreGive(s_timer_mutex);
                remain = in_break ? (int32_t)cfg->pomodoro_break * 60
                                  : (int32_t)cfg->pomodoro_work  * 60;
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

        case APP_MODE_ALBUM:
            render_album(cfg, &album_switch, first || mode_changed || theme_changed);
            break;

        case APP_MODE_WEATHER: {
            const weather_data_t *w = weather_get();
            bool now_valid  = (w != NULL && w->valid);
            /* Trigger re-render when: first draw, mode/theme change, new data
             * values arrived, OR validity flips (e.g. data fetched for the
             * first time while already showing the blank loading screen). */
            bool wx_changed = now_valid && last_wx_valid &&
                              (w->temp_c != last_temp_c || w->humidity != last_hum);
            bool valid_changed = (now_valid != last_wx_valid);
            if (first || mode_changed || theme_changed || wx_changed || valid_changed) {
                render_weather(cfg);
                if (now_valid) { last_temp_c = w->temp_c; last_hum = w->humidity; }
                last_wx_valid = now_valid;
            }
            break;
        }

        default: break;
        }

        last_mode = mode;
        strncpy(last_theme, cfg->theme, sizeof(last_theme) - 1);
        first = false;

        vTaskDelayUntil(&wake, pdMS_TO_TICKS(200)); /* 5 Hz */
    }
}

void display_task_start(void)
{
    xTaskCreatePinnedToCore(display_task, "display", 8192, NULL, 6, NULL, 1);
    ESP_LOGI(TAG, "Display task started");
}
