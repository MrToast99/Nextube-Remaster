/**
 * @file audio.c
 * @brief Nextube audio driver — WAV playback on a software-clocked DAC.
 *
 * Hardware: GPIO25 → 0.1uF AC coupling cap → LTK8002D amp (DAC_CHAN_0), no
 * shutdown control — always live, so anything on the pad is audible.
 * Supports PCM WAV, 8- or 16-bit (16-bit down-converted to 8-bit unsigned).
 * Playback runs in a task created per clip; a mutex serialises requests.
 *
 * dac_oneshot + gptimer, not dac_continuous: dac_continuous needs I2S0, the
 * same controller the mic's adc_continuous capture needs, so audio and
 * Spectrum mode would be mutually exclusive. dac_oneshot touches no I2S,
 * drops the DMA ring, and removes dac_continuous's ~19.6kHz minimum rate.
 *
 * Idle state: the pad is ISOLATED between clips (buffers off, no pulls) —
 * every driven idle (LOW, Hi-Z, live DAC buffer) measured noisier. The
 * isolated↔driven transition itself is an audible step; three things
 * minimize it: park at MID-RAIL (128 — the coupling cap sits at ~0V against
 * the amp's bias there, so the transition moves no charge, unlike 0V);
 * ramp only between 128 and the clip's own first/last sample; ramp FAST, one
 * LSB per sample — an 8-bit ramp is a staircase, and slowing it down moves
 * the staircase rate into the audible range instead of above it. A residual
 * click at the boundary is the price of a silent idle on this hardware;
 * muting the amp across it would need the SD pin the board doesn't route.
 *
 * Playback: leds_set_audio_active(true) pauses WS2812 RMT first (rail
 * transients couple into the DAC), then [fade-in][clip][fade-out] is
 * composed in PSRAM and fed to the ISR through a small internal-RAM double
 * buffer. Nothing touches flash once the timer is running.
 */

#include "audio.h"
#include "leds.h"            /* RMT pause across playback (rail-noise coupling) */
#include "board_pins.h"
#include "esp_log.h"
#include "driver/dac_oneshot.h"   /* DAC output; the only ISR-safe DAC API */
#include "driver/gptimer.h"       /* sample clock */
#include "driver/gpio.h"          /* "hiz" test mode only */
#include "driver/rtc_io.h"        /* rtc_gpio_isolate — GPIO25 idle state */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <errno.h>           /* fopen failure reporting in audio_play_task */
#include "esp_heap_caps.h"   /* PSRAM stream alloc; DMA-pool failure reporting */
#include <math.h>

static const char *TAG = "audio";

/* ── Runtime state ─────────────────────────────────────────────────── */
static int               s_volume      = 20;
static volatile bool     s_stop_flag   = false;
static TaskHandle_t      s_audio_task  = NULL;
static SemaphoreHandle_t s_play_mutex  = NULL;
/* Serializes concurrent calls to audio_dac_test_set() itself (e.g. two
 * overlapping HTTP requests hitting the same debug endpoint racing on
 * s_dac_os/s_dac_test_active).  A separate mutex from s_play_mutex is
 * required: audio_dac_test_set() takes-then-gives s_play_mutex mid-body
 * (to wait out any in-flight playback) before doing its own work, so
 * holding s_play_mutex across the whole function would self-deadlock. */
static SemaphoreHandle_t s_dac_test_mutex = NULL;

static volatile bool           s_audio_enabled   = true;
/* Set while a DAC test mode is active — blocks audio_play_file(). */
static volatile bool           s_dac_test_active = false;

/* ── Sizes ──────────────────────────────────────────────────────────── */

/* audio_play_task's stack — measured peak ~2050 B (the ISR does the
 * per-sample work; the task only parses the WAV, composes the stream, and
 * memcpys halves). 5120 leaves ~60% headroom. This task self-deletes per
 * clip, so it never appears in the periodic stack-usage dump — measure
 * directly before trusting a size here. */
#define AUDIO_PLAY_STACK_SIZE  5120

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


/* No fade DURATION constant any more: the ramps step one LSB per sample, so
 * their length falls out of how far the clip's endpoints sit from mid-rail.
 * See the ramp construction in audio_play_task for why that is the right
 * shape. */

