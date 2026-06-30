/**
 * @file microphone.c
 * @brief Electret mic + LMV321IDBVR preamp – adc_continuous + Goertzel
 *
 * Hardware: unmarked 4 mm SMT electret capsule (candidates in README; the
 * exact part is a visual guess) → LMV321IDBVR op-amp → GPIO35 (ADC1_CH7).
 *
 * CAPTURE — adc_continuous (I2S0 DMA, hardware-clocked):
 *   • The SAR is clocked by the I2S0 digital controller at ADC_HW_RATE
 *     (32 kHz — the ESP32 digital controller's minimum is 20 kHz) and DMA
 *     delivers complete frames; mic_task averages ×4 down to the 8 kHz
 *     analysis rate.  Exact sample spacing, ~zero CPU per sample.
 *   • Every software-timed approach was tried and failed: esp_timer +
 *     adc_oneshot_read could only sustain ~1.7 kHz (each read costs
 *     300–600 µs → aliasing), and even with fast register-level reads the
 *     RTOS preemption jitter (esp_timer catch-up clustering) smeared
 *     high-frequency energy into the low bands.  Uniform sampling needs a
 *     hardware clock.  See the ADC_HW_RATE comment for the full history.
 *   • I2S0 is shared with audio playback (dac_continuous): the audio
 *     component brackets DAC use with mic_set_audio_active(true/false);
 *     mic_task releases the peripheral within ~one 100 ms gate poll and the
 *     spectrum freezes for the duration of the clip.
 *
 * Silence gate (runtime, debug panel): cfg->mic_silence_gate — SPECTRAL gate
 * on the sum of post-floor band power (silence <10, quiet audio >50; 0=off).
 * Gain / noise floor: MIC_GAIN, MIC_NOISE_FLOOR  (compile-time).
 */

#include "microphone.h"
#include "config_mgr.h"
#include "board_pins.h"
#include "esp_adc/adc_oneshot.h"     /* debug-panel single reads            */
#include "esp_adc/adc_continuous.h"  /* hardware-clocked capture (I2S0 DMA) */
#include "soc/soc_caps.h"            /* SOC_ADC_DIGI_RESULT_BYTES           */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "mic";

/* ── Sampling parameters ─────────────────────────────────────────────── */
#define SAMPLE_RATE     8000
#define FRAME_SIZE      128             /* samples per Goertzel frame (16 ms) */
#define BAND_COUNT      MIC_BAND_COUNT   /* 24 — must equal MIC_BAND_COUNT in microphone.h */
/* ── Hardware-clocked capture (adc_continuous → I2S0 DMA) ───────────────
 * History, so nobody walks back into the trap: capture was originally an
 * esp_timer firing adc_oneshot_read() every 125 µs.  Each read costs
 * 300–600 µs (driver mutex, per-read reconfiguration), so the timer ran in
 * permanent catch-up: the real rate was ~1.7 kHz (bands above the true
 * ~850 Hz Nyquist were aliased noise) and the catch-up bursts produced
 * heavy sampling JITTER — clustered samples interpreted as uniform — which
 * smeared high-frequency energy into the low bands (tone-sweep test: dead
 * above ~450 Hz).  A register-level fast read fixed the rate but cannot fix
 * the jitter: software-timed sampling on a loaded RTOS core is never
 * uniform.
 *
 * adc_continuous clocks the SAR from hardware (the I2S0 peripheral on
 * ESP32) with DMA delivery: exact sample spacing, ~zero CPU per sample.
 * The ESP32 digital ADC controller's minimum rate is 20 kHz, so we capture
 * at 32 kHz and average every 4 samples down to the 8 kHz design rate —
 * the averaging doubles as a crude anti-alias filter and adds ~1 bit SNR.
 *
 * I2S0 is shared with audio playback (dac_continuous, per-clip): the audio
 * component claims it via mic_set_audio_active(true) before bringing the
 * DAC up and releases it after teardown.  The spectrum freezes for the
 * duration of a clip — same pattern as leds_set_audio_active(). */
#define ADC_HW_RATE        32000
#define DECIM              4                       /* 32 kHz → 8 kHz        */
#define RAW_FRAME_SAMPLES  (FRAME_SIZE * DECIM)    /* 512 raw per frame     */
#define RAW_FRAME_BYTES    (RAW_FRAME_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES)

/* Per-frame filter constants at the native 62.5 fps frame rate:
 *   DECAY        peak-hold fall per frame (~1 s decay)
 *   PHASE1_*     fast noise-floor convergence: 250 frames ≈ 4 s
 *   NOISE_ALPHA  steady-state floor tracking, τ ≈ 8 s                     */
#define DECAY           0.85f
#define PHASE1_FRAMES   250
#define PHASE1_ALPHA    0.02f
#define DC_ALPHA        0.999f

/* ── Sensitivity (compile-time) ──────────────────────────────────────── */
#define MIC_GAIN        1.0f    /* LMV321 handles hardware gain */
#define MIC_NOISE_FLOOR 1.0f   /* Goertzel normalisation floor for active frames */

