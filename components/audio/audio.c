/**
 * @file audio.c
 * @brief Nextube audio driver – WAV file playback via DAC continuous driver.
 *
 * Hardware: GPIO25 → LTK8002D amplifier (DAC_CHAN_0).
 *
 * Uses the IDF 5.x dac_continuous driver (driver/dac_continuous.h).
 *
 * Supports standard PCM WAV files (8-bit or 16-bit, mono or stereo).
 * 16-bit signed samples are down-converted to 8-bit unsigned before writing
 * to the DAC (the DAC is 8-bit; the continuous driver always accepts uint8_t).
 *
 * Playback runs in a dedicated FreeRTOS task so audio_play_file() returns
 * immediately.  A mutex serialises concurrent play requests.
 *
 * DAC mode lifecycle:
 *   disabled – dac_oneshot holds GPIO25 at 128 (≈1.65 V DC, VDD/2).
 *             The AC coupling cap charges to this voltage; thereafter the
 *             amp sees 0 V AC differential → genuine silence.  No I²S,
 *             no DMA, no periodic AHB activity.
 *             Changing audio_enabled requires a reboot — the setting is
 *             saved to config.json before esp_restart() so it takes effect
 *             clean at next boot.  Mixing oneshot and dac_continuous in
 *             the same boot session is unreliable on original ESP32.
 *
 *   playing – leds_set_audio_active(true) pauses WS2812 RMT first.
 *             A flat-128 prime buffer fills the ring before playback so
 *             V_amp_in = 0 V from the first sample — no pop, no chirp.
 *             On exit the ring is flushed with 128 and LEDs resume.
 */

#include "audio.h"
#include "leds.h"
#include "board_pins.h"
#include "esp_log.h"
#include "driver/dac_continuous.h"
#include "driver/dac_oneshot.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <math.h>

static const char *TAG = "audio";

/* ── Runtime state ─────────────────────────────────────────────────── */
static int               s_volume      = 20;
static volatile bool     s_stop_flag   = false;
static TaskHandle_t      s_audio_task  = NULL;
static SemaphoreHandle_t s_play_mutex  = NULL;

/* DAC handle – running when audio is enabled, NULL when disabled / Hi-Z */
static dac_continuous_handle_t s_dac_cont        = NULL;
/* Oneshot handle – held open when audio is disabled at boot to keep GPIO25
 * at mid-rail (128 ≈ 1.65 V).  Never mixed with dac_continuous in the same
 * boot session (unreliable on original ESP32). */
static dac_oneshot_handle_t    s_dac_os_silence  = NULL;
static volatile bool           s_audio_enabled   = true;
/* Set while a DAC test mode is active — blocks audio_play_file(). */
static volatile bool           s_dac_test_active = false;

/* ── Buffer / DMA sizes ─────────────────────────────────────────────── */
#define FIXED_DAC_RATE     32000
#define STREAM_BUF_BYTES   4096   /* file read chunk; also 8-bit output buf */
#define DAC_DESC_NUM          8   /* DMA descriptor count                   */
#define DAC_DMA_BUF_SIZE   2048   /* bytes per DMA descriptor               */

/* ── WAV RIFF header (44 bytes, little-endian) ─────────────────────── */
typedef struct __attribute__((packed)) {
    char     riff_id[4];        /* "RIFF"             */
    uint32_t file_size;         /* total_size - 8     */
    char     wave_id[4];        /* "WAVE"             */
    char     fmt_id[4];         /* "fmt "             */
    uint32_t fmt_size;          /* 16 for PCM         */
    uint16_t audio_format;      /* 1 = PCM            */
    uint16_t num_channels;      /* 1 or 2             */
    uint32_t sample_rate;       /* e.g. 44100         */
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;   /* 8 or 16            */
} wav_riff_hdr_t;


/* ── DAC lifecycle ──────────────────────────────────────────────────── */

