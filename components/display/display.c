#include "display.h"
#include "board_pins.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

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

static void st7735_init_one(int tube)
{
    select_tube(tube);
    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(0x3A); lcd_data_byte(0x05);
    lcd_cmd(0x36); lcd_data_byte(0x08);
    uint8_t fr[] = {0x01, 0x2C, 0x2D};
    lcd_cmd(0xB1); lcd_data(fr, 3);
    lcd_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(50));
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
        .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tmr);
    ledc_channel_config_t ch = {
        .gpio_num = PIN_LCD_BACKLIGHT, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 128,
    };
    ledc_channel_config(&ch);

    for (int i = 0; i < LCD_COUNT; i++) { st7735_init_one(i); display_fill(i, 0x0000); }
    ESP_LOGI(TAG, "Displays ready");
}

void display_set_brightness(uint8_t pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (pct * 255) / 100);
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
        spi_device_polling_transmit(spi_dev, &t);
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
    spi_transaction_t t = { .length = w*h*2*8, .tx_buffer = data };
    spi_device_polling_transmit(spi_dev, &t);
    deselect_all();
}

/* ════════════════════════════════════════════════════════════════════
 *  JPEG asset loader
 * ════════════════════════════════════════════════════════════════════ */
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

    /* ── 3. Push RGB565 frame to LCD ────────────────────────────── */
    display_show_digit(tube, rgb_buf, (int)out_img.width, (int)out_img.height);
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

static void render_clock(const nextube_config_t *cfg, const struct tm *t)
{
    bool is_12h = (strcmp(cfg->time_type, "12H") == 0);
    int h = t->tm_hour, m = t->tm_min, s = t->tm_sec;
    char p[256];

    if (is_12h) {
        bool pm = (h >= 12);
        h = h % 12;
        if (h == 0) h = 12;
        /* tubes 0-4: H1 H2 M1 M2 S1   tube 5: am/pm icon */
        int d[5] = {h/10, h%10, m/10, m%10, s/10};
        for (int i = 0; i < 5; i++) {
            /* leading zero on hour suppressed → show blank */
            if (i == 0 && d[0] == 0)
                display_show_ampm(0, "blank", cfg->theme);
            else
                display_show_number(i, d[i], cfg->theme);
        }
        display_show_ampm(5, pm ? "pm" : "am", cfg->theme);
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
    display_show_number(1, (value / 10000) % 10, theme);
    display_show_number(2, (value /  1000) % 10, theme);
    display_show_number(3, (value /   100) % 10, theme);
    display_show_number(4, (value /    10) % 10, theme);
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
        /* tube 0 = youtube icon, tubes 1-5 = 5-digit count */
        display_show_ampm(0, "youtube", cfg->theme);
        for (int i = 0; i < 5; i++)
            display_show_number(i+1, (count / (uint32_t[]){10000,1000,100,10,1}[i]) % 10, cfg->theme);
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
    struct tm     last_t       = {0};
    app_mode_t    last_mode    = (app_mode_t)-1;
    char          last_theme[32] = {0};
    uint32_t      last_subs    = UINT32_MAX;
    TickType_t    album_switch = 0;
    bool          first        = true;

    TickType_t wake = xTaskGetTickCount();

    while (1) {
        const nextube_config_t *cfg = config_get();
        app_mode_t mode = cfg->current_mode;
        bool mode_changed  = (mode != last_mode);
        bool theme_changed = (strcmp(cfg->theme, last_theme) != 0);

        if (mode_changed || theme_changed || first) {
            /* Reset album on mode/theme switch */
            s_album_loaded = false; s_album_index = 0; album_switch = 0;
            display_timer_reset();
        }

        switch (mode) {

        case APP_MODE_CLOCK:
        case APP_MODE_CUSTOM_CLOCK: {
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

        case APP_MODE_COUNTDOWN: {
            xSemaphoreTake(s_timer_mutex, portMAX_DELAY);
            int32_t elapsed = (int32_t)pdTICKS_TO_MS(xTaskGetTickCount() - s_timer_start) / 1000;
            xSemaphoreGive(s_timer_mutex);
            int32_t total   = (int32_t)cfg->countdown_minutes * 60;
            int32_t remain  = total - elapsed;
            render_countdown_display(cfg, remain);
            break;
        }

        case APP_MODE_POMODORO: {
            xSemaphoreTake(s_timer_mutex, portMAX_DELAY);
            int32_t elapsed = (int32_t)pdTICKS_TO_MS(xTaskGetTickCount() - s_timer_start) / 1000;
            bool in_break   = s_pomo_in_break;
            xSemaphoreGive(s_timer_mutex);

            int32_t period  = in_break ? (int32_t)cfg->pomodoro_break * 60
                                       : (int32_t)cfg->pomodoro_work  * 60;
            int32_t remain  = period - elapsed;
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
            render_pomodoro_display(cfg, remain, in_break);
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