/* ══ Software-clocked playback engine ══════════════════════════════════
 * A gptimer alarm fires once per sample; the ISR writes one byte via
 * dac_oneshot_output_voltage() (the one ISR-safe DAC API). Samples come from
 * a two-half buffer in plain internal .bss — the ISR drains one half while
 * the task refills the other, deliberately not DMA-capable (this engine
 * uses no DMA at all). s_sw_loop switches the ISR to replay both halves
 * forever instead, for the tone test mode with no task feeding it.
 * The ISR is not IRAM-safe, so it stalls if the cache is disabled by a
 * flash write mid-clip — clips are fully pre-buffered into PSRAM before the
 * timer starts, so this only bites if something else writes flash
 * mid-playback. Making it IRAM-safe isn't a drop-in change:
 * CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y puts
 * vTaskNotifyGiveFromISR() in flash, so an IRAM-safe ISR calling it during a
 * cache-disable window would crash rather than glitch. */

#define SW_HALF_SAMPLES  1024   /* per half-buffer; 64 ms at 16 kHz */

/* Read by the ISR — plain internal RAM, deliberately not DMA-capable. */
static uint8_t  s_sw_buf[2][SW_HALF_SAMPLES];
static volatile uint16_t s_sw_len[2];   /* valid samples in each half     */
static volatile uint16_t s_sw_pos;      /* read position in current half  */
static volatile uint8_t  s_sw_cur;      /* half the ISR is reading        */
static volatile bool     s_sw_need[2];  /* half drained, task must refill */
static volatile bool     s_sw_run;      /* gate: ISR emits only when true */
static volatile bool     s_sw_loop;     /* replay both halves forever (tone) */
static volatile uint8_t  s_sw_last = 128; /* last level, held on underrun */
static gptimer_handle_t     s_sw_timer = NULL;
static dac_oneshot_handle_t s_dac_os   = NULL;
static TaskHandle_t         s_sw_feeder = NULL;

static bool sw_on_alarm(gptimer_handle_t timer,
                        const gptimer_alarm_event_data_t *edata, void *ctx)
{
    (void)timer; (void)edata; (void)ctx;
    BaseType_t hpw = pdFALSE;
    /* s_dac_os is cleared by sw_dac_stop() while this may still fire once. */
    if (!s_sw_run || !s_dac_os) return false;

    uint8_t h = s_sw_cur;
    if (s_sw_pos >= s_sw_len[h]) {
        if (s_sw_loop) {
            /* Tone mode: both halves stay valid, just wrap. No refill, no
             * notification — nothing has to keep up with the ISR. */
            h ^= 1;
            s_sw_cur = h;
            s_sw_pos = 0;
            uint8_t lv = s_sw_buf[h][s_sw_pos++];
            s_sw_last = lv;
            dac_oneshot_output_voltage(s_dac_os, lv);
            return false;
        }
        s_sw_need[h] = true;                /* hand this half back  */
        h ^= 1;
        s_sw_cur = h;
        s_sw_pos = 0;
        if (s_sw_feeder) vTaskNotifyGiveFromISR(s_sw_feeder, &hpw);
        if (s_sw_len[h] == 0) {
            /* Underrun: hold the last level rather than emitting a step,
             * which would be an audible click. */
            dac_oneshot_output_voltage(s_dac_os, s_sw_last);
            return hpw == pdTRUE;
        }
    }
    uint8_t v = s_sw_buf[h][s_sw_pos++];
    s_sw_last = v;
    dac_oneshot_output_voltage(s_dac_os, v);
    /* The gptimer callback signals "yield on exit" through its RETURN VALUE —
     * calling portYIELD_FROM_ISR() here as well would be wrong. */
    return hpw == pdTRUE;
}