/*
 * Start (or restart) the continuous DAC from a powered-off / Hi-Z state.
 *
 * Called by audio_init() on boot and by audio_set_enabled(true) when the
 * user re-enables audio from the web UI.  The DMA ring is pre-filled with
 * flat 128 (VDD/2 = silence) immediately after enable so V_amp_in = VDD/2
 * from the first sample — no pop, no chirp.
 */
static void dac_restart(void)
{
    if (s_dac_cont) return;  /* already running */

    dac_continuous_config_t cfg = {
        .chan_mask = DAC_CHANNEL_MASK_CH0,
        .desc_num  = DAC_DESC_NUM,
        .buf_size  = DAC_DMA_BUF_SIZE,
        .freq_hz   = FIXED_DAC_RATE,
        .clk_src   = DAC_DIGI_CLK_SRC_DEFAULT,
        .chan_mode  = DAC_CHANNEL_MODE_SIMUL,
    };
    /* Retry up to 3 times with 100 ms gaps.
     * The I2S0 DMA controller can take >50 ms to fully release its internal
     * state after dac_continuous_del_channels(); without a gap,
     * new_channels() may return an error (I2S0 still occupied). */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3 && err != ESP_OK; attempt++) {
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(100));
        err = dac_continuous_new_channels(&cfg, &s_dac_cont);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "dac_restart: attempt %d failed: %s",
                     attempt + 1, esp_err_to_name(err));
            s_dac_cont = NULL;
        }
    }
    if (!s_dac_cont) {
        /* Rate-limit the all-attempts-failed log to once per minute.
         * Per-attempt warnings above (one ESP_LOGW per failed retry) still
         * fire on every call — those are the diagnostic signal.  This guard
         * just prevents a flood of identical ERRORs when audio_play_file()
         * is called repeatedly while the DAC remains unable to restart. */
        static int64_t s_last_err_us = 0;
        int64_t now = esp_timer_get_time();
        if (now - s_last_err_us > 60LL * 1000 * 1000) {
            ESP_LOGE(TAG, "dac_restart: all attempts failed — audio silenced");
            s_last_err_us = now;
        }
        return;
    }
    if (dac_continuous_enable(s_dac_cont) != ESP_OK) {
        ESP_LOGE(TAG, "dac_restart: enable failed");
        dac_continuous_del_channels(s_dac_cont);
        s_dac_cont = NULL;
        return;
    }

    /* Anti-Pop Boot Fade: 0 → 128 over 500 ms via cosine S-curve.
     * Gradually charges the AC cap so V_amp_in stays near 0 throughout. */
    size_t fade_samples = (FIXED_DAC_RATE * 500) / 1000;
    fade_samples = (fade_samples + 3) & ~3;
    uint8_t *boot_fade = (uint8_t *)calloc(1, fade_samples);
    if (boot_fade) {
        for (size_t i = 0; i < fade_samples; i++) {
            float t = (float)i / (float)fade_samples;
            boot_fade[i] = (uint8_t)(64.0f * (1.0f - cosf(t * (float)M_PI)));
        }
        size_t w;
        dac_continuous_write(s_dac_cont, boot_fade, fade_samples, &w, portMAX_DELAY);
        free(boot_fade);
    }

    /* Pre-fill the ring with silence so the DMA idles at mid-rail. */
    uint8_t silence[DAC_DMA_BUF_SIZE];
    memset(silence, 128, sizeof(silence));
    size_t w;
    for (int i = 0; i < DAC_DESC_NUM; i++)
        dac_continuous_write(s_dac_cont, silence, sizeof(silence), &w, portMAX_DELAY);

    ESP_LOGI(TAG, "DAC started (32 kHz continuous)");
}

/* ── Volume scaling ─────────────────────────────────────────────────── */
static void apply_volume(uint8_t *buf, int len_bytes,
                         uint16_t bits_per_sample, int vol_pct)
{
    if (vol_pct >= 100) return;
    const float scale = vol_pct / 100.0f;

    if (bits_per_sample == 16) {
        int16_t *s = (int16_t *)(void *)buf;
        int      n = len_bytes / 2;
        for (int i = 0; i < n; i++)
            s[i] = (int16_t)roundf((float)s[i] * scale);
    } else {
        for (int i = 0; i < len_bytes; i++)
            buf[i] = (uint8_t)(128 + (int)roundf(((int)buf[i] - 128) * scale));
    }
}

