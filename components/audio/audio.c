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
 *   disabled – the GPIO25 pad is ISOLATED (rtc_gpio_isolate: input/output
 *             buffers off, no pulls — set once in app_main); NO DAC is
 *             brought up at all.  Measured on hardware: isolation beats every
 *             driven idle (OUTPUT-LOW clamp referenced the amp input to
 *             digital ground and conducted the chip's activity in as a static
 *             floor + 1 Hz tick; digital Hi-Z picked up broadband coupling;
 *             a live DAC buffer injects its own reference / 1/f noise).
 *             Changing audio_enabled needs a reboot.
 *
 *             (dac_continuous, used during playback, requires a perpetually
 *             running I2S0 DMA + clock and so cannot serve as a quiet idle.)
 *
 *   playing – leds_set_audio_active(true) pauses WS2812 RMT first.
 *             A flat-128 prime buffer fills the ring before playback so
 *             V_amp_in = 0 V from the first sample — no pop, no chirp.
 *             On exit the ring is flushed with 128 and LEDs resume.
 */

#include "audio.h"
#include "leds.h"
#include "microphone.h"   /* I2S0 arbitration: mic releases it during playback */
#include "board_pins.h"
#include "esp_log.h"
#include "driver/dac_continuous.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"   /* rtc_gpio_isolate — GPIO25 idle state */
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

/* Continuous DAC handle – live only while audio is enabled (streams clips
 * via I2S0 DMA).  NULL when disabled (GPIO25 is driven LOW instead). */
static dac_continuous_handle_t s_dac_cont        = NULL;
static volatile bool           s_audio_enabled   = true;
/* Set while a DAC test mode is active — blocks audio_play_file(). */
static volatile bool           s_dac_test_active = false;

/* ── Buffer / DMA sizes ─────────────────────────────────────────────── */
#define FIXED_DAC_RATE     32000
#define STREAM_BUF_BYTES   4096   /* file read chunk; also 8-bit output buf */
/* Ring sizing: 4 × 1024 = 4096 samples = 128 ms at 32 kHz.  The ring is
 * pre-filled with silence before each clip (see dac_restart) — that full-ring
 * priming is a stability invariant: removing it (v1.13.8 test) caused the
 * device to wedge and task-WDT after 1–2 button clicks.  The old 8 × 2048
 * (512 ms) sizing made that priming cost half a second of onset latency per
 * click; 4 × 1024 keeps the invariant at a quarter of the latency.  Clips are
 * preloaded to PSRAM, so 128 ms of buffering is ample against scheduling
 * jitter, and the end-of-clip drain wait shrinks proportionally. */
#define DAC_DESC_NUM          4   /* DMA descriptor count                   */
#define DAC_DMA_BUF_SIZE   1024   /* bytes per DMA descriptor               */

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

/* Per-clip fade duration (ms).  When audio is enabled, the DAC is torn down to
 * the isolated-pad idle between clips (no continuous DMA = no idle noise floor,
 * matching the stock firmware).  Each clip brings the DAC up with a fade_in
 * (0 → 128) and the playback task fades out (128 → 0) before tearing down, so
 * the idle⇄DAC level transitions don't pop.  Kept short to limit onset
 * latency on short sounds like the button click. */
#define PLAY_FADE_MS  120

/*
 * Bring up the continuous DAC from the isolated-pad idle, ramping 0 → 128 over
 * fade_ms via a cosine S-curve so the AC coupling cap charges gently (no pop),
 * then pre-fill the ring with mid-rail silence (full-ring priming — see the
 * stability note at the pre-fill loop).  Called per clip by the playback
 * task; torn back down by dac_teardown() at clip end.
 */
