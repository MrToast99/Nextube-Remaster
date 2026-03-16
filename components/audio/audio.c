/**
 * @file audio.c
 * @brief Nextube audio driver – WAV file playback via DAC continuous driver.
 *
 * Hardware: GPIO25 → LTK8002D amplifier (DAC_CHAN_0).
 *
 * Uses the IDF 5.x dac_continuous driver (driver/dac_continuous.h) to replace
 * the legacy I2S-built-in-DAC path (driver/i2s.h + driver/dac.h).
 *
 * Supports standard PCM WAV files (8-bit or 16-bit, mono or stereo).
 * 16-bit signed samples are down-converted to 8-bit unsigned before writing
 * to the DAC (the DAC is 8-bit; the continuous driver always accepts uint8_t).
 * MP3 files are logged but skipped – the DAC requires raw PCM.
 *
 * Playback runs in a dedicated FreeRTOS task so audio_play_file() returns
 * immediately.  A mutex serialises concurrent play requests.
 *
 * DAC mode lifecycle:
 *   idle    – oneshot channel holds DAC at 128 (mid-rail = silence)
 *   playing – oneshot deleted, continuous channel streams PCM samples;
 *             on completion oneshot is re-created and restored to 128.
 */

#include "audio.h"
#include "board_pins.h"
#include "esp_log.h"
#include "driver/dac_continuous.h"
#include "driver/dac_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>    /* strcasecmp */
#include "esp_heap_caps.h"
#include "esp_timer.h"

static const char *TAG = "audio";

/* ── Runtime state ─────────────────────────────────────────────────── */
static int               s_volume      = 20;
static volatile bool     s_stop_flag   = false;
static TaskHandle_t      s_audio_task  = NULL;
static SemaphoreHandle_t s_play_mutex  = NULL;

/* DAC handles – only one is active at a time */
static dac_continuous_handle_t s_dac_cont = NULL;   /* during playback  */
static dac_oneshot_handle_t    s_dac_one  = NULL;   /* during idle      */

/* ── Buffer / DMA sizes ─────────────────────────────────────────────── */
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

/* ── DAC lifecycle helpers ──────────────────────────────────────────── */

/*
 * Transition from idle (oneshot) to continuous mode for streaming.
 * Oneshot and continuous channels cannot coexist on the same DAC output.
 */
static esp_err_t dac_cont_start(uint32_t sample_rate)
{
    /* Release oneshot so continuous can claim the channel */
    if (s_dac_one) {
        dac_oneshot_del_channel(s_dac_one);
        s_dac_one = NULL;
    }

    dac_continuous_config_t cfg = {
        .chan_mask = DAC_CHANNEL_MASK_CH0,
        .desc_num  = DAC_DESC_NUM,
        .buf_size  = DAC_DMA_BUF_SIZE,
        .freq_hz   = sample_rate,
        .clk_src   = DAC_DIGI_CLK_SRC_DEFAULT,
        .chan_mode  = DAC_CHANNEL_MODE_SIMUL,
    };
    esp_err_t err = dac_continuous_new_channels(&cfg, &s_dac_cont);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dac_continuous_new_channels: %s", esp_err_to_name(err));
        /* Reclaim oneshot so idle silence is restored */
        dac_oneshot_config_t one_cfg = { .chan_id = DAC_CHAN_0 };
        if (dac_oneshot_new_channel(&one_cfg, &s_dac_one) == ESP_OK)
            dac_oneshot_output_voltage(s_dac_one, 128);
        return err;
    }

    err = dac_continuous_enable(s_dac_cont);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dac_continuous_enable: %s", esp_err_to_name(err));
        dac_continuous_del_channels(s_dac_cont);
        s_dac_cont = NULL;
        dac_oneshot_config_t one_cfg = { .chan_id = DAC_CHAN_0 };
        if (dac_oneshot_new_channel(&one_cfg, &s_dac_one) == ESP_OK)
            dac_oneshot_output_voltage(s_dac_one, 128);
    }
    return err;
}

/*
 * Transition from continuous mode back to idle (oneshot), leaving DAC
 * at mid-rail (128) to prevent audible clicks or pops.
 */
static void dac_cont_stop(void)
{
    if (s_dac_cont) {
        dac_continuous_disable(s_dac_cont);
        dac_continuous_del_channels(s_dac_cont);
        s_dac_cont = NULL;
    }

    /* Restore oneshot and set DAC to silence */
    if (!s_dac_one) {
        dac_oneshot_config_t one_cfg = { .chan_id = DAC_CHAN_0 };
        if (dac_oneshot_new_channel(&one_cfg, &s_dac_one) == ESP_OK)
            dac_oneshot_output_voltage(s_dac_one, 128);
    }
}