static int pcm16_to_pcm8(uint8_t *buf, int len_bytes)
{
    int16_t *s16    = (int16_t *)(void *)buf;
    int      samples = len_bytes / 2;
    for (int i = 0; i < samples; i++)
        buf[i] = (uint8_t)((s16[i] >> 8) + 128);
    return samples;
}

/* ── Playback task ──────────────────────────────────────────────────── */
typedef struct { char path[128]; } play_arg_t;

static void audio_play_task(void *arg)
{
    play_arg_t *a = (play_arg_t *)arg;
    char path[128];
    strncpy(path, a->path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    free(a);

    uint8_t *buf     = NULL;
    uint8_t *preload = NULL;
    size_t   preload_n = 0;
    uint32_t frame = 0, total_bytes_out = 0;

    FILE *f = fopen(path, "rb");
    if (!f) goto task_exit;

    wav_riff_hdr_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) < (int)sizeof(hdr)) goto task_close;
    if (memcmp(hdr.riff_id, "RIFF", 4) != 0 || memcmp(hdr.wave_id, "WAVE", 4) != 0) goto task_close;
    if (hdr.audio_format != 1) goto task_close;

    {
        long data_start = -1;
        fseek(f, 12, SEEK_SET);
        while (!feof(f)) {
            char     cid[4];
            uint32_t csz;
            if (fread(cid, 1, 4, f) < 4) break;
            if (fread(&csz, 1, 4, f) < 4) break;
            if (memcmp(cid, "data", 4) == 0) { data_start = ftell(f); break; }
            fseek(f, (long)(csz + (csz & 1)), SEEK_CUR);
        }
        if (data_start < 0) goto task_close;
        fseek(f, data_start, SEEK_SET);
    }

    /* ── Upsample factor for low sample-rate files ─────────────────────
     * ESP32 DAC DMA minimum rate ≈ 19 608 Hz (160 MHz / (255 × 32)).
     * 8 kHz and 16 kHz files are integer-upsampled to ≥ 20 kHz. */
    uint32_t upsample = 1;
    if (hdr.sample_rate > 0 && FIXED_DAC_RATE >= hdr.sample_rate) {
        upsample = FIXED_DAC_RATE / hdr.sample_rate;
    }
    if (upsample < 1) upsample = 1;

    /* ── DMA window: internal SRAM ── */
    buf = (uint8_t *)heap_caps_malloc(STREAM_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf) goto task_cleanup;

    /* ── PSRAM pre-buffer ───────────────────────────────────────────────
     * Load the entire WAV data chunk into PSRAM before starting the DAC.
     * Prevents SPIFFS cold-read stalls (can be 500+ ms) from draining the
     * DMA ring mid-playback and causing pops/static. */