static void dac_restart(int fade_ms)
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
    /* The spectrum mic's adc_continuous capture also rides I2S0 — make it
     * release the peripheral before we claim it (bounded wait inside). */
    mic_set_audio_active(true);

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
        mic_set_audio_active(false);   /* no DAC came up — let capture resume */
        return;
    }
    if (dac_continuous_enable(s_dac_cont) != ESP_OK) {
        ESP_LOGE(TAG, "dac_restart: enable failed");
        dac_continuous_del_channels(s_dac_cont);
        s_dac_cont = NULL;
        mic_set_audio_active(false);   /* no DAC came up — let capture resume */
        return;
    }

    /* Anti-pop fade-in: 0 → 128 over fade_ms via cosine S-curve.
     * Gradually charges the AC cap from its idle level (~0 V) to mid-rail. */
    size_t fade_samples = (FIXED_DAC_RATE * (uint32_t)fade_ms) / 1000;
    fade_samples = (fade_samples + 3) & ~3;
    uint8_t *fade = (uint8_t *)calloc(1, fade_samples);
    if (fade) {
        for (size_t i = 0; i < fade_samples; i++) {
            float t = (float)i / (float)fade_samples;
            fade[i] = (uint8_t)(64.0f * (1.0f - cosf(t * (float)M_PI)));
        }
        size_t w;
        dac_continuous_write(s_dac_cont, fade, fade_samples, &w, portMAX_DELAY);
        free(fade);
    }

    /* Pre-fill the ring with mid-rail silence so the DMA starts fully primed.
     * This is a stability invariant, not just pop-protection: a build that
     * skipped the pre-fill (v1.13.8 test) wedged and task-WDT'd after 1–2
     * clicks.  Latency cost = ring depth (4 × 1024 = 128 ms), kept small by
     * the ring sizing above. */
    uint8_t silence[DAC_DMA_BUF_SIZE];
    memset(silence, 128, sizeof(silence));
    size_t w;
    for (int i = 0; i < DAC_DESC_NUM; i++)
        dac_continuous_write(s_dac_cont, silence, sizeof(silence), &w, portMAX_DELAY);

    ESP_LOGI(TAG, "DAC up (32 kHz, %d ms fade-in)", fade_ms);
}

/* Tear the continuous DAC down and return GPIO25 to the quiet isolated idle.
 * Called by the playback task after each clip so that — when audio is enabled —
 * there is NO continuous I2S0 DMA running between clips (the idle-noise source).
 * The caller should fade the DAC to 0 first so this teardown's transition to
 * the isolated pad has no level step (no end-of-clip pop). */
static void dac_teardown(void)
{
    if (s_dac_cont) {
        dac_continuous_disable(s_dac_cont);
        dac_continuous_del_channels(s_dac_cont);
        s_dac_cont = NULL;
    }
    /* Isolate the pad (stock firmware's idle state) rather than clamping LOW —
     * a LOW clamp references the amp input to digital ground through the pin's
     * pull-down FET and conducts every supply/ground transient into the amp
     * (constant static floor + activity hiss).  rtc_gpio_isolate() disconnects
     * the pad from the digital domain entirely; measured near-silent. */
    rtc_gpio_isolate(PIN_AUDIO_DAC);

    /* I2S0 free again — spectrum capture may resume. */
    mic_set_audio_active(false);
}