/* ── Volume scaling ─────────────────────────────────────────────────── */
/*
 * In-place volume attenuation applied before bit-depth conversion.
 * 16-bit: samples are signed (centre = 0).
 * 8-bit:  samples are unsigned (centre = 128).
 */
static void apply_volume(uint8_t *buf, int len_bytes,
                         uint16_t bits_per_sample, int vol_pct)
{
    if (vol_pct >= 100) return;
    const float scale = vol_pct / 100.0f;

    if (bits_per_sample == 16) {
        int16_t *s = (int16_t *)(void *)buf;
        int      n = len_bytes / 2;
        for (int i = 0; i < n; i++)
            s[i] = (int16_t)((float)s[i] * scale);
    } else {
        for (int i = 0; i < len_bytes; i++)
            buf[i] = (uint8_t)(128 + (int)(((int)buf[i] - 128) * scale));
    }
}

/*
 * Convert 16-bit signed PCM to 8-bit unsigned PCM in-place.
 * Works by taking the high byte and re-centering: val = (s16 >> 8) + 128.
 * Returns the number of 8-bit output bytes produced (= len_bytes / 2).
 */
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

    ESP_LOGI(TAG, "task start: internal_free=%u  total_free=%u  stack_hwm=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)esp_get_free_heap_size(),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    uint8_t *buf     = NULL;   /* internal-SRAM DMA window               */
    uint8_t *pcm_buf = NULL;   /* PSRAM pre-buffer (whole file, optional) */

    /* ── Open file ── */
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open audio file: %s", path);
        goto task_exit;
    }

    /* ── Parse RIFF/WAVE header ── */
    wav_riff_hdr_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) < (int)sizeof(hdr)) {
        ESP_LOGE(TAG, "Short read on WAV header: %s", path);
        goto task_close;
    }
    if (memcmp(hdr.riff_id, "RIFF", 4) != 0 ||
        memcmp(hdr.wave_id, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Not a WAV file: %s", path);
        goto task_close;
    }
    if (hdr.audio_format != 1) {
        ESP_LOGE(TAG, "Only PCM WAV supported (format=%u)", hdr.audio_format);
        goto task_close;
    }

    /* ── Find the 'data' sub-chunk (skip 'LIST', 'fact', etc.) ── */
    {
        long data_start = -1;
        fseek(f, 12, SEEK_SET);   /* skip RIFF/WAVE preamble */
        while (!feof(f)) {
            char     cid[4];
            uint32_t csz;
            if (fread(cid, 1, 4, f) < 4) break;
            if (fread(&csz, 1, 4, f) < 4) break;
            if (memcmp(cid, "data", 4) == 0) {
                data_start = ftell(f);
                break;
            }
            fseek(f, (long)(csz + (csz & 1)), SEEK_CUR);
        }
        if (data_start < 0) {
            ESP_LOGE(TAG, "No 'data' chunk in: %s", path);
            goto task_close;
        }
        fseek(f, data_start, SEEK_SET);
    }

    ESP_LOGI(TAG, "WAV play: %s  %u Hz  %u ch  %u-bit  vol=%d%%",
             path, (unsigned)hdr.sample_rate, hdr.num_channels,
             hdr.bits_per_sample, s_volume);

    /* ── Pre-buffer entire PCM payload into PSRAM ─────────────────────
     * SPIFFS fread latency can be 100–600 ms per 4 KB chunk, which starves
     * the DAC and causes audible glitches.  Reading the whole file in one
     * go amortises that cost.  CPU reads from PSRAM are fine; only DMA
     * from PSRAM is forbidden (see DMA window below).
     *
     * Fallback: if PSRAM is unavailable or the file is too large, we fall
     * back to the original chunked-fread path. */

    /* Determine remaining PCM byte count */
    long pcm_start = ftell(f);
    fseek(f, 0, SEEK_END);
    long pcm_end   = ftell(f);
    fseek(f, pcm_start, SEEK_SET);
    size_t pcm_size = (size_t)(pcm_end - pcm_start);

    if (pcm_size > 0) {
        pcm_buf = (uint8_t *)heap_caps_malloc(pcm_size, MALLOC_CAP_SPIRAM);
        if (pcm_buf) {
            int64_t t_load = esp_timer_get_time();
            size_t got = fread(pcm_buf, 1, pcm_size, f);
            int64_t load_ms = (esp_timer_get_time() - t_load) / 1000;
            ESP_LOGI(TAG, "pre-buffered %u bytes into PSRAM @%p in %lld ms",
                     (unsigned)got, pcm_buf, (long long)load_ms);
            pcm_size = got;   /* actual bytes read */
        } else {
            ESP_LOGW(TAG, "PSRAM alloc failed for %u bytes — using chunked fread",
                     (unsigned)pcm_size);
        }
    }

    /* ── Small DMA window in internal SRAM ────────────────────────────
     * The DAC DMA controller cannot access PSRAM.  We copy STREAM_BUF_BYTES
     * at a time from the PSRAM pre-buffer into this internal-SRAM window
     * before handing it to dac_continuous_write. */
    buf = (uint8_t *)heap_caps_malloc(STREAM_BUF_BYTES,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "OOM for DMA window (internal SRAM)");
        goto task_cleanup;
    }
    ESP_LOGI(TAG, "DMA window @%p (internal SRAM)", buf);

    /* ── Start DAC continuous at the file's sample rate ── */
    if (dac_cont_start(hdr.sample_rate) != ESP_OK)
        goto task_cleanup;   /* buf and pcm_buf allocated; task_cleanup frees both */

    /* ── Prime DMA ring buffer with silence ────────────────────────────
     * When dac_continuous_enable() fires the DMA descriptor buffers are
     * zeroed (0x00 = DAC at ground = full negative rail).  The sudden
     * step from the oneshot idle level (128 = mid-rail) down to 0 is
     * amplified and heard as a pop at the very start of every sound.
     *
     * Pre-filling the entire ring with 0x80 (128 = mid-rail = silence)
     * before the first real PCM write eliminates that transient.
     *
     * Ring size = DAC_DESC_NUM × DAC_DMA_BUF_SIZE = 8 × 2 048 = 16 384 B.
     * STREAM_BUF_BYTES = 4 096, so 4 writes saturate the ring exactly. */
    memset(buf, 128, STREAM_BUF_BYTES);
    {
        size_t _w;
        for (int i = 0; i < (DAC_DESC_NUM * DAC_DMA_BUF_SIZE) / STREAM_BUF_BYTES; i++)
            dac_continuous_write(s_dac_cont, buf, STREAM_BUF_BYTES, &_w, pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "DMA ring primed with silence (%d × %d bytes)",
                 (DAC_DESC_NUM * DAC_DMA_BUF_SIZE) / STREAM_BUF_BYTES, STREAM_BUF_BYTES);
    }

    /* ── Stream PCM data ── */
    {
        uint32_t frame = 0, write_stalls = 0, total_bytes_out = 0;
        int64_t  t_start = esp_timer_get_time();

        size_t psram_pos = 0;   /* read cursor into PSRAM pre-buffer */

        while (!s_stop_flag) {
            int rd;

            if (pcm_buf) {
                /* Fast path: copy from PSRAM pre-buffer into DMA window */
                size_t remaining = pcm_size - psram_pos;
                if (remaining == 0) break;
                rd = (int)(remaining < (size_t)STREAM_BUF_BYTES
                           ? remaining : (size_t)STREAM_BUF_BYTES);
                memcpy(buf, pcm_buf + psram_pos, (size_t)rd);
                psram_pos += (size_t)rd;
            } else {
                /* Fallback path: chunked fread (original behaviour) */
                int64_t t_rd = esp_timer_get_time();
                rd = fread(buf, 1, STREAM_BUF_BYTES, f);
                int64_t rd_us = esp_timer_get_time() - t_rd;
                if (rd <= 0) break;
                if (rd_us > 50000)
                    ESP_LOGW(TAG, "fread slow: %lld ms (frame %u)",
                             (long long)(rd_us / 1000), frame);
            }

            if (rd <= 0) break;

            /* Volume attenuation (operates on native bit depth) */
            apply_volume(buf, rd, hdr.bits_per_sample, s_volume);

            /* DAC continuous only accepts 8-bit unsigned PCM */
            int out_bytes = rd;
            if (hdr.bits_per_sample == 16)
                out_bytes = pcm16_to_pcm8(buf, rd);

            size_t written = 0;
            int64_t t_wr = esp_timer_get_time();
            esp_err_t werr = dac_continuous_write(s_dac_cont, buf, (size_t)out_bytes,
                                                  &written, pdMS_TO_TICKS(1000));
            int64_t wr_us = esp_timer_get_time() - t_wr;

            if (wr_us > 50000) {
                write_stalls++;
                ESP_LOGW(TAG, "DAC write stall: %lld ms (frame %u, written=%u/%d)",
                         (long long)(wr_us / 1000), frame,
                         (unsigned)written, out_bytes);
            }
            if (werr != ESP_OK) {
                ESP_LOGW(TAG, "DAC write error %s — stopping playback",
                         esp_err_to_name(werr));
                break;
            }
            total_bytes_out += (uint32_t)written;
            frame++;
        }

        int64_t elapsed_ms = (esp_timer_get_time() - t_start) / 1000;
        ESP_LOGI(TAG, "playback done: %u frames  %u bytes  %lld ms  %u stalls",
                 frame, total_bytes_out, (long long)elapsed_ms, write_stalls);
    }