#define PSRAM_PRELOAD_MAX  (256 * 1024)
    {
        long cur = ftell(f);
        fseek(f, 0, SEEK_END);
        long eof = ftell(f);
        fseek(f, cur, SEEK_SET);
        size_t raw_bytes = (eof > cur) ? (size_t)(eof - cur) : 0;

        if (raw_bytes > 0 && raw_bytes <= PSRAM_PRELOAD_MAX) {
            size_t post_conv  = (hdr.bits_per_sample == 16) ? raw_bytes/2 : raw_bytes;
            size_t expanded   = post_conv * upsample;
            size_t alloc_size = (raw_bytes > expanded) ? raw_bytes : expanded;

            preload = (uint8_t *)heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM);
            if (preload) {
                size_t got = fread(preload, 1, raw_bytes, f);
                apply_volume(preload, (int)got, hdr.bits_per_sample, s_volume);

                int out8 = (int)got;
                if (hdr.bits_per_sample == 16) out8 = pcm16_to_pcm8(preload, (int)got);

                if (upsample > 1) {
                    for (int i = out8 - 1; i >= 0; i--) {
                        uint8_t sv = preload[i];
                        for (uint32_t j = 0; j < upsample; j++)
                            preload[(uint32_t)i * upsample + j] = sv;
                    }
                    out8 *= (int)upsample;
                }
                preload_n = (size_t)out8;
            }
        }
    }

    /* ── Pause LED RMT before starting DAC ─────────────────────────────
     * WS2812 current spikes on the 3.3 V rail couple into the DAC output.
     * Pausing RMT stops all transmissions; LEDs hold their last colour. */
    leds_set_audio_active(true);

    /* Stream PCM to the perpetually running DMA */
    {
        if (preload) {
            /* PSRAM path */
            size_t pos = 0;
            while (!s_stop_flag && pos < preload_n) {
                size_t chunk = preload_n - pos;
                if (chunk > STREAM_BUF_BYTES) chunk = STREAM_BUF_BYTES;
                
                chunk &= ~3; 
                if (chunk == 0) break;

                memcpy(buf, preload + pos, chunk);
                pos += chunk;

                size_t written = 0;
                esp_err_t werr = dac_continuous_write(s_dac_cont, buf, chunk,
                                                      &written, pdMS_TO_TICKS(1000));
                if (werr != ESP_OK) break;
                total_bytes_out += (uint32_t)written;
                frame++;
            }
            free(preload);
            preload = NULL;
        } else {
            /* SPIFFS streaming fallback */
            const size_t read_size = STREAM_BUF_BYTES / upsample;
            while (!s_stop_flag) {
                int rd = (int)fread(buf, 1, read_size, f);
                if (rd <= 0) break;

                apply_volume(buf, rd, hdr.bits_per_sample, s_volume);

                int out_bytes = rd;
                if (hdr.bits_per_sample == 16) out_bytes = pcm16_to_pcm8(buf, rd);

                if (upsample > 1) {
                    for (int i = out_bytes - 1; i >= 0; i--) {
                        uint8_t sv = buf[i];
                        for (uint32_t j = 0; j < upsample; j++)
                            buf[(uint32_t)i * upsample + j] = sv;
                    }
                    out_bytes *= (int)upsample;
                }
                
                out_bytes &= ~3;
                if (out_bytes == 0) break;

                size_t written = 0;
                esp_err_t werr = dac_continuous_write(s_dac_cont, buf,
                                                      (size_t)out_bytes,
                                                      &written, pdMS_TO_TICKS(1000));
                if (werr != ESP_OK) break;
                total_bytes_out += (uint32_t)written;
                frame++;
            }
        }
    }

task_cleanup:
    if (preload) { free(preload); preload = NULL; }

    if (buf && s_dac_cont) {
        /* Flush ring with pure silence (128) to safely drain audio 
         * and leave the DMA perfectly resting at mid-rail. */
        memset(buf, 128, STREAM_BUF_BYTES);
        size_t w;
        for (int i = 0; i < DAC_DESC_NUM; i++) {
            dac_continuous_write(s_dac_cont, buf, DAC_DMA_BUF_SIZE, &w, pdMS_TO_TICKS(200));
        }
    }
    
    free(buf);
    leds_set_audio_active(false);

task_close:
    fclose(f);
task_exit:
    xSemaphoreGive(s_play_mutex);
    s_audio_task = NULL;
    vTaskDelete(NULL);
}

/* ════════════════════════════════════════════════════════════════════ */
/* Public API                                                          */
/* ════════════════════════════════════════════════════════════════════ */