/* ── Frequency bands ─────────────────────────────────────────────────── */
/* 24 log-spaced bands: 280 Hz → 3800 Hz (Nyquist at 8 kHz sample rate).
 * Ratio ≈ 1.12× per step — grouped 4 per tube for a richer spectrum display.
 * Avoids the 125–250 Hz range (SPI switching noise / mains hum on this hardware).
 *   Tube 0 (bass):     280  315  350  395 Hz
 *   Tube 1 (u.bass):   440  495  555  620 Hz
 *   Tube 2 (lo.mid):   695  780  870  975 Hz
 *   Tube 3 (mid):     1095 1225 1370 1535 Hz
 *   Tube 4 (presence):1720 1925 2160 2420 Hz
 *   Tube 5 (treble):  2710 3030 3395 3800 Hz */
static const float BAND_FREQS[BAND_COUNT] = {
    /* Tube 0 — low bass */
     280.0f,  315.0f,  350.0f,  395.0f,
    /* Tube 1 — upper bass */
     440.0f,  495.0f,  555.0f,  620.0f,
    /* Tube 2 — lower mid */
     695.0f,  780.0f,  870.0f,  975.0f,
    /* Tube 3 — midrange */
    1095.0f, 1225.0f, 1370.0f, 1535.0f,
    /* Tube 4 — presence */
    1720.0f, 1925.0f, 2160.0f, 2420.0f,
    /* Tube 5 — treble */
    2710.0f, 3030.0f, 3395.0f, 3800.0f,
};

/* ── Bin-summed band energy (continuous spectral coverage) ──────────────
 * A single Goertzel per band has SPECTRAL CRACKS: with 128 samples at 8 kHz
 * each bin only hears ±125 Hz (Hann main lobe), but the log-spaced band
 * centres above ~1 kHz are 200–400 Hz apart — tones falling between centres
 * simply vanished (tone-sweep test: 2050 Hz measured 0.3 % of the energy of
 * an on-centre tone; ≥3.2 kHz measured zero).  The bug existed from day one
 * but was masked while capture was aliased/jittered.
 *
 * Fix: compute Goertzel at EVERY 62.5 Hz bin across the analysis range and
 * sum bins into bands by geometric band edges (lookup built in mic_init).
 * Simulated response across a 300–3900 Hz sweep: correct tube everywhere,
 * worst-case energy −5 dB vs best (was −∞).  ~60 bins ≈ 2.5× the Goertzel
 * cost of the old 24 — still only a few percent of one core. */
#define SPEC_BIN_HZ   ((float)SAMPLE_RATE / (float)FRAME_SIZE)   /* 62.5 Hz */
#define SPEC_BIN_LO   4                       /* 250 Hz                    */
#define SPEC_BIN_HI   63                      /* 3937 Hz (< 4 kHz Nyquist) */
#define SPEC_NBINS    (SPEC_BIN_HI - SPEC_BIN_LO + 1)
static int8_t s_bin_band[SPEC_NBINS];         /* bin → band index, -1 = skip */

/* The narrow low bands (35–45 Hz wide) can be NARROWER than the 62.5 Hz bin
 * grid, leaving a band with zero bins (measured: band 2 / 350 Hz read
 * raw = 0.000 forever — tube 1 bar 3 could never light).  Bands flagged
 * here get a single Goertzel at their centre frequency instead, which is
 * the correct measure for a band narrower than one bin. */
static bool s_band_center_only[BAND_COUNT];

/* ── ADC mappings ────────────────────────────────────────────────────── */
static const adc_channel_t ADC1_CHAN_MAP[8] = {
    ADC_CHANNEL_0, ADC_CHANNEL_1, ADC_CHANNEL_2, ADC_CHANNEL_3,
    ADC_CHANNEL_4, ADC_CHANNEL_5, ADC_CHANNEL_6, ADC_CHANNEL_7,
};
static const int ADC1_GPIO_MAP[8] = { 36, 37, 38, 39, 32, 33, 34, 35 };

/* ── Shared ADC state ────────────────────────────────────────────────── */
static adc_oneshot_unit_handle_t s_adc        = NULL;
static adc_channel_t             s_active_chan = ADC_CHANNEL_7;
static uint8_t                   s_active_ch  = 7;   /* config index */

/* ── Continuous-capture state ────────────────────────────────────────── */
/* The adc_continuous handle is created/started and stopped/deleted ONLY by
 * mic_task (single owner — no handle lifecycle races).  Other tasks
 * communicate via flags:
 *   s_audio_claims_i2s — set by the audio component around DAC playback
 *                        (dac_continuous also needs I2S0); mic_task tears
 *                        the capture down and acks via s_i2s_released.   */
static adc_continuous_handle_t s_acq               = NULL;
static volatile bool           s_audio_claims_i2s  = false;
static SemaphoreHandle_t       s_i2s_released      = NULL;

/* Hann window (precomputed in mic_init) — applied to the Goertzel input to
 * suppress spectral leakage: without it a strong off-band tone (e.g. a loud
 * bass note below the lowest 280 Hz band) splatters across all bands via
 * the rectangular window's −13 dB sidelobes. */
static float s_hann[FRAME_SIZE];

/* Raw-frame export for /api/debug/micframe — lets the unprocessed 32 kHz
 * sample stream be inspected offline (waveform purity, sample ordering,
 * aliasing) without serial access.  mic_task copies the next DMA frame here
 * when requested. */
static volatile bool     s_dump_req  = false;
static uint16_t          s_dump_buf[RAW_FRAME_SAMPLES];
static float             s_dump_dec[FRAME_SIZE];   /* decimated view, SAME frame */
static SemaphoreHandle_t s_dump_done = NULL;