/* Bring the DAC up at `rate` Hz, output parked at `start_level`. */
static bool sw_dac_start(uint32_t rate, uint8_t start_level)
{
    s_sw_run = false;
    s_sw_loop = false;
    s_sw_cur = 0; s_sw_pos = 0;
    s_sw_len[0] = s_sw_len[1] = 0;
    s_sw_need[0] = s_sw_need[1] = false;
    s_sw_last = start_level;
    s_sw_feeder = xTaskGetCurrentTaskHandle();

    dac_oneshot_config_t oc = { .chan_id = DAC_CHAN_0 };
    if (dac_oneshot_new_channel(&oc, &s_dac_os) != ESP_OK) {
        ESP_LOGE(TAG, "sw_dac_start: dac_oneshot_new_channel failed");
        s_dac_os = NULL;
        return false;
    }
    /* Park the output immediately. Creating the channel connects the pad, so
     * anything else here would be an uncontrolled level for however long
     * setup takes. */
    dac_oneshot_output_voltage(s_dac_os, start_level);

    gptimer_config_t tc = {
        .clk_src       = GPTIMER_CLK_SRC_DEFAULT,
        .direction     = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,        /* 1 MHz -> 1 us ticks */
    };
    if (gptimer_new_timer(&tc, &s_sw_timer) != ESP_OK) {
        ESP_LOGE(TAG, "sw_dac_start: gptimer_new_timer failed");
        dac_oneshot_del_channel(s_dac_os); s_dac_os = NULL;
        s_sw_timer = NULL;
        return false;
    }
    gptimer_event_callbacks_t cbs = { .on_alarm = sw_on_alarm };
    /* Must be registered before gptimer_enable(). */
    if (gptimer_register_event_callbacks(s_sw_timer, &cbs, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "sw_dac_start: callback registration failed");
        gptimer_del_timer(s_sw_timer); s_sw_timer = NULL;
        dac_oneshot_del_channel(s_dac_os); s_dac_os = NULL;
        return false;
    }
    gptimer_alarm_config_t ac = {
        .alarm_count                = 1000000UL / rate,   /* us per sample */
        .reload_count               = 0,
        .flags.auto_reload_on_alarm = true,
    };
    gptimer_set_alarm_action(s_sw_timer, &ac);
    gptimer_enable(s_sw_timer);
    gptimer_start(s_sw_timer);
    return true;
}

static void sw_dac_stop(void)
{
    s_sw_run = false;
    if (s_sw_timer) {
        gptimer_stop(s_sw_timer);
        gptimer_disable(s_sw_timer);
        gptimer_del_timer(s_sw_timer);
        s_sw_timer = NULL;
    }
    if (s_dac_os) {
        /* Leave the pad at mid-rail before releasing it: isolating from 128 is
         * silent, isolating from any other level pops as the coupling cap
         * re-centres (measured — see the fade comment in audio_play_task).
         * The fade-out already ends here; this makes it true for every exit
         * path, including the test modes and error bail-outs. */
        dac_oneshot_output_voltage(s_dac_os, 128);

        /* Isolate BEFORE deleting the channel, not after.
         *
         * The teardown used to be del_channel() then rtc_gpio_isolate() — two
         * separate pad reconfigurations, heard as a two-step click. Isolating
         * first means the single audible transition happens while the output
         * still matches the cap's charge (the silent case), and the channel
         * teardown then lands on a pad that is already disconnected from the
         * amp. It should also leave the DAC register holding 128 rather than
         * whatever del_channel resets it to, so the NEXT clip's
         * dac_oneshot_new_channel() reconnects at mid-rail instead of blipping
         * through 0 first — the likely source of the small click on play. */
        rtc_gpio_isolate(PIN_AUDIO_DAC);
        dac_oneshot_del_channel(s_dac_os);
        s_dac_os = NULL;
    }
    s_sw_feeder = NULL;
    /* Re-assert: if del_channel() above restored the pad to a driven default,
     * this puts it back to the measured-quietest state. A no-op when it did
     * not, and inaudible either way since the pad is already disconnected. */
    rtc_gpio_isolate(PIN_AUDIO_DAC);
}

/* Hold a fixed DC level (test modes "silence" / "dc"). No timer at all — a
 * one-shot write latches the level until the channel is deleted, so this is
 * strictly simpler than the DMA ring it replaces. */
static bool sw_level_start(uint8_t level)
{
    dac_oneshot_config_t oc = { .chan_id = DAC_CHAN_0 };
    if (dac_oneshot_new_channel(&oc, &s_dac_os) != ESP_OK) {
        s_dac_os = NULL;
        return false;
    }
    s_sw_last = level;
    dac_oneshot_output_voltage(s_dac_os, level);
    return true;
}