void audio_init(bool enabled)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "Audio init – DAC GPIO%d  enabled=%d", PIN_AUDIO_DAC, (int)enabled);

    s_play_mutex = xSemaphoreCreateBinary();
    xSemaphoreGive(s_play_mutex);

    if (!enabled) {
        s_audio_enabled = false;
        /* Hold GPIO25 at mid-rail (128 ≈ 1.65 V DC) via dac_oneshot.
         *
         * The LTK8002D input is AC-coupled.  A stable VDD/2 DC level
         * charges the AC cap to VDD/2; thereafter the amp sees 0 V AC
         * differential — genuine silence.  dac_oneshot uses the RTC DAC
         * path: no I²S, no DMA, no periodic AHB bus activity.
         *
         * The handle is kept open for the life of the boot session so
         * GPIO25 stays in DAC mode.  Changing audio_enabled takes effect
         * after a reboot (the web server saves config then calls
         * esp_restart); we never transition oneshot → dac_continuous in
         * the same session — that path is unreliable on original ESP32. */
        dac_oneshot_config_t os_cfg = { .chan_id = DAC_CHAN_0 };
        if (dac_oneshot_new_channel(&os_cfg, &s_dac_os_silence) == ESP_OK) {
            dac_oneshot_output_voltage(s_dac_os_silence, 128);
            ESP_LOGI(TAG, "Audio disabled at init — GPIO%d held at 128 (~1.65 V) via oneshot",
                     PIN_AUDIO_DAC);
        } else {
            /* Fallback: drive GPIO25 LOW.  Less ideal (AC cap charges to 0 V
             * instead of VDD/2, coupling any supply ripple as AC to the amp)
             * but still much quieter than Hi-Z. */
            gpio_reset_pin(PIN_AUDIO_DAC);
            gpio_set_direction(PIN_AUDIO_DAC, GPIO_MODE_OUTPUT);
            gpio_set_level(PIN_AUDIO_DAC, 0);
            ESP_LOGW(TAG, "Audio disabled at init — oneshot failed, GPIO%d driven LOW",
                     PIN_AUDIO_DAC);
        }
        return;
    }

    /* Audio is enabled: bring up the DAC immediately so the APLL locks now
     * (during the deferred-start task, before any user action) rather than
     * introducing a ~1.6 s stall on the first audio_play_file() call. */
    dac_restart();
}

void audio_set_enabled(bool enabled)
{
    /* audio_enabled changes take effect after a reboot.  The web server
     * saves the new value to config.json then calls esp_restart(); on the
     * next boot audio_init() picks up the correct state.
     *
     * Dynamic switching is intentionally not supported: transitioning from
     * dac_oneshot (used when audio is disabled at boot) to dac_continuous
     * in the same boot session is unreliable on the original ESP32 — the
     * I²S0 controller sometimes fails to release state after oneshot use,
     * causing dac_continuous_new_channels() to return an error even after
     * repeated retries. */
    (void)enabled;
    ESP_LOGI(TAG, "audio_set_enabled: change saved — takes effect after reboot");
}

void audio_play_file(const char *path)
{
    if (!s_audio_enabled)  return;
    if (s_dac_test_active) return;   /* a DAC test owns the output — skip playback */
    if (!s_dac_cont)       return;   /* DAC not ready (e.g. after failed restart)   */
    if (!path || path[0] == '\0') return;

    const char *ext = strrchr(path, '.');
    if (!ext || strcasecmp(ext, ".wav") != 0) return;

    if (!s_play_mutex) return;

    if (s_audio_task == NULL && uxSemaphoreGetCount(s_play_mutex) == 0) {
        xSemaphoreGive(s_play_mutex);
    }

    if (xSemaphoreTake(s_play_mutex, 0) != pdTRUE) return;

    s_stop_flag = false;

    play_arg_t *a = (play_arg_t *)malloc(sizeof(play_arg_t));
    if (!a) { xSemaphoreGive(s_play_mutex); return; }
    strncpy(a->path, path, sizeof(a->path) - 1);
    a->path[sizeof(a->path) - 1] = '\0';

    if (xTaskCreate(audio_play_task, "audio_play", 16384, a, 5, &s_audio_task) != pdPASS) {
        free(a);
        xSemaphoreGive(s_play_mutex);
    }
}

void audio_set_volume(int vol)
{
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    s_volume = vol;
}