/* Per-band pipeline snapshot for /api/debug/micbands — written every frame
 * under s_mux so each processing stage (raw band energy → noise floor →
 * post-tilt power) can be inspected live.  Diagnoses "band shows nothing"
 * by revealing WHICH stage eats the signal (no raw energy = capture/
 * acoustics; floor ≈ raw = poisoned baseline; power ok = display side). */
static float s_dbg_raw  [BAND_COUNT];
static float s_dbg_floor[BAND_COUNT];
static float s_dbg_power[BAND_COUNT];

/* ── Shared output ───────────────────────────────────────────────────── */
static float        s_bands[BAND_COUNT];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ── Spectral tilt (display weighting) ───────────────────────────────── */
/* Goertzel bins have constant bandwidth (SAMPLE_RATE/FRAME_SIZE = 62.5 Hz),
 * and music/voice power per bin falls roughly as 1/f ("pink").  With the
 * global peak normalisation below, the bass bands therefore always set the
 * scale and the upper tubes (presence/treble, 1.7–3.8 kHz) barely move.
 * Weighting each band by (f / f_lowest)^MIC_TILT_EXP counteracts the slope:
 * exponent 1.0 = +3 dB/octave in power, which flattens an ideal pink source
 * (top band ×13.6, bass ×1).  Raise toward 1.5 for an even brighter top end,
 * lower toward 0.5 for a more bass-weighted classic look.
 *
 * Applied AFTER noise-floor subtraction so the adaptive floor and users'
 * saved calibration baselines (both captured in the unweighted domain)
 * remain valid — the tilt shapes only the displayed power. */
/* ── Display gamma (perceptual bar scaling) ─────────────────────────────
 * Published band values are (peak/max)^SPEC_GAMMA instead of the linear
 * power ratio.  Music's loudest band (bass) carries 10–20× the power of
 * the mids; linearly mapped, that pins one tube at full scale and leaves
 * the rest at one or two segments.  γ = 0.45 ≈ square-root-ish loudness
 * compression: a band at 10 % of max displays at ~35 % height — bass still
 * clearly leads, but mids and treble live.  1.0 = linear (old behaviour);
 * lower = flatter/more compressed. */
#define SPEC_GAMMA  0.45f

/* With BIN-SUMMED bands (see s_bin_band) the tilt is now OFF by default:
 * log-spaced bands sum more bins as frequency rises (bandwidth ∝ f), which
 * already flattens a pink source — the same compensation the tilt provided
 * for the old single-bin design.  Stacking both gave broadband noise an f²
 * boost (measured: idle noise displayed 0.5–1.0 on tubes 5–6 while mid
 * bands sat at 0.02).  Raise above 0 only if real music still reads
 * bass-heavy after a clean baseline capture. */
#define MIC_TILT_EXP  0.0f
static float s_band_weight[BAND_COUNT];

/* ── Per-band adaptive noise floor ──────────────────────────────────── */
/* Long-term exponential average of each band's Goertzel energy.
 * Subtracted before peak-hold so the display reads zero in silence even
 * when the ADC / LMV321 preamp has a non-trivial electrical noise floor.
 * Adapts to the quiet-period baseline within ~8 s of startup. */
static float s_noise_floor[BAND_COUNT];   /* zero-initialised; adapts automatically */
#define NOISE_ALPHA  0.002f               /* time constant ≈ 500 frames × 16 ms ≈ 8 s */

/* Phase 1 frame counter — promoted to file scope so mic_reset_calibration()
 * and mic_apply_calibration() can manipulate it without touching mic_task. */
static int s_noise_cal = 0;   /* counts frames during Phase 1; PHASE1_FRAMES = steady state */

/* ── Manual baseline calibration state ──────────────────────────────── */
static volatile bool     s_cal_requested = false;  /* set by mic_calibrate() */
static int               s_cal_frame_cnt = 0;      /* frames accumulated so far */
static float             s_cal_accum[BAND_COUNT];  /* raw Goertzel accumulator   */
static SemaphoreHandle_t s_cal_done      = NULL;   /* binary; given when done    */

/* Last raw ADC count (updated by mic_task per frame) for the debug panel */
static volatile int s_last_raw = -1;

/* ── Capture lifecycle (mic_task is the sole owner of s_acq) ─────────── */

/* Create + start hardware-clocked capture on the active channel.
 * Returns false (and logs, rate-limited) when I2S0 is unavailable — e.g.
 * the audio DAC released it a moment ago and the peripheral has not fully
 * settled; the caller just retries on its next loop pass. */
static bool acq_start(void)
{
    if (s_acq) return true;

    adc_continuous_handle_cfg_t hcfg = {
        .max_store_buf_size = RAW_FRAME_BYTES * 4,
        .conv_frame_size    = RAW_FRAME_BYTES,
    };
    if (adc_continuous_new_handle(&hcfg, &s_acq) != ESP_OK) {
        s_acq = NULL;
        goto fail;
    }

    adc_digi_pattern_config_t pat = {
        .atten     = ADC_ATTEN_DB_12,
        .channel   = (uint8_t)s_active_chan,
        .unit      = ADC_UNIT_1,
        .bit_width = ADC_BITWIDTH_12,
    };
    adc_continuous_config_t ccfg = {
        .pattern_num    = 1,
        .adc_pattern    = &pat,
        .sample_freq_hz = ADC_HW_RATE,
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
    };
    if (adc_continuous_config(s_acq, &ccfg) != ESP_OK ||
        adc_continuous_start(s_acq)         != ESP_OK) {
        adc_continuous_deinit(s_acq);
        s_acq = NULL;
        goto fail;
    }
    return true;

fail:;
    static int64_t s_last_fail_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - s_last_fail_us > 10LL * 1000 * 1000) {
        ESP_LOGW(TAG, "acq_start: I2S0/ADC unavailable — will retry");
        s_last_fail_us = now;
    }
    return false;
}