task_cleanup:
    heap_caps_free(pcm_buf);   /* NULL-safe; frees PSRAM pre-buffer if used */
    free(buf);
    dac_cont_stop();   /* disable continuous, restore oneshot silence */

task_close:
    fclose(f);
task_exit:
    ESP_LOGI(TAG, "task exit: stack_hwm=%u  internal_free=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    xSemaphoreGive(s_play_mutex);
    s_audio_task = NULL;
    vTaskDelete(NULL);
}

/* ════════════════════════════════════════════════════════════════════ */
/*  Public API                                                          */
/* ════════════════════════════════════════════════════════════════════ */

void audio_init(void)
{
    /* Pin this component's log level to INFO at runtime.
     * Prevents a global esp_log_level_set("*", WARN) elsewhere from
     * silencing the diagnostic logs in audio_play_task. */
    esp_log_level_set(TAG, ESP_LOG_INFO);

    ESP_LOGI(TAG, "Audio init – DAC GPIO%d", PIN_AUDIO_DAC);

    /* Start in oneshot mode so we can set an idle DC level (silence) */
    dac_oneshot_config_t one_cfg = { .chan_id = DAC_CHAN_0 };
    ESP_ERROR_CHECK(dac_oneshot_new_channel(&one_cfg, &s_dac_one));
    /* Ramp 0 → 128 over ~32 ms to suppress the LTK8002D power-on pop.
     * A hard step from the DAC's undefined power-on state to mid-rail
     * is amplified and audible as a click; a slow ramp is inaudible. */
    for (int v = 0; v <= 128; v += 4) {
        dac_oneshot_output_voltage(s_dac_one, (uint8_t)v);
        vTaskDelay(1);   /* 1 tick ≈ 1 ms → 32 steps × 1 ms ≈ 32 ms total */
    }
    dac_oneshot_output_voltage(s_dac_one, 128);   /* settle at mid-rail = silence */

    s_play_mutex = xSemaphoreCreateMutex();
}