void audio_stop(void)
{
    s_stop_flag = true;
    for (int i = 0; i < 30 && s_audio_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ── DAC test API ────────────────────────────────────────────────────── */
/*
 * Design notes:
 *
 * "hiz"     : tear down dac_continuous → GPIO25 = Hi-Z input.
 *             The DAC output buffer is completely powered down.
 *
 * "silence" / "dc" : fresh dac_continuous ring pre-filled with a constant
 *   level (same approach as "tone").  dac_oneshot was previously used here
 *   but the dac_oneshot → dac_continuous transition is unreliable on the
 *   original ESP32: del_channel() leaves the RTC/DAC hardware in a state
 *   that blocks dac_continuous_new_channels() even with a 50 ms delay.
 *   A fresh empty ring has all descriptors immediately available, so
 *   filling with a constant level completes without blocking.
 *
 * "tone"    : fresh dac_continuous ring (empty at start).
 *   An empty ring makes all descriptors immediately available, so writes
 *   complete without blocking.  Phase-continuous sine fills all descriptors
 *   and the DMA loops them.
 *
 * "normal"  : tear down the test dac_continuous channel, then call
 *   dac_restart() to restore the idle silence channel.  dac_restart()
 *   retries up to 3 × 100 ms to handle I2S0 settling.
 */

/* Helper: tear down the active test dac_continuous channel. */
static void dac_test_teardown(void)
{
    if (s_dac_cont) {
        dac_continuous_disable(s_dac_cont);
        dac_continuous_del_channels(s_dac_cont);
        s_dac_cont = NULL;
    }
}

void audio_dac_test_set(const char *mode, int param_a, int param_b)
{
    if (!mode) return;

    /* Stop any active playback so we have exclusive DAC access. */
    if (s_audio_task) {
        s_stop_flag = true;
        for (int i = 0; i < 50 && s_audio_task != NULL; i++)
            vTaskDelay(pdMS_TO_TICKS(10));
        s_stop_flag = false;
    }

    /* ── "normal" — restore idle silence ────────────────────────────── */
    if (strcmp(mode, "normal") == 0) {
        if (!s_dac_test_active) {
            ESP_LOGI(TAG, "DAC test: already normal, no-op");
            return;
        }
        dac_test_teardown();
        s_dac_test_active = false;

        if (s_audio_enabled) {
            dac_restart();   /* retry loop in dac_restart() handles I2S0 settling */
            ESP_LOGI(TAG, "DAC test: normal idle restored");
        } else {
            /* Audio disabled: restart DAC then immediately fill with 0. */
            dac_restart();
            if (s_dac_cont) {
                uint8_t zero[DAC_DMA_BUF_SIZE];
                memset(zero, 0, sizeof(zero));
                size_t w;
                for (int i = 0; i < DAC_DESC_NUM; i++)
                    dac_continuous_write(s_dac_cont, zero, sizeof(zero), &w, pdMS_TO_TICKS(200));
            }
            ESP_LOGI(TAG, "DAC test: restored to level-0 idle (audio_enabled=false)");
        }
        return;
    }

    /* ── All other modes: tear down current driver first ────────────── */
    dac_test_teardown();

    /* ── "hiz" — power off DAC output buffer entirely (test/diagnostic only) */
    if (strcmp(mode, "hiz") == 0) {
        gpio_reset_pin(PIN_AUDIO_DAC);
        gpio_set_direction(PIN_AUDIO_DAC, GPIO_MODE_INPUT);
        s_dac_test_active = true;
        ESP_LOGI(TAG, "DAC test: Hi-Z (GPIO%d = input)", PIN_AUDIO_DAC);
        return;
    }

    /* ── "silence" / "dc" — fresh DMA ring pre-filled with a constant level */
    if (strcmp(mode, "silence") == 0 || strcmp(mode, "dc") == 0) {
        int level = (strcmp(mode, "dc") == 0) ? param_a : 128;
        if (level < 0)   level = 0;
        if (level > 255) level = 255;

        /* Use a fresh dac_continuous channel (same pattern as "tone").
         * A fresh empty ring has all 8 descriptors immediately available so
         * filling with a constant level completes without blocking. */
        dac_continuous_config_t dcfg = {
            .chan_mask = DAC_CHANNEL_MASK_CH0,
            .desc_num  = DAC_DESC_NUM,
            .buf_size  = DAC_DMA_BUF_SIZE,
            .freq_hz   = FIXED_DAC_RATE,
            .clk_src   = DAC_DIGI_CLK_SRC_DEFAULT,
            .chan_mode  = DAC_CHANNEL_MODE_SIMUL,
        };
        if (dac_continuous_new_channels(&dcfg, &s_dac_cont) != ESP_OK ||
            dac_continuous_enable(s_dac_cont) != ESP_OK) {
            ESP_LOGE(TAG, "DAC test: %s — DMA init failed", mode);
            if (s_dac_cont) { dac_continuous_del_channels(s_dac_cont); s_dac_cont = NULL; }
            return;
        }
        uint8_t *buf = (uint8_t *)malloc(DAC_DMA_BUF_SIZE);
        if (!buf) { ESP_LOGE(TAG, "DAC test: OOM"); return; }
        memset(buf, (uint8_t)level, DAC_DMA_BUF_SIZE);
        size_t w;
        for (int i = 0; i < DAC_DESC_NUM; i++)
            dac_continuous_write(s_dac_cont, buf, DAC_DMA_BUF_SIZE, &w, portMAX_DELAY);
        free(buf);
        s_dac_test_active = true;
        ESP_LOGI(TAG, "DAC test: %s level=%d (~%.0fmV)",
                 mode, level, level * 3300.0f / 255.0f);
        return;
    }

    /* ── "tone" — fresh DMA ring, filled immediately ─────────────────── */
    if (strcmp(mode, "tone") == 0) {
        int freq = (param_a > 0 && param_a <= 4000) ? param_a : 1000;
        int amp  = (param_b >= 0 && param_b <= 127) ? param_b : 64;

        dac_continuous_config_t dcfg = {
            .chan_mask = DAC_CHANNEL_MASK_CH0,
            .desc_num  = DAC_DESC_NUM,
            .buf_size  = DAC_DMA_BUF_SIZE,
            .freq_hz   = FIXED_DAC_RATE,
            .clk_src   = DAC_DIGI_CLK_SRC_DEFAULT,
            .chan_mode  = DAC_CHANNEL_MODE_SIMUL,
        };
        if (dac_continuous_new_channels(&dcfg, &s_dac_cont) != ESP_OK ||
            dac_continuous_enable(s_dac_cont) != ESP_OK) {
            ESP_LOGE(TAG, "DAC test: tone — DMA init failed");
            if (s_dac_cont) { dac_continuous_del_channels(s_dac_cont); s_dac_cont = NULL; }
            return;
        }

        uint8_t *buf = (uint8_t *)malloc(DAC_DMA_BUF_SIZE);
        if (!buf) { ESP_LOGE(TAG, "DAC test: tone OOM"); return; }
        size_t w;
        for (int d = 0; d < DAC_DESC_NUM; d++) {
            int base = d * DAC_DMA_BUF_SIZE;
            for (int i = 0; i < DAC_DMA_BUF_SIZE; i++) {
                float ph = 2.0f * (float)M_PI * (float)freq
                           * (float)(base + i) / (float)FIXED_DAC_RATE;
                buf[i] = (uint8_t)(128 + (int)(sinf(ph) * (float)amp));
            }
            dac_continuous_write(s_dac_cont, buf, DAC_DMA_BUF_SIZE, &w, portMAX_DELAY);
        }
        free(buf);
        s_dac_test_active = true;
        ESP_LOGI(TAG, "DAC test: tone %d Hz amp=%d", freq, amp);
        return;
    }

    ESP_LOGW(TAG, "DAC test: unknown mode '%s'", mode);
}

void audio_dac_test_stop(void)
{
    audio_dac_test_set("normal", 0, 0);
}