static void acq_stop(void)
{
    if (!s_acq) return;
    adc_continuous_stop(s_acq);
    adc_continuous_deinit(s_acq);
    s_acq = NULL;
}

/* Called by the audio component around DAC playback (dac_continuous and
 * adc_continuous both need the I2S0 peripheral on ESP32 and cannot coexist).
 * active=true blocks capture and waits (bounded) for mic_task to release
 * I2S0; active=false lets mic_task re-acquire on its next loop pass.
 * Safe to call when the mic was never initialised (flag-only no-op). */
void mic_set_audio_active(bool active)
{
    s_audio_claims_i2s = active;
    if (active && s_i2s_released) {
        /* Drain any stale ack, then wait for mic_task to confirm release.
         * Bounded: if the ack is late, the caller's dac_continuous bring-up
         * retries (100 ms apart) absorb the remainder of the handoff. */
        while (xSemaphoreTake(s_i2s_released, 0) == pdTRUE) { }
        xSemaphoreTake(s_i2s_released, pdMS_TO_TICKS(400));
    }
}

/* ── Goertzel single-bin energy ──────────────────────────────────────── */
static float goertzel(const float *buf, int N, float freq)
{
    float omega = 2.0f * (float)M_PI * freq / (float)SAMPLE_RATE;
    float coeff = 2.0f * cosf(omega);
    float s0, s1 = 0.0f, s2 = 0.0f;
    for (int i = 0; i < N; i++) {
        s0 = coeff * s1 - s2 + buf[i];
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

/* ── ADC channel reconfigure (called from mic_task only) ─────────────── */
static void reconfigure_channel(uint8_t cfg_ch)
{
    if (cfg_ch > 7) cfg_ch = 7;
    /* Tear capture down; the next acq_start() bakes the new channel into its
     * conversion pattern.  The oneshot config keeps the debug-panel single
     * reads (mic_read_raw) on the same channel. */
    acq_stop();

    adc_oneshot_chan_cfg_t ccfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_channel_t new_chan = ADC1_CHAN_MAP[cfg_ch];
    if (adc_oneshot_config_channel(s_adc, new_chan, &ccfg) == ESP_OK) {
        s_active_chan = new_chan;
        s_active_ch  = cfg_ch;
        ESP_LOGI(TAG, "ADC channel → CH%u (GPIO%d)", cfg_ch, ADC1_GPIO_MAP[cfg_ch]);
    }
}

/* ── Mic sampling / analysis task ────────────────────────────────────── */
static void mic_task(void *arg)
{
    /* Static buffers — mic_task is single-instance and these would otherwise
     * cost ~3 KB of task stack (rawbuf 1 KB + samples/windowed 1 KB).      */
    static uint8_t rawbuf  [RAW_FRAME_BYTES];   /* 512 × 2 B DMA results    */
    static float   samples [FRAME_SIZE];        /* DC-removed, unwindowed   */
    static float   windowed[FRAME_SIZE];        /* Hann-windowed (Goertzel) */
    float   dc   = 2048.0f;
    float   peak[BAND_COUNT];
    memset(peak, 0, sizeof(peak));

    const float norm = (float)(FRAME_SIZE * FRAME_SIZE) / 4.0f;

    ESP_LOGI(TAG, "mic_task running  (adc_continuous %d Hz ÷%d → %d Hz, DMA-clocked)",
             ADC_HW_RATE, DECIM, SAMPLE_RATE);

    while (1) {
        uint8_t want_ch;
        bool    mic_enabled;
        bool    spectrum_en;
        app_mode_t cur_mode;
        float   silence_gate;

        config_lock();
        const nextube_config_t *cfg = config_get();
        want_ch      = cfg->mic_adc_channel < 8 ? cfg->mic_adc_channel : 7;
        mic_enabled  = cfg->mic_enabled;
        spectrum_en  = (cfg->enabled_modes & (1u << APP_MODE_SPECTRUM)) != 0;
        cur_mode     = cfg->current_mode;
        silence_gate = cfg->mic_silence_gate;
        config_unlock();

        /* ── Capture gate ───────────────────────────────────────────────────
         * Capture only when the mic is enabled AND (Spectrum is on screen OR
         * a calibration is in progress) AND audio playback is not holding the
         * I2S0 peripheral.  Calibration (s_cal_requested) bypasses the mode
         * test so a quiet-room baseline can be captured from any mode. */
        bool want_capture =
            mic_enabled &&
            ((spectrum_en && cur_mode == APP_MODE_SPECTRUM) || s_cal_requested);

        if (!want_capture || s_audio_claims_i2s) {
            acq_stop();
            if (s_audio_claims_i2s && s_i2s_released)
                xSemaphoreGive(s_i2s_released);   /* ack the audio handoff */
            taskENTER_CRITICAL(&s_mux);
            memset(s_bands, 0, sizeof(s_bands));
            taskEXIT_CRITICAL(&s_mux);
            vTaskDelay(pdMS_TO_TICKS(100));   /* poll the gates at 10 Hz */
            continue;
        }

        /* ── Reconfigure ADC channel if debug panel changed it ── */
        if (want_ch != s_active_ch) {
            reconfigure_channel(want_ch);
        }

        if (!acq_start()) {                   /* I2S0 not free yet — retry */
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        /* ── Read one hardware-clocked frame (512 raw samples = 16 ms).
         * The short timeout bounds how fast we react to gate / audio-claim
         * changes; expiry without data just re-evaluates the gates above. */
        uint32_t got = 0;
        if (adc_continuous_read(s_acq, rawbuf, RAW_FRAME_BYTES, &got,
                                100) != ESP_OK || got < RAW_FRAME_BYTES) {
            continue;
        }

        adc_digi_output_data_t *d = (adc_digi_output_data_t *)rawbuf;

        /* Capture geometry, verified on hardware (MICDIAG, since removed):
         * frames arrive at 32 124 Hz (config is honest), and samples come in
         * mildly time-skewed pairs (the I2S stereo slots read the SAR twice
         * in quick succession: ~26 µs + ~37 µs alternating instead of a
         * uniform 31.25 µs).  Simulating the full chain with that exact skew
         * changed band energies by <1 % — harmless, no compensation needed. */

        /* ── Decimate ×4 (average) + DC removal + float conversion ──
         * Averaging four 32 kHz samples to one 8 kHz sample is a crude
         * anti-alias low-pass and gains ~1 bit of effective resolution. */
        for (int i = 0; i < FRAME_SIZE; i++) {
            int acc = 0;
            for (int j = 0; j < DECIM; j++)
                acc += (int)d[i * DECIM + j].type1.data;
            float raw = (float)acc * (1.0f / (float)DECIM);
            s_last_raw  = (int)raw;
            dc          = DC_ALPHA * dc + (1.0f - DC_ALPHA) * raw;
            samples[i]  = raw - dc;
        }

        /* ── Raw + decimated frame export (debug) ──
         * Captured AFTER decimation so both views come from the SAME DMA
         * frame: any divergence between them is the decimation itself.
         * s_dump_dec holds the exact values the Goertzel input is built
         * from (samples[] = decimated − DC), pre-window. */
        if (s_dump_req) {
            for (int i = 0; i < RAW_FRAME_SAMPLES; i++)
                s_dump_buf[i] = (uint16_t)d[i].type1.data;
            for (int i = 0; i < FRAME_SIZE; i++)
                s_dump_dec[i] = samples[i];
            s_dump_req = false;
            if (s_dump_done) xSemaphoreGive(s_dump_done);
        }

        /* ── Hann window for the Goertzel input ──
         * Suppresses rectangular-window leakage (−13 dB sidelobes) that lets
         * one loud off-band tone splatter across every band. */
        for (int i = 0; i < FRAME_SIZE; i++)
            windowed[i] = samples[i] * s_hann[i];

        /* ── Goertzel energy + adaptive noise floor subtraction ── */
        /* Two-phase noise floor estimator:
         *
         *  Phase 1 — calibration (first 250 frames ≈ 4 s):
         *    Fast alpha (0.02) with no signal guard.  Needed because the guard
         *    condition "raw < floor×4" is always false when floor starts at 0 —
         *    the floor would never adapt otherwise.  With α=0.02 the floor
         *    reaches 99 % of the noise level within ~250 frames.
         *
         *  Phase 2 — steady state:
         *    Slow alpha (NOISE_ALPHA = 0.002) only when the current bin is NOT
         *    clearly in signal territory (raw < floor×4).  This lets the floor
         *    track slow drift (temperature, supply) without chasing audio peaks.
         *
         *  In both phases, power[b] = raw − floor, clamped to ≥0.
         *  Result: bars sit at zero in silence with no manual gate tuning. */
        /* Band energy = sum of all 62.5 Hz Goertzel bins inside each band's
         * frequency range (see s_bin_band) — continuous coverage, no cracks.
         * Bands narrower than the bin grid use a centre Goertzel instead. */
        float bandE[BAND_COUNT];
        memset(bandE, 0, sizeof(bandE));
        for (int k = 0; k < SPEC_NBINS; k++) {
            int b = s_bin_band[k];
            if (b >= 0)
                bandE[b] += goertzel(windowed, FRAME_SIZE,
                                     (float)(SPEC_BIN_LO + k) * SPEC_BIN_HZ);
        }
        for (int b = 0; b < BAND_COUNT; b++)
            if (s_band_center_only[b])
                bandE[b] = goertzel(windowed, FRAME_SIZE, BAND_FREQS[b]);

        float max_power = MIC_NOISE_FLOOR;
        float power[BAND_COUNT];
        for (int b = 0; b < BAND_COUNT; b++) {
            float raw = bandE[b] / norm * MIC_GAIN;

            /* Accumulate raw (pre-floor) energy for manual baseline capture */
            if (s_cal_requested) s_cal_accum[b] += raw;

            if (s_noise_cal < PHASE1_FRAMES) {
                /* Phase 1: fast unconstrained convergence */
                s_noise_floor[b] += PHASE1_ALPHA * (raw - s_noise_floor[b]);
            } else if (raw < s_noise_floor[b] * 4.0f) {
                /* Phase 2: slow guarded tracking */
                s_noise_floor[b] += NOISE_ALPHA * (raw - s_noise_floor[b]);
            }
            power[b] = raw - s_noise_floor[b];
            if (power[b] < 0.0f) power[b] = 0.0f;
            power[b] *= s_band_weight[b];   /* spectral tilt — see definition */
        }
        if (s_noise_cal < PHASE1_FRAMES) s_noise_cal++;

        /* ── Temporal smoothing (EMA, τ ≈ 3 frames ≈ 45 ms) ──
         * Computed BEFORE the silence gate so the gate decision uses the
         * smoothed total instead of raw per-frame power.  A single broadband
         * noise burst can spike raw power[b] for one frame and briefly open
         * the gate across all bands, producing a visible multi-tube blip.
         * The EMA attenuates any single-frame transient to ≤35 % of its
         * amplitude, preventing false gate triggers while sustained audio
         * (which builds the EMA to steady state within 2-3 frames) still
         * opens the gate normally. */
        static float s_smooth[BAND_COUNT];
        for (int b = 0; b < BAND_COUNT; b++)
            s_smooth[b] += 0.35f * (power[b] - s_smooth[b]);

        /* ── SPECTRAL silence gate (runtime tuneable via the noise-floor
         * slider; cfg->mic_silence_gate, new semantics) ──
         * Gates on the SUM of smoothed post-floor band power.  Smoothed
         * values suppress single-frame broadband noise spikes that would
         * otherwise briefly illuminate all tubes (the blip artefact).
         * Silence sums to ~0 by construction (adaptive floor absorbed the
         * noise); any real sustained spectral content stands above it.
         * Typical values: silence < 10, quiet tone > 50.
         * Floor adaptation above happens on gated frames too (it must — the
         * floor is learned FROM silence; the old pre-analysis gate starved it). */
        float total_power = 0.0f;
        for (int b = 0; b < BAND_COUNT; b++) total_power += s_smooth[b];

        /* WiFi background-scan RF-coupling spike rejection.
         * The ESP32 WiFi binary scans ~13 channels every ~5 s for roaming.
         * RF switching noise during each scan couples into the ADC and produces
         * a broadband burst appearing as the first frame after a ~3 s HTTP
         * blackout.  Observed spike magnitudes: 400–3800 total_power.
         *
         * Detection signature: previous frame was at moderate-or-lower power
         * (< 200) AND current frame jumps to > 400.  Real audio never makes
         * that jump; even a sudden loud onset builds gradually over multiple
         * EMA frames (α=0.35 means one loud frame only moves s_smooth by 35%).
         * The 200/400 thresholds are calibrated from field recordings:
         *   • max legitimate audio peak seen:   ~190 total_power
         *   • min scan spike seen:              ~533 total_power
         *   • s_prev at spike onset (scans fire during audio, not just silence):
         *     observed range 0–83 in recordings → threshold 200 gives margin.
         * When triggered, reset s_smooth so the spike does not contaminate
         * future EMA state; the silence-gate below then suppresses output. */
        static float s_prev_total = 0.0f;
        if (s_prev_total < 200.0f && total_power > 400.0f) {
            for (int b = 0; b < BAND_COUNT; b++) s_smooth[b] = 0.0f;
            total_power = 0.0f;
        }
        s_prev_total = total_power;

        if (silence_gate > 0.0f && total_power < silence_gate && !s_cal_requested) {
            for (int b = 0; b < BAND_COUNT; b++) peak[b] *= DECAY;
            taskENTER_CRITICAL(&s_mux);
            memset(s_bands, 0, sizeof(s_bands));
            for (int b = 0; b < BAND_COUNT; b++) {
                s_dbg_raw[b]   = bandE[b] / norm * MIC_GAIN;
                s_dbg_floor[b] = s_noise_floor[b];
                s_dbg_power[b] = 0.0f;   /* gated */
            }
            taskEXIT_CRITICAL(&s_mux);
            continue;   /* DMA keeps streaming; next frame in 16 ms */
        }

        /* ── Manual calibration completion ── */
        if (s_cal_requested) {
            s_cal_frame_cnt++;
            if (s_cal_frame_cnt >= MIC_CAL_FRAMES) {
                /* Average the accumulated raw energy into the noise floor */
                taskENTER_CRITICAL(&s_mux);
                for (int b = 0; b < BAND_COUNT; b++)
                    s_noise_floor[b] = s_cal_accum[b] / (float)MIC_CAL_FRAMES;
                s_noise_cal = PHASE1_FRAMES;   /* skip Phase 1 — floor is now precisely set */
                taskEXIT_CRITICAL(&s_mux);
                s_cal_requested = false;
                xSemaphoreGive(s_cal_done);
            }
        }

        /* ── Peak-hold ── */
        for (int b = 0; b < BAND_COUNT; b++) {
            if (s_smooth[b] > peak[b]) peak[b]  = s_smooth[b];
            else                       peak[b] *= DECAY;
            if (peak[b] > max_power) max_power = peak[b];
        }

        /* ── Publish normalised 0.0–1.0 values + debug snapshot ──
         * Perceptual gamma applied at publish (see SPEC_GAMMA) — debug
         * snapshots below stay in linear power units for diagnostics.
         * powf computed outside the critical section (ints stay enabled). */
        float disp_out[BAND_COUNT];
        for (int b = 0; b < BAND_COUNT; b++)
            disp_out[b] = powf(peak[b] / max_power, SPEC_GAMMA);
        taskENTER_CRITICAL(&s_mux);
        for (int b = 0; b < BAND_COUNT; b++) {
            s_bands[b]     = disp_out[b];
            s_dbg_raw[b]   = bandE[b] / norm * MIC_GAIN;
            s_dbg_floor[b] = s_noise_floor[b];
            s_dbg_power[b] = power[b];
        }
        taskEXIT_CRITICAL(&s_mux);

    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void mic_init(void)
{
    config_lock();
    uint8_t cfg_ch = config_get()->mic_adc_channel;
    config_unlock();
    if (cfg_ch > 7) cfg_ch = 7;
    s_active_ch   = cfg_ch;
    s_active_chan  = ADC1_CHAN_MAP[cfg_ch];

    /* Precompute the spectral-tilt weights (see MIC_TILT_EXP). */
    for (int b = 0; b < BAND_COUNT; b++)
        s_band_weight[b] = powf(BAND_FREQS[b] / BAND_FREQS[0], MIC_TILT_EXP);

    /* Precompute the Hann window (see s_hann). */
    for (int i = 0; i < FRAME_SIZE; i++)
        s_hann[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i
                                        / (float)(FRAME_SIZE - 1)));

    /* Map DFT bins → bands by geometric band edges (see s_bin_band). */
    {
        float edge[BAND_COUNT + 1];
        /* Bottom edge wide enough to include the 250 Hz bin: with the
         * tighter /1.06 edge (264 Hz) band 0 owned ZERO bins and bar 1
         * could never light (tone-sweep: 276 Hz showed on bar 2 only). */
        edge[0] = BAND_FREQS[0] / 1.15f;
        for (int b = 1; b < BAND_COUNT; b++)
            edge[b] = sqrtf(BAND_FREQS[b - 1] * BAND_FREQS[b]);
        edge[BAND_COUNT] = BAND_FREQS[BAND_COUNT - 1] * 1.06f;
        for (int k = 0; k < SPEC_NBINS; k++) {
            float f = (float)(SPEC_BIN_LO + k) * SPEC_BIN_HZ;
            s_bin_band[k] = -1;
            for (int b = 0; b < BAND_COUNT; b++)
                if (f >= edge[b] && f < edge[b + 1]) { s_bin_band[k] = (int8_t)b; break; }
        }
        /* Bands that ended up with zero bins fall back to a centre Goertzel. */
        int bincnt[BAND_COUNT] = {0};
        for (int k = 0; k < SPEC_NBINS; k++)
            if (s_bin_band[k] >= 0) bincnt[s_bin_band[k]]++;
        for (int b = 0; b < BAND_COUNT; b++) {
            s_band_center_only[b] = (bincnt[b] == 0);
            if (s_band_center_only[b])
                ESP_LOGI(TAG, "band %d (%.0f Hz): no bins in range — using centre Goertzel",
                         b, (double)BAND_FREQS[b]);
        }
    }

    ESP_LOGI(TAG, "mic_init: CH%u (GPIO%d)", cfg_ch, ADC1_GPIO_MAP[cfg_ch]);

    /* Initialise ADC1 oneshot unit */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_active_chan, &chan_cfg));

    /* Audio↔mic I2S0 handoff ack (see mic_set_audio_active). */
    s_i2s_released = xSemaphoreCreateBinary();
    configASSERT(s_i2s_released);

    /* Raw-frame export completion (see mic_capture_raw_frame). */
    s_dump_done = xSemaphoreCreateBinary();
    configASSERT(s_dump_done);

    /* The adc_continuous capture itself is created on demand by mic_task
     * (gated to Spectrum mode / calibration, released during audio playback)
     * — nothing to start here. */
    ESP_LOGI(TAG, "Capture: adc_continuous %d Hz ÷%d → %d Hz effective — gated to Spectrum mode",
             ADC_HW_RATE, DECIM, SAMPLE_RATE);
}

int mic_read_raw(void)
{
    /* When capture is gated (not in Spectrum mode) the cached value would be
     * stale, so take a fresh one-shot read.  While the continuous capture is
     * running we return the cached value mic_task keeps updating — a oneshot
     * read would contend with the digital controller for the SAR anyway. */
    if (!s_acq && s_adc) {
        int raw = -1;
        if (adc_oneshot_read(s_adc, s_active_chan, &raw) == ESP_OK)
            s_last_raw = raw;
    }
    return s_last_raw;
}

int mic_gpio_num(void)
{
    config_lock();
    uint8_t cfg_ch = config_get()->mic_adc_channel;
    config_unlock();
    if (cfg_ch > 7) cfg_ch = 7;
    return ADC1_GPIO_MAP[cfg_ch];
}

void mic_task_start(void)
{
    /* Create the calibration-done semaphore before starting the task so
     * mic_calibrate() can safely block on it from another task. */
    s_cal_done = xSemaphoreCreateBinary();
    configASSERT(s_cal_done);

    /* Stack 8192: samples[512 B] + peak/power arrays + Goertzel + cosf() call
     * chain (Xtensa software-float trig can use 400–500 B of stack through
     * multiple windowed-register rotations) + FreeRTOS context save (~400 B)
     * + xSemaphoreTake / config_lock call depth.  The original 4096 B was
     * too tight: under field conditions the combined depth triggered a stack
     * overflow that corrupted the saved SP, causing Core 1's SPI flash IPC
     * to hang while waiting for Core 0's (now-corrupted) acknowledgement. */
    if (xTaskCreatePinnedToCore(mic_task, "mic", 8192, NULL, 5, NULL, 0) != pdPASS)
        ESP_LOGE(TAG, "mic_task creation failed");
    else
        ESP_LOGI(TAG, "mic_task started (core 0)");
}

void mic_get_bands(float out[MIC_BAND_COUNT])
{
    taskENTER_CRITICAL(&s_mux);
    memcpy(out, s_bands, BAND_COUNT * sizeof(float));
    taskEXIT_CRITICAL(&s_mux);
}

bool mic_calibrate(float out[MIC_BAND_COUNT], uint32_t timeout_ms)
{
    if (!s_cal_done) {
        ESP_LOGW(TAG, "mic_calibrate: mic not started");
        return false;
    }

    /* Reset accumulators and request calibration atomically */
    taskENTER_CRITICAL(&s_mux);
    memset(s_cal_accum, 0, sizeof(s_cal_accum));
    s_cal_frame_cnt = 0;
    s_cal_requested = true;
    taskEXIT_CRITICAL(&s_mux);

    /* s_cal_requested opens mic_task's capture gate from any mode; the task
     * notices within one of its ~100 ms gate polls and brings the capture up
     * itself (it is the sole owner of the adc_continuous handle).  If audio
     * is playing (I2S0 claimed), capture waits for the clip to end — the
     * caller's timeout below covers that case. */

    /* Block until mic_task signals completion (or timeout) */
    if (xSemaphoreTake(s_cal_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        s_cal_requested = false;
        ESP_LOGW(TAG, "mic_calibrate: timeout after %u ms", (unsigned)timeout_ms);
        return false;
    }

    /* Copy the captured floor values if the caller wants them */
    if (out) {
        taskENTER_CRITICAL(&s_mux);
        memcpy(out, s_noise_floor, BAND_COUNT * sizeof(float));
        taskEXIT_CRITICAL(&s_mux);
    }
    ESP_LOGI(TAG, "mic_calibrate: baseline captured (%d frames)", MIC_CAL_FRAMES);
    return true;
}

void mic_get_band_debug(float raw[MIC_BAND_COUNT], float floor[MIC_BAND_COUNT],
                        float power[MIC_BAND_COUNT], float bands[MIC_BAND_COUNT])
{
    taskENTER_CRITICAL(&s_mux);
    if (raw)   memcpy(raw,   (const void *)s_dbg_raw,   sizeof(s_dbg_raw));
    if (floor) memcpy(floor, (const void *)s_dbg_floor, sizeof(s_dbg_floor));
    if (power) memcpy(power, (const void *)s_dbg_power, sizeof(s_dbg_power));
    if (bands) memcpy(bands, (const void *)s_bands,     sizeof(s_bands));
    taskEXIT_CRITICAL(&s_mux);
}

bool mic_capture_raw_frame(uint16_t out[MIC_RAW_FRAME_SAMPLES], uint32_t timeout_ms)
{
    return mic_capture_frame_pair(out, NULL, timeout_ms);
}

bool mic_capture_frame_pair(uint16_t raw[MIC_RAW_FRAME_SAMPLES],
                            float dec[MIC_FRAME_SAMPLES], uint32_t timeout_ms)
{
    _Static_assert(RAW_FRAME_SAMPLES == MIC_RAW_FRAME_SAMPLES,
                   "header constant out of sync with capture frame size");
    _Static_assert(FRAME_SIZE == MIC_FRAME_SAMPLES,
                   "header constant out of sync with analysis frame size");
    if (!s_dump_done) return false;
    while (xSemaphoreTake(s_dump_done, 0) == pdTRUE) { }   /* drain stale */
    s_dump_req = true;
    if (xSemaphoreTake(s_dump_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        s_dump_req = false;   /* capture not running (not in Spectrum mode?) */
        return false;
    }
    if (raw) memcpy(raw, (const void *)s_dump_buf, sizeof(s_dump_buf));
    if (dec) memcpy(dec, (const void *)s_dump_dec, sizeof(s_dump_dec));
    return true;
}

void mic_reset_calibration(void)
{
    taskENTER_CRITICAL(&s_mux);
    memset(s_noise_floor, 0, sizeof(s_noise_floor));
    s_noise_cal = 0;   /* restart Phase 1 (~4 s convergence) */
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "mic_reset_calibration: noise floor cleared, Phase 1 restarted");
}

void mic_apply_calibration(const float floor[MIC_BAND_COUNT])
{
    taskENTER_CRITICAL(&s_mux);
    memcpy(s_noise_floor, floor, BAND_COUNT * sizeof(float));
    s_noise_cal = PHASE1_FRAMES;   /* skip Phase 1 — saved floor is already accurate */
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "mic_apply_calibration: saved baseline applied, Phase 1 skipped");
}