/* ── Volume scaling ─────────────────────────────────────────────────── */
static void apply_volume(uint8_t *buf, int len_bytes,
                         uint16_t bits_per_sample, int vol_pct)
{
    if (vol_pct < 0)   vol_pct = 0;
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
    if (hdr.bits_per_sample != 8 && hdr.bits_per_sample != 16) goto task_close;

    {
        long data_start = -1;
        fseek(f, 12, SEEK_SET);
        while (!feof(f)) {
            char     cid[4];
            uint32_t csz;
            if (fread(cid, 1, 4, f) < 4) break;
            if (fread(&csz, 1, 4, f) < 4) break;
            if (memcmp(cid, "data", 4) == 0) { data_start = ftell(f); break; }
            if (csz == 0) break;
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
            /* Guard against size_t overflow before the upsample multiplication.
             * A crafted WAV with a very low sample_rate can produce a huge upsample
             * factor; overflowing alloc_size would under-allocate and the fill loop
             * would write past the end of the PSRAM buffer. */
            bool size_ok = ((size_t)upsample <= 1 ||
                            post_conv <= SIZE_MAX / (size_t)upsample);
            if (size_ok) {
            size_t expanded   = post_conv * (size_t)upsample;
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
            } /* if (size_ok) */
        }
    }

    /* ── Pause LED RMT before starting DAC ─────────────────────────────
     * WS2812 current spikes on the 3.3 V rail couple into the DAC output.
     * Pausing RMT stops all transmissions; LEDs hold their last colour. */
    leds_set_audio_active(true);

    /* Bring the DAC up for this clip.  When audio is enabled the DAC is torn
     * down to the isolated-pad idle between clips (no continuous DMA floor), so
     * we (re)create it here with a short fade-in.  On failure, skip to cleanup. */
    dac_restart(PLAY_FADE_MS);
    if (!s_dac_cont) goto task_cleanup;

    /* Stream PCM to the DMA */
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
        /* Fade-out 128 → 0 over PLAY_FADE_MS (cosine), queued behind the clip
         * tail.  This ramps the AC coupling cap down to 0 V so the subsequent
         * teardown → pad isolation has no level step (no end-of-clip pop). */
        size_t fade_n = (FIXED_DAC_RATE * (uint32_t)PLAY_FADE_MS) / 1000;
        fade_n = (fade_n + 3) & ~3;
        size_t done = 0, w;
        while (done < fade_n) {
            size_t chunk = fade_n - done;
            if (chunk > STREAM_BUF_BYTES) chunk = STREAM_BUF_BYTES;
            for (size_t i = 0; i < chunk; i++) {
                float t = (float)(done + i) / (float)fade_n;              /* 0..1   */
                buf[i] = (uint8_t)(64.0f * (1.0f + cosf(t * (float)M_PI))); /* 128..0 */
            }
            if (dac_continuous_write(s_dac_cont, buf, chunk, &w, pdMS_TO_TICKS(500)) != ESP_OK)
                break;
            done += chunk;
        }
        /* Wait for the ring to fully clock out before tearing down, so the tail
         * and fade actually play (del_channels would otherwise cut them off).
         * Worst case = full ring depth + the fade just queued. */
        uint32_t ring_ms = (uint32_t)(1000ULL * DAC_DESC_NUM * DAC_DMA_BUF_SIZE / FIXED_DAC_RATE);
        vTaskDelay(pdMS_TO_TICKS(ring_ms + PLAY_FADE_MS + 30));
    }

    free(buf);
    leds_set_audio_active(false);
    /* Tear the DAC down → isolated-pad idle: no continuous DMA between clips. */
    dac_teardown();

task_close:
    fclose(f);
task_exit:
    s_audio_task = NULL;
    xSemaphoreGive(s_play_mutex);
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
        /* Disabled: do NOT touch GPIO25 — leave it in the isolated state
         * app_main already established (rtc_gpio_isolate: pad disconnected
         * from the digital domain, the stock firmware's idle).  Any change to
         * the pin's drive state is a DC step through the amp's AC coupling
         * cap = a pop, so it is set exactly once at boot and never re-driven.
         * s_dac_cont stays NULL so audio_play_file() can never stream.
         * Re-enabling requires a reboot. */
        s_audio_enabled = false;
        ESP_LOGI(TAG, "Audio disabled — GPIO%d stays isolated (untouched), no DAC",
                 PIN_AUDIO_DAC);
        return;
    }

    /* Audio is enabled, but the DAC is NOT brought up at idle.  It is created
     * per-clip by the playback task and torn down again afterwards (matching
     * the stock firmware's install-on-play / uninstall-after behaviour), so
     * there is no continuous I2S0 DMA running between clips — that continuous
     * DMA was an idle-noise source.  GPIO25 stays in the boot isolation
     * state until the first clip plays. */
    s_audio_enabled = true;
    ESP_LOGI(TAG, "Audio enabled — DAC brought up per-clip (GPIO%d isolated at idle)",
             PIN_AUDIO_DAC);
}

void audio_set_enabled(bool enabled)
{
    /* audio_enabled changes take effect after a reboot.  The web server
     * saves the new value to config.json then calls esp_restart(); on the
     * next boot audio_init() applies the correct state from scratch — the
     * GPIO25 pad stays isolated either way; only the enabled flag differs.
     * Dynamic switching is not supported. */
    (void)enabled;
    ESP_LOGI(TAG, "audio_set_enabled: change saved — takes effect after reboot");
}

