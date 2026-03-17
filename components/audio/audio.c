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
#include "esp_rom_sys.h"    /* esp_rom_delay_us – sub-tick busy-wait for DAC ramps */

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
    /* Ramp DAC from mid-rail (128) down to 0 before releasing the oneshot.
     * When dac_continuous_enable() fires, the DMA ring is zero-filled (0x00).
     * A hard step from 128→0 is amplified as an audible pop.  A short ramp
     * (9 steps × 2 ms = 18 ms) brings the output to 0 first so the
     * oneshot→continuous transition is 0→0 — inaudible.
     *
     * NOTE: vTaskDelay(pdMS_TO_TICKS(1)) must NOT be used here.  With the
     * default ESP-IDF FreeRTOS tick rate of 100 Hz, pdMS_TO_TICKS(1) = 0
     * (integer truncation), so the delay would be 0 ticks = no delay at
     * all, making the ramp an instantaneous hard step.  esp_rom_delay_us is
     * a busy-wait that bypasses tick granularity and gives accurate ms-range
     * delays. */
    if (s_dac_one) {
        for (int v = 128; v >= 0; v -= 16) {
            dac_oneshot_output_voltage(s_dac_one, (uint8_t)v);
            esp_rom_delay_us(2000);   /* 2 ms per step → 18 ms total ramp */
        }
        dac_oneshot_del_channel(s_dac_one);
        s_dac_one = NULL;
    }

    /* Guard: clean up any orphaned continuous handle left by a previous
     * task that was killed (e.g. watchdog) before reaching dac_cont_stop().
     * Without this, dac_continuous_new_channels() returns an error on every
     * subsequent call and audio never works again until reboot. */
    if (s_dac_cont) {
        ESP_LOGW(TAG, "Orphaned dac_cont handle detected — cleaning up before restart");
        dac_continuous_disable(s_dac_cont);
        dac_continuous_del_channels(s_dac_cont);
        s_dac_cont = NULL;
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
 *
 * dac_continuous_del_channels() disables digi-bypass; the DAC immediately
 * reverts from the last DMA output value to the RTC register value, which
 * was last written to 0 by the start-of-playback ramp-down.  To make this
 * a 0→0 (silent) switch rather than a 128→0 (pop) switch, task_cleanup
 * must end the DMA ring at 0 before calling here.  This function then
 * ramps 0→128 in oneshot to restore mid-rail idle. */
static void dac_cont_stop(void)
{
    if (s_dac_cont) {
        dac_continuous_disable(s_dac_cont);
        dac_continuous_del_channels(s_dac_cont);
        s_dac_cont = NULL;
    }

    /* Restore oneshot and ramp 0 → 128.
     * After del_channels the DAC holds the RTC register value (0).
     * A hard jump to 128 would cause a pop; ramp instead.
     * Same busy-wait rationale as the ramp-down in dac_cont_start:
     * pdMS_TO_TICKS(1) = 0 at 100 Hz tick rate → use esp_rom_delay_us. */
    if (!s_dac_one) {
        dac_oneshot_config_t one_cfg = { .chan_id = DAC_CHAN_0 };
        if (dac_oneshot_new_channel(&one_cfg, &s_dac_one) == ESP_OK) {
            for (int v = 0; v <= 128; v += 16) {
                dac_oneshot_output_voltage(s_dac_one, (uint8_t)v);
                esp_rom_delay_us(2000);   /* 2 ms per step → 18 ms total ramp */
            }
            dac_oneshot_output_voltage(s_dac_one, 128);   /* settle at silence */
        }
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

    uint8_t *buf = NULL;   /* internal-SRAM DMA window */
    /* Declared here (not inside the streaming block) so task_cleanup can
     * read total_bytes_out regardless of which goto reaches it. */
    uint32_t frame = 0, write_stalls = 0, total_bytes_out = 0;

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

    /* ── Upsample factor for low sample-rate files ────────────────────
     * The ESP32 DAC DMA uses I2S0 internally.  The I2S clock source is
     * D2PLL (160 MHz), with a 32× multiplier (2 ch × 16-bit slots).
     * mclk_div = 160 MHz / (sample_rate × 32); the register is 8-bit (max 255).
     * Minimum rate = 160 000 000 / (255 × 32) ≈ 19 608 Hz.
     * 8 000 Hz (divider 625) and 16 000 Hz (divider 312) both exceed 255 and
     * return ESP_ERR_INVALID_ARG from dac_continuous_new_channels().
     * Fix: integer upsample until dac_rate ≥ 20 000 Hz. */
    uint32_t upsample = 1;
    uint32_t dac_rate = hdr.sample_rate;
    while (dac_rate < 20000) { upsample <<= 1; dac_rate <<= 1; }
    if (upsample > 1)
        ESP_LOGI(TAG, "Upsampling x%u: %u Hz → %u Hz (DAC min ≈19608 Hz)",
                 (unsigned)upsample, (unsigned)hdr.sample_rate, (unsigned)dac_rate);

    /* ── DMA window in internal SRAM ──────────────────────────────────
     * The DAC DMA controller cannot access PSRAM.  All PCM is streamed
     * directly from SPIFFS into this internal-SRAM window (no pre-buffer).
     * 8-bit mono files at 8 000 Hz are small (≤ ~32 KB) so SPIFFS read
     * latency is negligible; fread() timing is logged for diagnostics. */
    buf = (uint8_t *)heap_caps_malloc(STREAM_BUF_BYTES,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "OOM for DMA window (internal SRAM)");
        goto task_cleanup;
    }
    ESP_LOGI(TAG, "DMA window @%p (internal SRAM)", buf);

    /* ── Start DAC continuous at the (possibly upsampled) rate ── */
    if (dac_cont_start(dac_rate) != ESP_OK)
        goto task_cleanup;   /* buf allocated; task_cleanup frees it */

    /* ── Prime DMA ring with a short silence burst ─────────────────────
     * dac_continuous_enable() zeros the DMA descriptors (0x00 = full
     * negative rail).  A step from oneshot idle (128 = mid-rail) down to
     * 0x00 is amplified as a pop.  Writing ~100 ms of 0x80 before the
     * first PCM data smooths the transition without causing a noticeable
     * delay.
     *
     * Priming = sample_rate / 10 bytes (100 ms at any sample rate).
     * At 8 000 Hz this is 800 bytes — far less than the old full-ring fill
     * of 16 384 bytes which caused a ~2-second silence before audio started. */
    {
        size_t prime_bytes = dac_rate / 10;   /* 100 ms of 8-bit mono at dac_rate */
        if (prime_bytes > STREAM_BUF_BYTES) prime_bytes = STREAM_BUF_BYTES;
        /* Ramp 0x00→0x80 over the priming window.  The DMA ring starts at
         * 0x00 (zero-filled by the driver); a flat 0x80 would cause a hard
         * 0→128 step.  A linear ramp removes this second pop and produces
         * a smooth fade-in matching the ramp-down done above. */
        for (size_t i = 0; i < prime_bytes; i++)
            buf[i] = (uint8_t)(128 * i / prime_bytes);
        size_t _w;
        dac_continuous_write(s_dac_cont, buf, prime_bytes, &_w, pdMS_TO_TICKS(200));
        ESP_LOGI(TAG, "DMA ring primed with %u-byte ramp 0→128 (~100 ms)",
                 (unsigned)prime_bytes);
    }

    /* ── Stream PCM data directly from SPIFFS ── */
    {
        int64_t  t_start = esp_timer_get_time();

        /* When upsampling, read fewer raw bytes so the expanded output still
         * fits inside the STREAM_BUF_BYTES DMA window.
         *   8-bit  : read STREAM_BUF_BYTES/upsample → expand → STREAM_BUF_BYTES
         *   16-bit : read STREAM_BUF_BYTES/upsample → pcm16_to_pcm8 halves it
         *            → expand × upsample → STREAM_BUF_BYTES/2  (always fits)  */
        const size_t read_size = STREAM_BUF_BYTES / upsample;

        while (!s_stop_flag) {
            int64_t t_rd = esp_timer_get_time();
            int rd = (int)fread(buf, 1, read_size, f);
            int64_t rd_us = esp_timer_get_time() - t_rd;
            if (rd <= 0) break;
            /* Warn only if SPIFFS read latency exceeds half the DMA ring's
             * capacity (~256 ms at 32 kHz).  Shorter spikes are absorbed
             * silently by the ring and don't affect audio continuity. */
            if (rd_us > 250000)
                ESP_LOGW(TAG, "fread slow: %lld ms (frame %u)",
                         (long long)(rd_us / 1000), frame);

            /* Volume attenuation (operates on native bit depth) */
            apply_volume(buf, rd, hdr.bits_per_sample, s_volume);

            /* DAC continuous only accepts 8-bit unsigned PCM */
            int out_bytes = rd;
            if (hdr.bits_per_sample == 16)
                out_bytes = pcm16_to_pcm8(buf, rd);

            /* Integer nearest-neighbour upsample ─────────────────────────
             * Expand 8-bit samples in-place, back-to-front, so we never
             * overwrite a source sample before it has been duplicated. */
            if (upsample > 1) {
                for (int i = out_bytes - 1; i >= 0; i--) {
                    uint8_t s = buf[i];
                    for (uint32_t j = 0; j < upsample; j++)
                        buf[(uint32_t)i * upsample + j] = s;
                }
                out_bytes *= (int)upsample;
            }

            size_t written = 0;
            int64_t t_wr = esp_timer_get_time();
            esp_err_t werr = dac_continuous_write(s_dac_cont, buf, (size_t)out_bytes,
                                                  &written, pdMS_TO_TICKS(1000));
            int64_t wr_us = esp_timer_get_time() - t_wr;

            /* Normal DMA back-pressure: with an 8×2048-byte ring at dac_rate Hz,
             * each 4096-byte write blocks ~4096/dac_rate*1000 ms waiting for
             * 2 descriptors to drain — roughly 128 ms at 32 kHz.  Only warn if
             * the stall significantly exceeds that, which would indicate the ring
             * drained (potential gap) or the system is heavily loaded. */
            if (wr_us > 300000) {
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
    if (buf && s_dac_cont) {
        /* ── End-of-playback pop suppression ──────────────────────────────
         *
         * Hardware root cause:
         *   dac_continuous_del_channels() calls dac_ll_digi_bypass_enable(false),
         *   which instantly switches the DAC from the last DMA output value to
         *   the RTC register.  That register was last written to 0 by the
         *   start-of-playback ramp-down and has stayed at 0 throughout.
         *   If the DMA was outputting 128 (mid-rail silence), the switch is
         *   128 → 0 — a half-rail step that the amplifier hears as a loud pop.
         *
         * Fix: append a 256-sample 128→0 fade-down ramp to the DMA ring so the
         *   ring's last output is 0.  When del_channels fires, the switch is
         *   0 → 0 (RTC register also 0) — completely silent.  dac_cont_stop()
         *   then ramps 0→128 in oneshot to restore mid-rail idle.
         *
         * Drain timing:
         *   During steady-state playback the ring is nearly full.
         *   ring_bytes = DAC_DESC_NUM × DAC_DMA_BUF_SIZE (e.g. 16384 B at 32 kHz
         *   = 512 ms).  The old wait of STREAM_BUF_BYTES/dac_rate (178 ms) expired
         *   while hundreds of ms of audio were still in the ring → pop mid-audio.
         *   Correct wait = min(total_bytes_out, ring_bytes) / dac_rate + margin.
         *   For short sounds total_bytes_out < ring_bytes and the wait is shorter;
         *   for long sounds the ring is full and the wait includes the last 512 ms
         *   of the audio file playing out — no user-visible added delay. */

        const size_t   RAMP_BYTES = 256;                     /* 8 ms at 32 kHz */
        const uint32_t ring_bytes = (uint32_t)DAC_DESC_NUM * DAC_DMA_BUF_SIZE;

        /* Build the 128 → 0 ramp (linear, last sample exactly 0). */
        for (size_t i = 0; i < RAMP_BYTES; i++)
            buf[i] = (uint8_t)(128 * (RAMP_BYTES - 1 - i) / (RAMP_BYTES - 1));

        size_t _fw;
        dac_continuous_write(s_dac_cont, buf, RAMP_BYTES, &_fw, pdMS_TO_TICKS(500));

        /* Wait for the ring to fully drain.  ring_occ = bytes currently
         * queued = min(total_bytes_out, ring_bytes).  The ramp write blocked
         * until RAMP_BYTES bytes of space were available and then filled them
         * back, so occupancy is unchanged after the write. */
        uint32_t ring_occ = (total_bytes_out < ring_bytes) ? total_bytes_out : ring_bytes;
        vTaskDelay(pdMS_TO_TICKS(ring_occ * 1000 / dac_rate + 150));
        /* DAC is now at 0; dac_cont_stop() switches 0→0 and ramps 0→128. */
    }
    free(buf);
    dac_cont_stop();

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

    /* Binary semaphore (no task-ownership enforcement).
     * A mutex requires that the same task which called xSemaphoreTake also
     * calls xSemaphoreGive — but audio_play_file() takes in the caller's task
     * while audio_play_task gives on exit, which is a different task.
     * That cross-task give triggers the FreeRTOS assert in xTaskPriorityDisinherit.
     * A binary semaphore has identical counting behaviour with no ownership check. */
    s_play_mutex = xSemaphoreCreateBinary();
    xSemaphoreGive(s_play_mutex);   /* start in "available" state */
}

void audio_play_file(const char *path)
{
    if (!path || path[0] == '\0') {
        ESP_LOGW(TAG, "audio_play_file: empty path — ignoring");
        return;
    }
    ESP_LOGI(TAG, "audio_play_file: %s", path);

    /* Only PCM WAV is supported via the built-in DAC */
    const char *ext = strrchr(path, '.');
    if (!ext || strcasecmp(ext, ".wav") != 0) {
        ESP_LOGW(TAG, "Skipping non-WAV file (DAC only supports PCM WAV): %s",
                 path);
        return;
    }

    if (!s_play_mutex) {
        ESP_LOGE(TAG, "audio_play_file: mutex NULL — audio_init() not called?");
        return;
    }

    /* Orphaned-mutex recovery: if the playback task was killed externally
     * (watchdog, stack overflow) it never reached xSemaphoreGive().
     * Detect this by checking that the task handle is NULL (task is gone)
     * while the mutex is still taken (count=0).  Force-release so the next
     * play request is not permanently blocked. */
    if (s_audio_task == NULL && uxSemaphoreGetCount(s_play_mutex) == 0) {
        ESP_LOGW(TAG, "Orphaned play mutex detected (task dead) — force-releasing");
        xSemaphoreGive(s_play_mutex);
    }

    /* Non-blocking: drop immediately if a sound is already playing.
     * audio_play_file() is called from the touch-handler and HTTP-handler
     * tasks.  The old audio_stop() + 300 ms mutex wait blocked those tasks
     * for up to 600 ms; during that window the touch poll task queued new
     * events that fired immediately on unblock, producing cascading mode
     * changes ("cycling screens").
     *
     * For click / notification sounds, dropping a concurrent request is
     * the correct behaviour.  To interrupt a playing file and start a new
     * one, call audio_stop() explicitly before audio_play_file(). */
    if (xSemaphoreTake(s_play_mutex, 0) != pdTRUE) {
        ESP_LOGW(TAG, "audio busy — dropping %s", path);
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