void audio_play_file(const char *path)
{
    if (!path || path[0] == '\0') return;

    /* Only PCM WAV is supported via the built-in DAC */
    const char *ext = strrchr(path, '.');
    if (!ext || strcasecmp(ext, ".wav") != 0) {
        ESP_LOGW(TAG, "Skipping non-WAV file (DAC only supports PCM WAV): %s",
                 path);
        return;
    }

    /* Stop any running playback first */
    audio_stop();

    /* Acquire play lock (playback task releases it on exit) */
    if (xSemaphoreTake(s_play_mutex, pdMS_TO_TICKS(300)) != pdTRUE) {
        ESP_LOGW(TAG, "audio busy – play request dropped");
        return;
    }

    s_stop_flag = false;

    play_arg_t *a = (play_arg_t *)malloc(sizeof(play_arg_t));
    if (!a) {
        xSemaphoreGive(s_play_mutex);
        return;
    }
    strncpy(a->path, path, sizeof(a->path) - 1);
    a->path[sizeof(a->path) - 1] = '\0';

    BaseType_t rc = xTaskCreate(audio_play_task, "audio_play",
                                16384, a, 5, &s_audio_task);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio_play task");
        free(a);
        xSemaphoreGive(s_play_mutex);
    }
}

void audio_set_volume(int vol)
{
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    s_volume = vol;
    ESP_LOGD(TAG, "Volume set to %d%%", s_volume);
}

void audio_stop(void)
{
    s_stop_flag = true;
    /* Poll briefly for playback task to finish (it gives the mutex on exit) */
    for (int i = 0; i < 30 && s_audio_task != NULL; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    /* Guarantee silence on DAC output */
    if (s_dac_one)
        dac_oneshot_output_voltage(s_dac_one, 128);
}