/* Free-running sine (test mode "tone"), replayed from the double buffer with
 * no task involvement at all.
 *
 * The buffer holds a whole number of cycles so the wrap is seamless, which
 * means the emitted frequency is quantised to (cycles * rate / 2*HALF). The
 * caller is told the frequency it actually got rather than the one it asked
 * for — at 32 kHz over 2048 samples the step is 15.6 Hz. */
static bool sw_tone_start(int freq, int amp, float *actual_hz)
{
    /* 16 kHz. Two reasons:
     *   - It halves the interrupt rate. At 32 kHz there are only 31.25 us
     *     between samples, and any lengthy higher-priority interrupt (WiFi in
     *     particular) or cache-disable window pushes a sample late, which on a
     *     continuous tone is directly audible as wobble.
     *   - Clips play at their own file rate, which for this firmware's assets
     *     is 16 kHz — so the tone now exercises the same timing regime as real
     *     playback instead of a harsher one, which is the point of a
     *     diagnostic.
     * Tones are capped at 4 kHz by the caller, so 16 kHz is still 4x
     * oversampled. */
    const uint32_t rate = 16000;
    const uint32_t n    = 2u * SW_HALF_SAMPLES;

    uint32_t cycles = ((uint64_t)n * (uint64_t)freq + rate / 2) / rate;
    if (cycles < 1) cycles = 1;
    if (actual_hz) *actual_hz = (float)cycles * (float)rate / (float)n;

    if (!sw_dac_start(rate, 128)) return false;

    for (uint32_t i = 0; i < n; i++) {
        float ph = 2.0f * (float)M_PI * (float)cycles * (float)i / (float)n;
        s_sw_buf[i / SW_HALF_SAMPLES][i % SW_HALF_SAMPLES] =
            (uint8_t)(128 + (int)(sinf(ph) * (float)amp));
    }
    s_sw_len[0] = s_sw_len[1] = SW_HALF_SAMPLES;
    s_sw_loop = true;
    s_sw_run  = true;
    return true;
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

/* Created fresh per clip by audio_play_file() and self-deletes when the clip
 * ends, so its stack is only held while a sound is actually playing — a
 * persistent queue-fed task would hold that stack permanently from boot,
 * which starves sht30_task/wled_sync_task/MQTT on a device whose task
 * stacks already total close to usable internal RAM. Per-clip creation can
 * still fail if internal RAM gets fragmented enough, but that's the safer
 * failure mode of the two. */
static void audio_play_task(void *arg)
{
    play_arg_t *a = (play_arg_t *)arg;
    char path[128];
    strncpy(path, a->path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    free(a);

    {
        uint8_t *preload = NULL;   /* whole composed stream, PSRAM */
        uint32_t total_bytes_out = 0;

        /* Every bail-out below used to be silent: the task would start, give up,
         * and delete itself with no log line at all — indistinguishable from
         * "played fine but inaudible". That cost real debugging time (a clip
         * that exited after 32 ms looked like successful playback), so each
         * failure now says which check rejected the file. */
        FILE *f = fopen(path, "rb");
        if (!f) {
            ESP_LOGW(TAG, "play '%s': fopen failed (errno=%d)", path, errno);
            goto task_exit;
        }

        wav_riff_hdr_t hdr;
        if (fread(&hdr, 1, sizeof(hdr), f) < (int)sizeof(hdr)) {
            ESP_LOGW(TAG, "play '%s': short read on RIFF header", path);
            goto task_close;
        }
        if (memcmp(hdr.riff_id, "RIFF", 4) != 0 || memcmp(hdr.wave_id, "WAVE", 4) != 0) {
            ESP_LOGW(TAG, "play '%s': not a RIFF/WAVE file", path);
            goto task_close;
        }
        if (hdr.audio_format != 1) {
            ESP_LOGW(TAG, "play '%s': not PCM (audio_format=%u)", path, (unsigned)hdr.audio_format);
            goto task_close;
        }
        if (hdr.bits_per_sample != 8 && hdr.bits_per_sample != 16) {
            ESP_LOGW(TAG, "play '%s': unsupported bit depth %u (need 8 or 16)",
                     path, (unsigned)hdr.bits_per_sample);
            goto task_close;
        }

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
            if (data_start < 0) {
                ESP_LOGW(TAG, "play '%s': no 'data' chunk found", path);
                goto task_close;
            }
            fseek(f, data_start, SEEK_SET);
        }

        /* No upsampling: the gptimer clocks at the file's own rate. The old
         * integer upsample existed purely because dac_continuous could not
         * clock below ~19.6 kHz; a software sample clock has no such floor,
         * so a 16 kHz file plays at 16 kHz with no zero-order-hold imaging
         * and at half the interrupt rate. */
        uint32_t rate = hdr.sample_rate;
        if (rate < 4000 || rate > 48000) {
            ESP_LOGW(TAG, "play '%s': unsupported sample rate %u Hz", path, (unsigned)rate);
            goto task_cleanup;
        }

        /* -- Compose the whole output stream in PSRAM ----------------------
         * Layout: [fade-in 0->128][clip, 8-bit unsigned][fade-out 128->0].
         *
         * Building it as one contiguous buffer means the feed loop is a plain
         * sequential copy and the fades need no special-casing in the ISR path.
         * The fades are DC ramps, not amplitude envelopes: they walk the AC
         * coupling cap between 0 V and mid-rail so neither the pad connect at
         * the start nor the isolate at the end is a step the amp can hear.
         *
         * Pre-buffering the entire clip also keeps all file I/O off the
         * playback path - nothing touches flash once the timer is running. */
#define PSRAM_PRELOAD_MAX  (256 * 1024)
/* Worst-case ramp length: a full 0..255 LSB span at one LSB per sample. */
#define FADE_MAX_SAMPLES   255
        size_t clip_n  = 0;
        size_t total_n = 0;
        uint8_t *stream = NULL;   /* composed stream start (inside `preload`) */
        {
            long cur = ftell(f);
            fseek(f, 0, SEEK_END);
            long eof = ftell(f);
            fseek(f, cur, SEEK_SET);
            size_t raw_bytes = (eof > cur) ? (size_t)(eof - cur) : 0;
            if (raw_bytes == 0 || raw_bytes > PSRAM_PRELOAD_MAX) {
                ESP_LOGW(TAG, "play '%s': data chunk %u B not playable (max %u)",
                         path, (unsigned)raw_bytes, (unsigned)PSRAM_PRELOAD_MAX);
                goto task_cleanup;
            }
            clip_n = (hdr.bits_per_sample == 16) ? raw_bytes / 2 : raw_bytes;

            size_t alloc_n = FADE_MAX_SAMPLES + clip_n + FADE_MAX_SAMPLES;
            preload = (uint8_t *)heap_caps_malloc(alloc_n, MALLOC_CAP_SPIRAM);
            if (!preload) {
                ESP_LOGW(TAG, "play '%s': PSRAM stream alloc failed (%u B)",
                         path, (unsigned)alloc_n);
                goto task_cleanup;
            }

            /* Read the clip into its slot, convert in place, apply volume. */
            uint8_t *clip = preload + FADE_MAX_SAMPLES;
            size_t got = fread(clip, 1, raw_bytes, f);
            apply_volume(clip, (int)got, hdr.bits_per_sample, s_volume);
            if (hdr.bits_per_sample == 16) {
                int n8 = pcm16_to_pcm8(clip, (int)got);
                clip_n = (size_t)n8;
            } else {
                clip_n = got;
            }

            /* Ramps between MID-RAIL (128) and the clip's own first/last
             * sample, at exactly one LSB per sample.
             *
             * Mid-rail, not 0: at 128 the coupling cap sits at ~0V against
             * the amp's bias, so isolating moves no charge; at 0V it holds
             * ~1.65V and isolating pops.
             *
             * One LSB per sample, not a smooth curve: an 8-bit ramp is a
             * staircase, and its step RATE is what's audible — spreading it
             * over more samples lowers the staircase frequency into the
             * audible range instead of above it. One step per sample puts
             * the staircase at the sample rate itself (16kHz), above
             * hearing — counter-intuitively, a LONGER ramp is worse here.
             *
             * Linear, not cosine: an S-curve clusters steps at the ends,
             * the opposite of the uniform 1-LSB/sample step this needs. A
             * clip already starting at mid-rail needs no ramp at all. */
            uint8_t first = clip_n ? clip[0] : 128;
            uint8_t last  = clip_n ? clip[clip_n - 1] : 128;

            int din  = (int)first - 128;
            int dout = 128 - (int)last;
            size_t fade_in_n  = (size_t)(din  < 0 ? -din  : din);
            size_t fade_out_n = (size_t)(dout < 0 ? -dout : dout);

            uint8_t *fin = clip - fade_in_n;
            int step_in = (din > 0) ? 1 : -1;
            for (size_t i = 0; i < fade_in_n; i++)
                fin[i] = (uint8_t)(128 + step_in * (int)(i + 1));

            uint8_t *fout = clip + clip_n;
            int step_out = (dout > 0) ? 1 : -1;
            for (size_t i = 0; i < fade_out_n; i++)
                fout[i] = (uint8_t)((int)last + step_out * (int)(i + 1));

            stream  = fin;
            total_n = fade_in_n + clip_n + fade_out_n;
        }

        /* -- Pause LED RMT before driving the pad --------------------------
         * WS2812 current spikes on the 3.3 V rail couple into the DAC output.
         * Pausing RMT stops all transmissions; LEDs hold their last color.
         * Done while the pad is still isolated so the transient is inaudible. */
        leds_set_audio_active(true);

        /* Park at mid-rail, not 0 — connecting the pad at 128 matches the
         * charge the coupling cap already holds from the previous isolate,
         * so there is no step to hear. See the fade comment above. */
        if (!sw_dac_start(rate, 128)) {
            leds_set_audio_active(false);
            goto task_cleanup;
        }

        /* -- Feed the ISR's double buffer ----------------------------------
         * Prime both halves, start emitting, then top up whichever half the
         * ISR hands back. The ISR notifies us, so this blocks rather than
         * polls; the timeout only bounds a lost-notification stall. */
        {
            size_t pos  = 0;
            /* Counts halves actually holding samples. Must be counted, not
             * assumed to be 2: a clip shorter than one half leaves the second
             * half empty, and the ISR never hands back a half it never played
             * — so a hardcoded 2 would spin out the drain guard below. */
            int    live = 0;
            for (int h = 0; h < 2; h++) {
                size_t n = total_n - pos;
                if (n > SW_HALF_SAMPLES) n = SW_HALF_SAMPLES;
                if (n) memcpy(s_sw_buf[h], stream + pos, n);
                s_sw_len[h]  = (uint16_t)n;
                s_sw_need[h] = false;
                pos += n;
                if (n) live++;
            }
            s_sw_run = true;

            /* Keep servicing hand-backs until BOTH halves have been handed
             * back empty, not just until source data runs out — a half
             * whose len is left non-zero gets replayed forever, since the
             * ISR only holds its last level when it finds len == 0, and
             * stopping the timer mid-loop would cut the clip at an
             * arbitrary sample instead of its own final mid-rail ramp
             * sample. `live` tracks halves still holding samples; the
             * guard bounds the loop so a stalled ISR can't hang the task. */
            int guard = (int)(total_n / SW_HALF_SAMPLES) + 8;
            while (live > 0 && !s_stop_flag && guard-- > 0) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
                for (int h = 0; h < 2; h++) {
                    if (!s_sw_need[h]) continue;
                    size_t n = (pos < total_n) ? (total_n - pos) : 0;
                    if (n > SW_HALF_SAMPLES) n = SW_HALF_SAMPLES;
                    if (n) memcpy(s_sw_buf[h], stream + pos, n);
                    s_sw_len[h]  = (uint16_t)n;   /* length before clearing the */
                    s_sw_need[h] = false;         /* flag the ISR watches       */
                    pos += n;
                    if (n == 0) live--;           /* this half is now silent   */
                }
            }
            total_bytes_out = (uint32_t)pos;

            /* Both halves empty means the ISR is already holding the final
             * sample — no drain wait needed, just a moment for the coupling
             * cap to settle at mid-rail before the pad transition. */
            vTaskDelay(pdMS_TO_TICKS(20));
        }

task_cleanup:
        /* Stop the clock and isolate the pad BEFORE resuming the LEDs - the
         * fade-out has already walked the output to 0 V, so isolation is not a
         * step, and the RMT restart transient then lands on a pad that is
         * already disconnected from the amp. */
        sw_dac_stop();
        leds_set_audio_active(false);
        if (preload) { free(preload); preload = NULL; }

task_close:
        fclose(f);
task_exit:
        /* Debug level, not info: the sizing question this answered is settled
         * (peak ~2050 B of 5120), so it is off by default but still one
         * esp_log_level_set away if playback ever needs investigating again. */
        ESP_LOGD(TAG, "audio_play: %u B out, stack peak %u B of %u",
                 (unsigned)total_bytes_out,
                 (unsigned)(AUDIO_PLAY_STACK_SIZE
                            - uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
                 (unsigned)AUDIO_PLAY_STACK_SIZE);
        s_audio_task = NULL;
        xSemaphoreGive(s_play_mutex);
        vTaskDelete(NULL);
    }
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
    s_dac_test_mutex = xSemaphoreCreateMutex();

    if (!enabled) {
        /* Disabled: do NOT touch GPIO25 — leave it in the isolated state
         * app_main already established (rtc_gpio_isolate: pad disconnected
         * from the digital domain, the stock firmware's idle).  Any change to
         * the pin's drive state is a DC step through the amp's AC coupling
         * cap = a pop, so it is set exactly once at boot and never re-driven.
         * s_audio_enabled stays false so audio_play_file() never spawns a
         * playback task. Re-enabling requires a reboot. */
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
    /* Nothing to check about the DAC itself: the channel and its timer are
     * created per clip by the playback task and torn down again, so there is
     * no persistent handle whose state could gate this. */
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

    /* This CAN fail once internal RAM is fragmented enough that no contiguous
     * 16 KB block is left — see audio_play_task's comment. Kept loud rather
     * than silent (the original code dropped the press with no log at all,
     * which is why two dead playback tests took a while to explain). */
    /* Priority 7, above display_task's 6: this task refills a double buffer
     * the sample-clock ISR drains in real time (one half is 64ms at 16kHz),
     * and display_task's heavy frames (JPEG decode + SPI blit) at a lower
     * priority preempted playback often enough to audibly wobble the
     * playback rate. This task is blocked on the ISR's notification almost
     * all the time, so raising it doesn't meaningfully starve the display.
     *
     * Pinned to core 1, not left unpinned: sw_dac_start() allocates the
     * gptimer interrupt on whichever core this task happens to be running
     * on, so unpinned would move the sample-clock ISR between cores per
     * clip. Core 1 specifically because core 0 carries WiFi's interrupts —
     * the longest and most frequent on the device — while core 1 only sees
     * short SPI-completion interrupts during playback (LED RMT already
     * paused). mic_task is pinned to core 0, so the two can now run
     * concurrently (playback no longer needs I2S0) without this task
     * preempting the mic's Goertzel work. */
    if (xTaskCreatePinnedToCore(audio_play_task, "audio_play", AUDIO_PLAY_STACK_SIZE,
                                a, 7, &s_audio_task, 1) != pdPASS) {
        ESP_LOGW(TAG, "audio_play_file: task create failed (largest_internal=%u B) — press dropped",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
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
 * All four modes drive the pad through the same dac_oneshot + gptimer engine
 * real playback uses, so a passing test actually says something about
 * whether a clip will play. None touch I2S0, so the microphone is unaffected.
 *
 * "hiz"            : GPIO25 as a plain input, DAC buffer fully powered down —
 *                     the most isolated state available.
 * "silence" / "dc" : one-shot write latches a constant level with no clock
 *                     running — steps the level manually, exactly what real
 *                     playback's ramps avoid, for characterising pops.
 * "tone"           : loop mode replays a whole number of sine cycles from the
 *                     double buffer for a seamless wrap; frequency is
 *                     quantised by that and logged.
 * "normal"         : return to the quiet isolated-pad idle, same as between
 *                     clips.
 */

/* Helper: release whatever a previous test mode left driving the pad.
 *
 * Nothing here touches I2S0, so the microphone is never disturbed by a
 * diagnostic. And because the test modes drive the pad through the same
 * engine as real playback, a passing tone actually means playback works —
 * which was not true while these ran on dac_continuous and clips ran on
 * one-shot. */
static void dac_test_teardown(void)
{
    sw_dac_stop();
}

void audio_dac_test_set(const char *mode, int param_a, int param_b)
{
    if (!mode) return;

    /* Serialize whole-function calls against each other (e.g. two overlapping
     * HTTP debug-endpoint requests) — everything below races on s_dac_os /
     * s_dac_test_active if two callers interleave.  See s_dac_test_mutex's
     * comment for why this can't just reuse s_play_mutex. */
    if (s_dac_test_mutex) xSemaphoreTake(s_dac_test_mutex, portMAX_DELAY);

    /* Stop any active playback so we have exclusive DAC access.
     *
     * MUST NOT proceed if the playback task hasn't released the mutex: the
     * teardown below deletes s_dac_os and the timer while the ISR could still
     * be writing through them (use-after-free → DAC driver crash).
     * Worst-case drain is the tail delay plus the fade-out still buffered, so
     * wait longer than audio_play_file's 700 ms and
     * abort the test request on timeout — same "drop as a last resort"
     * policy audio_play_file uses.  s_stop_flag stays set on the abort path
     * so the wedged task still exits ASAP. */
    if (s_audio_task) {
        s_stop_flag = true;
        if (!s_play_mutex ||
            xSemaphoreTake(s_play_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGW(TAG, "DAC test '%s': playback still draining after 2 s — "
                          "request dropped, retry shortly", mode);
            goto done;
        }
        xSemaphoreGive(s_play_mutex);
        s_stop_flag = false;
    }

    /* ── "normal" — restore idle silence ────────────────────────────── */
    if (strcmp(mode, "normal") == 0) {
        if (!s_dac_test_active) {
            ESP_LOGI(TAG, "DAC test: already normal, no-op");
            goto done;
        }
        dac_test_teardown();
        s_dac_test_active = false;

        /* Restore the normal idle: pad isolated with no DAC running.
         * This is the idle for BOTH enabled (DAC is per-clip now) and disabled
         * audio.  A clip will bring the DAC up again on demand. */
        rtc_gpio_isolate(PIN_AUDIO_DAC);
        ESP_LOGI(TAG, "DAC test: restored to isolated-pad idle");
        goto done;
    }

    /* ── All other modes: tear down current driver first ────────────── */
    dac_test_teardown();

    /* ── "hiz" — power off DAC output buffer entirely (test/diagnostic only) */
    if (strcmp(mode, "hiz") == 0) {
        gpio_reset_pin(PIN_AUDIO_DAC);
        gpio_set_direction(PIN_AUDIO_DAC, GPIO_MODE_INPUT);
        s_dac_test_active = true;
        ESP_LOGI(TAG, "DAC test: Hi-Z (GPIO%d = input)", PIN_AUDIO_DAC);
        goto done;
    }

    /* ── "silence" / "dc" — hold a constant level, no clock ──────────── */
    if (strcmp(mode, "silence") == 0 || strcmp(mode, "dc") == 0) {
        int level = (strcmp(mode, "dc") == 0) ? param_a : 128;
        if (level < 0)   level = 0;
        if (level > 255) level = 255;

        if (!sw_level_start((uint8_t)level)) {
            ESP_LOGE(TAG, "DAC test: %s — one-shot channel init failed", mode);
            goto done;
        }
        s_dac_test_active = true;
        ESP_LOGI(TAG, "DAC test: %s level=%d (~%.0fmV)",
                 mode, level, level * 3300.0f / 255.0f);
        goto done;
    }

    /* ── "tone" — free-running sine from the playback engine ─────────── */
    if (strcmp(mode, "tone") == 0) {
        int   freq = (param_a > 0 && param_a <= 4000) ? param_a : 1000;
        int   amp  = (param_b >= 0 && param_b <= 127) ? param_b : 64;
        float actual = 0.0f;

        if (!sw_tone_start(freq, amp, &actual)) {
            ESP_LOGE(TAG, "DAC test: tone — engine init failed");
            goto done;
        }
        s_dac_test_active = true;
        /* Frequency is quantised so the loop wraps seamlessly — report what
         * was actually produced, not what was requested. */
        ESP_LOGI(TAG, "DAC test: tone %.1f Hz (requested %d) amp=%d",
                 (double)actual, freq, amp);
        goto done;
    }

    ESP_LOGW(TAG, "DAC test: unknown mode '%s'", mode);

done:
    if (s_dac_test_mutex) xSemaphoreGive(s_dac_test_mutex);
}

void audio_dac_test_stop(void)
{
    audio_dac_test_set("normal", 0, 0);
}