void audio_play_file(const char *path)
{
    if (!s_audio_enabled)  return;
    if (s_dac_test_active) return;   /* a DAC test owns the output — skip playback */
    /* Note: s_dac_cont is NULL at idle now (DAC is torn down between clips and
     * brought up by the playback task per clip), so we do NOT gate on it here. */
    if (!path || path[0] == '\0') return;

    const char *ext = strrchr(path, '.');
    if (!ext || strcasecmp(ext, ".wav") != 0) return;

    if (!s_play_mutex) return;


    if (xSemaphoreTake(s_play_mutex, 0) != pdTRUE) {
        /* A previous clip is still playing or draining its tail.  Interrupt
         * it rather than dropping this request: button-click feedback must
         * sound on EVERY press.  s_stop_flag makes the playback task break
         * out of its streaming loop at the next chunk boundary; it then
         * fades out, drains the (now small, 128 ms) ring and releases the
         * mutex — worst case ≈ 600 ms, typically much less.  The same
         * stop-and-wait pattern is used by audio_dac_test_set(). */
        s_stop_flag = true;
        if (xSemaphoreTake(s_play_mutex, pdMS_TO_TICKS(700)) != pdTRUE) {
            /* Playback task wedged or very slow — leave s_stop_flag set so
             * it still exits ASAP; this press is dropped as a last resort. */
            ESP_LOGW(TAG, "audio_play_file: busy >700 ms — press dropped");
            return;
        }
    }

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
    if (!s_play_mutex) return;
    s_stop_flag = true;
    /* s_play_mutex is Given when idle and Taken while a clip plays.
     * Taking it here blocks until the task exits and gives it back,
     * then we restore the idle (Given) state for the next caller. */
    if (xSemaphoreTake(s_play_mutex, pdMS_TO_TICKS(300)) == pdTRUE)
        xSemaphoreGive(s_play_mutex);
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
 * "normal"  : tear down the test dac_continuous channel and return GPIO25 to
 *   the quiet isolated-pad idle (no DAC running).  This matches the normal idle
 *   for both enabled and disabled audio — a clip brings the DAC up on demand.
 */

/* Helper: release the continuous DAC channel so a test mode can claim it.
 * Also releases the I2S0 claim — a following test mode that needs the DAC
 * re-claims it before creating its own channel. */
static void dac_test_teardown(void)
{
    if (s_dac_cont) {
        dac_continuous_disable(s_dac_cont);
        dac_continuous_del_channels(s_dac_cont);
        s_dac_cont = NULL;
    }
    mic_set_audio_active(false);
}

void audio_dac_test_set(const char *mode, int param_a, int param_b)
{
    if (!mode) return;

    /* Stop any active playback so we have exclusive DAC access.
     *
     * MUST NOT proceed if the playback task hasn't released the mutex: the
     * teardown below deletes s_dac_cont while the task could still be inside
     * dac_continuous_write() on it (use-after-free → DAC driver crash /
     * wedged I2S0).  Worst-case drain is an in-flight write (≤1 s timeout)
     * + fade + ring drain, so wait longer than audio_play_file's 700 ms and
     * abort the test request on timeout — same "drop as a last resort"
     * policy audio_play_file uses.  s_stop_flag stays set on the abort path
     * so the wedged task still exits ASAP. */
    if (s_audio_task) {
        s_stop_flag = true;
        if (!s_play_mutex ||
            xSemaphoreTake(s_play_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGW(TAG, "DAC test '%s': playback still draining after 2 s — "
                          "request dropped, retry shortly", mode);
            return;
        }
        xSemaphoreGive(s_play_mutex);
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

        /* Restore the normal idle: pad isolated with no DAC running.
         * This is the idle for BOTH enabled (DAC is per-clip now) and disabled
         * audio.  A clip will bring the DAC up again on demand. */
        rtc_gpio_isolate(PIN_AUDIO_DAC);
        ESP_LOGI(TAG, "DAC test: restored to isolated-pad idle");
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
         * A fresh empty ring has all descriptors immediately available so
         * filling with a constant level completes without blocking. */
        mic_set_audio_active(true);   /* claim I2S0 from spectrum capture */
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
            mic_set_audio_active(false);
            return;
        }
        uint8_t *buf = (uint8_t *)malloc(DAC_DMA_BUF_SIZE);
        if (!buf) {
            ESP_LOGE(TAG, "DAC test: OOM");
            dac_continuous_disable(s_dac_cont);
            dac_continuous_del_channels(s_dac_cont); s_dac_cont = NULL;
            mic_set_audio_active(false);
            return;
        }
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

        mic_set_audio_active(true);   /* claim I2S0 from spectrum capture */
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
            mic_set_audio_active(false);
            return;
        }

        uint8_t *buf = (uint8_t *)malloc(DAC_DMA_BUF_SIZE);
        if (!buf) {
            ESP_LOGE(TAG, "DAC test: tone OOM");
            dac_continuous_disable(s_dac_cont);
            dac_continuous_del_channels(s_dac_cont); s_dac_cont = NULL;
            mic_set_audio_active(false);
            return;
        }
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
