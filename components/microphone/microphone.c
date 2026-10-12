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
 *     delivers complete frames; mic_task low-passes and decimates ×2 down to the
 *     16 kHz analysis rate (8 kHz of bandwidth). Exact sample spacing, ~zero CPU per sample — a
 *     software-timed esp_timer + adc_oneshot_read can't sustain uniform
 *     sampling on a loaded RTOS core (see the ADC_HW_RATE comment).
 *   • I2S0 is held for the life of the device once mic_hw_init() claims it,
 *     so capture is never interrupted by an audio clip (see s_acq's
 *     declaration comment).
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
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "mic";

/* ── Sampling parameters ─────────────────────────────────────────────── */
#define SAMPLE_RATE     16000
#define FRAME_SIZE      256             /* samples per Goertzel frame (16 ms) */
#define BAND_COUNT      MIC_BAND_COUNT   /* 24 — must equal MIC_BAND_COUNT in microphone.h */
/* ── Hardware-clocked capture (adc_continuous → I2S0 DMA) ───────────────
 * A software-timed esp_timer + adc_oneshot_read can't hold uniform
 * sampling on a loaded RTOS core: each read costs 300-600us, so a fast
 * timer runs in permanent catch-up (aliasing), and even a fast
 * register-level read still has RTOS preemption jitter that smears
 * high-frequency energy into the low bands. Uniform sampling needs a
 * hardware clock — don't revisit software timing here.
 *
 * adc_continuous clocks the SAR from hardware (I2S0) with DMA delivery:
 * exact sample spacing, ~zero CPU per sample. The ESP32 digital ADC
 * controller's minimum rate is 20kHz, so capture runs at 32kHz and
 * averages every 4 samples down to the 8kHz design rate — doubling as a
 * crude anti-alias filter and adding ~1 bit SNR.
 *
 * I2S0 is ours exclusively (see s_acq's declaration comment), so the
 * handle is claimed once at boot and held for the life of the device. */
#define ADC_HW_RATE        32000
#define DECIM              2                       /* 32 kHz → 16 kHz       */
#define RAW_FRAME_SAMPLES  (FRAME_SIZE * DECIM)    /* 512 raw per frame     */
#define RAW_FRAME_BYTES    (RAW_FRAME_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES)

/* Per-frame filter constants at the native 62.5 fps frame rate:
 *   FALL_DECAY[] peak-hold fall per frame (user "Bar fall speed")
 *   PHASE1_*     fast noise-floor convergence: 250 frames ≈ 4 s
 *   NOISE_ALPHA  steady-state floor tracking, τ ≈ 8 s                     */
#define PHASE1_FRAMES   250
#define PHASE1_ALPHA    0.02f
#define DC_ALPHA        0.9995f   /* ~125 ms at the 16 kHz analysis rate */

/* Attack/release smoothing for s_disp_smooth[] (see the temporal-smoothing
 * block in mic_task).  This is the display/peak-hold path ONLY — the gate's
 * own s_gate_smooth[] keeps the original single symmetric alpha (0.35,
 * tau ~45 ms) unchanged; see the comment at s_gate_smooth's use for why.
 * SMOOTH_ATTACK reacts to a rising band almost immediately (tau ~1 frame,
 * ~16-24 ms) for snappier response; SMOOTH_RELEASE is slower (tau ~5-6
 * frames, ~90 ms) so it averages out noise-floor fluctuations on the way
 * down instead of tracking every wiggle. */
/* The user's "Bar rise speed" / "Bar fall speed" settings (1-10, default 5) pick from these: the rise level sets the attack
 * of the per-band smoothing; the fall level sets both its release and the peak-hold fall per frame. Level 5 = the
 * original SMOOTH_ATTACK 0.65 / SMOOTH_RELEASE 0.18 / DECAY 0.85. */
static const float RISE_ATTACK[10]  = { 0.08f, 0.14f, 0.25f, 0.42f, 0.65f, 0.78f, 0.88f, 0.94f, 0.98f, 1.00f };
static const float FALL_RELEASE[10] = { 0.04f, 0.06f, 0.09f, 0.13f, 0.18f, 0.25f, 0.35f, 0.50f, 0.70f, 0.90f };
static const float FALL_DECAY[10]   = { 0.97f, 0.95f, 0.93f, 0.89f, 0.85f, 0.80f, 0.74f, 0.66f, 0.55f, 0.40f };

/* ── Sensitivity (compile-time) ──────────────────────────────────────── */
#define MIC_GAIN        1.0f    /* LMV321 handles hardware gain */
#define MIC_NOISE_FLOOR 1.0f   /* Goertzel normalisation floor for active frames */

/* ── Frequency bands ─────────────────────────────────────────────────── */
/* 24 log-spaced bands, grouped 4 per tube (tube 1 = lowest ... tube 6 = highest), running from 280 Hz up
 * to a user-set top frequency (cfg->mic_top_hz, 1200-8000 Hz, default 8000). On this hardware the mic's
 * response is weak above ~1.7 kHz, so the default packs the usable range across all six tubes instead of
 * reserving the top two for 2-4 kHz. Avoids the 125-250 Hz range (SPI switching noise / mains hum).
 * Rebuilt by mic_build_bands() whenever the setting changes. */
#define BAND_LOWEST_HZ  280.0f
static float    BAND_FREQS[BAND_COUNT];          /* band centres in the ACTIVE layout, Hz */
static uint16_t s_built_top_hz = 0;
static float    s_built_mask = -1.0f, s_built_rlo = -1.0f, s_built_rhi = -1.0f, s_built_tilt = 1e9f;
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
#define SPEC_BIN_HI   127                     /* 7937 Hz (< 8 kHz Nyquist) */
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
static adc_oneshot_unit_handle_t  s_adc        = NULL;
static volatile adc_channel_t     s_active_chan = ADC_CHANNEL_7;
static volatile uint8_t           s_active_ch  = 7;   /* config index */

/* ── Continuous-capture state ────────────────────────────────────────── */
/* The adc_continuous handle is created at boot (mic_hw_init) and lives for
 * the device's lifetime, same as the oneshot unit s_adc — started and
 * stopped per Spectrum-mode session, never deinit'd/recreated.
 * adc_continuous_config() is safe to call repeatedly on the same handle (it
 * just memcpy's into an already-allocated pattern buffer), so a channel
 * change only needs stop → config → start, not a full recreate.
 *
 * s_acq is therefore non-NULL for the whole run after mic_hw_init(), and
 * s_acq_running (not s_acq's NULLness) is the "is capture active" flag.
 * mic_task is the sole owner — no handle lifecycle races.
 *
 * Keep this handle held for the device's lifetime: releasing it means
 * calling adc_continuous_new_handle() again at runtime, which can ABORT
 * the device from inside itself when memory is tight (see
 * mic_hw_init()'s comment). Audio playback no longer needs I2S0 (it clocks
 * dac_oneshot from a gptimer instead), so nothing currently forces a
 * release — don't reintroduce one without checking that comment first. */
static adc_continuous_handle_t s_acq               = NULL;
static volatile bool           s_acq_running        = false;

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
static volatile bool s_hold_capture = false;   /* see mic_hold_capture() */
/* Last three frames of decimated audio for the oscilloscope view. PSRAM, allocated by the task: 1.5 KB of static internal
 * RAM is too much on this heap. Not locked: a read racing the 16 ms update at worst draws one slightly torn frame. */
static int16_t *s_scope;
/* Edge-triggered beat-onset flag — set by the detector below, consumed (and
 * cleared) by mic_get_beat_pulse(). See mic_get_beat_pulse()'s comment. */
static volatile bool s_beat_pulse = false;

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
 * remain valid — the tilt shapes only the displayed power, NOT the silence gate or the
 * Wi-Fi spike guard: weighting those too let treble noise (x28 at 8 kHz for tilt 1) open the gate
 * on its own (idle: 63 % of samples lit at tilt 1, against 8 % at tilt 0). */
/* ── Display gamma (perceptual bar scaling) ─────────────────────────────
 * Published band values are (peak/max)^SPEC_GAMMA instead of the linear
 * power ratio.  Music's loudest band (bass) carries 10–20× the power of
 * the mids; linearly mapped, that pins one tube at full scale and leaves
 * the rest at one or two segments.  γ = 0.6: a band at 10 % of max displays at
 * ~25 % height — bass still clearly leads and its hits reach full height, while
 * mids and treble live (0.45 lifted the quiet bands more but flattened the
 * spectrum and softened beats).  1.0 = linear; lower = flatter/more compressed. */
#define SPEC_GAMMA  0.6f

/* ── Pre-gamma squelch (kills near-zero-ratio noise flicker) ────────────
 * SPEC_GAMMA's whole job is to boost quiet-but-real bands into visibility,
 * but it boosts quiet-but-NOT-real ones just as eagerly: a band sitting at
 * 1 % of the loudest band's peak (pure noise-floor jitter once the sum-gate
 * is open because something elsewhere is playing) still renders
 * 0.01^0.45 ≈ 0.15 — two lit segments of nothing.  Squelch remaps the
 * bottom SPEC_SQUELCH_RATIO of the ratio range to a hard 0 and rescales the
 * remainder back to [0,1] before applying gamma, so the compression curve
 * is unchanged for anything above the squelch line and there's no visible
 * jump at the boundary (ratio == squelch maps continuously to 0). */
#define SPEC_SQUELCH_RATIO  0.01f

/* Per-band scaling. The mic hears a typical track far stronger in the low bands than higher up (measured:
 * band 0 at power ~300 vs ~4 in the mids), so scaling every bar to the loudest band left tubes 2-6 below the
 * squelch and dark. Each band is now scaled to REF_GAIN x its own recent average level (fast up, slow down:
 * a peak envelope was tried first, but rare loud peaks inflated it and kept the mids small), blended with the
 * loudest band's by SPEC_EQ (1 = every band against its own level, 0 = the old all-against-the-loudest), and
 * never to less than REF_SNR times the band's own noise threshold so idle noise stays small. */
#define SPEC_EQ     0.95f
#define REF_UP      0.01f
#define REF_DOWN    0.002f
#define REF_GAIN    2.0f
#define REF_SNR     4.0f

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
/* The floor is a MEAN, but a quiet room's band energy is bursty (measured: median below the mean, 99th
 * percentile 5x the mean in the low bands and 20x+ higher up), so bursts ride well above it and, being
 * normalised to the loudest band, light the bars. Each band therefore needs to exceed floor x (1 + this)
 * before it counts, which scales with how noisy that band is (the low bands are the noisiest). */
#define NOISE_DEADZONE  1.0f
/* The bars use a smaller dead zone than the gate: in a typical room the mids and treble of music sit barely above the
 * noise, so a 2x-floor cut-off left tubes 3-6 mostly dark. The gate keeps NOISE_DEADZONE, so silence still blanks them. */
#define DISP_DEADZONE   0.25f

/* Broadband bursts: a frame in which at least BURST_BARS of the bars above the bass jump to (about) their maximum at once is
 * noise, not music (the silence gate hides them in a quiet room; with music on they flashed tubes 1-4 together). Such a frame
 * is dropped, but never more than BURST_MAX_RUN in a row, so genuinely loud sound still gets through. BURST_LEVEL is the display height (fraction of full)
 * that counts as "at max"; the matching smoothed-power threshold follows the gamma setting. */
#define BURST_BARS      10
#define BURST_FIRST_BAND 4
#define BURST_LEVEL     0.85f
#define BURST_MAX_RUN   2


/* Phase 1 frame counter — promoted to file scope so mic_reset_calibration()
 * and mic_apply_calibration() can manipulate it without touching mic_task. */
static int s_noise_cal = 0;   /* counts frames during Phase 1; PHASE1_FRAMES = steady state */

/* Quiet reference per band: the floor as it stood when Phase 1 ended, or as captured/applied by a baseline.
 * The adaptive floor may drift up to FLOOR_CAP_RATIO x this and follows a quieter room down, but sustained
 * LOUD sound can no longer drag it up to the music's own level (measured with a bass-heavy track playing:
 * the floor rose from 8-9 to 189/74/49 on the lowest bands, so the bars subtracted the music itself and the
 * dead zone, which scales with the floor, swallowed every beat). A permanently noisier room needs a
 * new baseline capture. */
static float s_noise_ref[BAND_COUNT];
#define FLOOR_CAP_RATIO  2.0f
#define FLOOR_REF_MIN    0.02f

/* ── Manual baseline calibration state ──────────────────────────────── */
static volatile bool     s_cal_requested = false;  /* set by mic_calibrate() */
static volatile bool     s_test_capture  = false;  /* set by mic_set_test_capture() */

/* ── Accent-LED PWM ripple: skipped, not compensated ───────────────────────
 * The WS2812 chips dim by internal PWM (~1.27 kHz), and its ripple couples into the mic in a few NARROW
 * frequency ranges whatever the colour or brightness (measured with tools/led_noise_sweep.py: 99 % of it
 * in 1.19-1.31 kHz, with weaker lines at 2.3-3.2 kHz and 3.6-4 kHz). How much each colour adds is not
 * predictable (the three channels' ripple adds with phase offsets: white came out 2x below the sum of its
 * channels, cyan at 50 % 5x above it), and predicting/subtracting it cost the bars their snap. So those
 * ranges are simply not analysed: a band whose bins all fall inside one is masked and shown as the
 * average of its neighbours, so the display stays contiguous and the LEDs cannot reach it. */
#define RIPPLE0_LO  1170.0f   /* the fundamental; editable live from the diagnostics panel */
#define RIPPLE0_HI  1330.0f
static float LED_RIPPLE_HZ[][2] = { { RIPPLE0_LO, RIPPLE0_HI }, { 2290.0f, 3210.0f }, { 3640.0f, 3880.0f } };
#define LED_RIPPLE_RANGES  ((int)(sizeof(LED_RIPPLE_HZ) / sizeof(LED_RIPPLE_HZ[0])))
static bool  s_mask_on = true;
static bool s_band_masked[BAND_COUNT];

void mic_get_band_freqs(float out[MIC_BAND_COUNT])
{
    for (int b = 0; b < BAND_COUNT; b++) out[b] = BAND_FREQS[b];
}

void mic_get_band_masked(bool out[MIC_BAND_COUNT])
{
    for (int b = 0; b < BAND_COUNT; b++) out[b] = s_band_masked[b];
}
static volatile uint32_t s_test_until_ms = 0;      /* esp_timer ms deadline for the above */
static int               s_cal_frame_cnt = 0;      /* frames accumulated so far */
static float             s_cal_accum[BAND_COUNT];  /* raw Goertzel accumulator   */
static SemaphoreHandle_t s_cal_done      = NULL;   /* binary; given when done    */

/* Last raw ADC count (updated by mic_task per frame) for the debug panel */
static volatile int s_last_raw = -1;

/* ── Capture lifecycle (mic_task is the sole owner of s_acq) ─────────── */

/* (Re)configure + start hardware-clocked capture on the active channel.
 * s_acq is created once at boot (mic_hw_init) and lives for the run — this
 * only starts/reconfigures it, never allocates or frees the handle.
 *
 * Returns false (and logs, rate-limited) on any failure; the caller just
 * retries on its next loop pass. Logs the actual esp_err_t from each step so
 * a persistent failure is diagnosable instead of just "will retry". */
static bool acq_start(void)
{
    if (s_acq_running) return true;

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
    esp_err_t cerr = adc_continuous_config(s_acq, &ccfg);
    esp_err_t serr = (cerr == ESP_OK) ? adc_continuous_start(s_acq) : ESP_FAIL;
    if (cerr != ESP_OK || serr != ESP_OK) {
        if (cerr == ESP_OK && serr != ESP_OK) {
            /* config succeeded but start failed — best-effort defensive stop
             * so a retried acq_start() doesn't compound partial state on top
             * of a handle ESP-IDF may still consider partially started. */
            adc_continuous_stop(s_acq);
        }
        static int64_t s_last_fail_us = 0;
        int64_t now = esp_timer_get_time();
        if (now - s_last_fail_us > 10LL * 1000 * 1000) {
            ESP_LOGW(TAG, "acq_start: config=%s start=%s — will retry",
                     esp_err_to_name(cerr),
                     cerr == ESP_OK ? esp_err_to_name(serr) : "skipped");
            s_last_fail_us = now;
        }
        return false;
    }
    s_acq_running = true;
    return true;
}

static void acq_stop(void)
{
    if (!s_acq_running) return;
    esp_err_t err = adc_continuous_stop(s_acq);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "acq_stop: adc_continuous_stop failed (%s) — driver still running, "
                       "leaving s_acq_running set", esp_err_to_name(err));
        return;
    }
    s_acq_running = false;
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

/* ── Live tuning (hidden diagnostics panel) ─────────────────────────────────
 * The #defines in this file are the defaults; this table is the working copy, changed at runtime and read
 * by mic_task once per frame. Not persisted. */
#define SPIKE_LO     200.0f   /* Wi-Fi spike guard: previous smoothed total below this ...        */
#define SPIKE_HI     400.0f   /* ... and this one above this = a suspect jump                      */
#define SPIKE_RATIO  0.25f    /* ... confirmed a spike if next frame's power is below this x it    */
static const mic_tune_desc_t s_tune_desc[MT_COUNT] = {
    [MT_EQ]        = { "eq",        SPEC_EQ,         0.0f,    1.0f   },
    [MT_GAIN]      = { "gain",      REF_GAIN,        0.25f,   20.0f  },
    [MT_UP]        = { "up",        REF_UP,          0.0005f, 0.2f   },
    [MT_DOWN]      = { "down",      REF_DOWN,        0.0001f, 0.05f  },
    [MT_SNR]       = { "snr",       REF_SNR,         0.0f,    50.0f  },
    [MT_DEADZONE]  = { "deadzone",  NOISE_DEADZONE,  0.0f,    10.0f  },
    [MT_SQUELCH]   = { "squelch",   SPEC_SQUELCH_RATIO, 0.0f, 0.5f   },
    [MT_GMODE]     = { "gmode",     3.0f,            0.0f,    3.0f   },   /* 0 sum of power, 1 per-band SNR units, 2 = 1 + 2-of-3, 3 = units that persist */
    [MT_GSNR]      = { "gsnr",      2.0f,            0.5f,    100.0f },
    [MT_GCAP]      = { "gcap",      8.0f,            1.0f,    100.0f },
    [MT_GTHR]      = { "gthr",      0.5f,            0.05f,   20.0f  },
    [MT_GUMIN]     = { "gumin",     1.5f,            0.0f,    20.0f  },   /* mode 3: a band counts once above this many units ... */
    [MT_GRUN]      = { "grun",      5.0f,            1.0f,    60.0f  },   /* ... for this many frames in a row */
    [MT_GHK]       = { "ghk",       0.15f,           0.0f,    2.0f   },   /* mode 3 hold = ghk x frames open so far ... */
    [MT_GHM]       = { "ghm",       16.0f,           0.0f,    120.0f },   /* ... capped at this many frames (16 = 256 ms) */
    [MT_TILT]      = { "tilt",      MIC_TILT_EXP,    -0.5f,   2.0f   },
    [MT_NALPHA]    = { "nalpha",    NOISE_ALPHA,     0.0002f, 0.05f  },
    [MT_SPIKE]     = { "spike",     1.0f,            0.0f,    1.0f   },
    [MT_SPIKE_LO]  = { "spike_lo",  SPIKE_LO,        20.0f,   2000.0f },
    [MT_SPIKE_HI]  = { "spike_hi",  SPIKE_HI,        50.0f,   5000.0f },
    [MT_SPIKE_RATIO] = { "spike_ratio", SPIKE_RATIO, 0.02f,   1.0f   },
    [MT_MASK]      = { "mask",      1.0f,            0.0f,    1.0f   },
    [MT_RIP_LO]    = { "rip_lo",    RIPPLE0_LO,      250.0f,  7900.0f },
    [MT_RIP_HI]    = { "rip_hi",    RIPPLE0_HI,      250.0f,  8000.0f },
    [MT_DZDISP]    = { "dzdisp",    DISP_DEADZONE,   0.0f,    10.0f  },
    [MT_GAMMA]     = { "gamma",     SPEC_GAMMA,      0.2f,    1.0f   },
    [MT_BURSTN]    = { "burstn",    (float)BURST_BARS, 0.0f,  24.0f  },
};
static float s_tune[MT_COUNT];
static bool  s_tune_init = false;
static uint32_t s_st_frames, s_st_fail, s_st_gated, s_st_holds;
static float    s_st_gate_val, s_st_gate_thr;
static uint32_t s_st_eval, s_st_open[9];

/* Per-frame raw band-energy trace (see microphone.h). */
static uint16_t        *s_tr_buf = NULL;
static int              s_tr_cap = 0;
static volatile int     s_tr_n = 0;
static volatile bool    s_tr_active = false;
static float            s_tr_floor[BAND_COUNT];


static void tune_init_once(void)
{
    if (s_tune_init) return;
    for (int i = 0; i < MT_COUNT; i++) s_tune[i] = s_tune_desc[i].def;
    s_tune_init = true;
}
const mic_tune_desc_t *mic_tune_desc(void) { return s_tune_desc; }
void mic_tune_get(float out[MT_COUNT]) { tune_init_once(); for (int i = 0; i < MT_COUNT; i++) out[i] = s_tune[i]; }
void mic_tune_set(const float in[MT_COUNT])
{
    tune_init_once();
    for (int i = 0; i < MT_COUNT; i++) {
        float v = in[i];
        if (v < s_tune_desc[i].lo) v = s_tune_desc[i].lo;
        if (v > s_tune_desc[i].hi) v = s_tune_desc[i].hi;
        s_tune[i] = v;
    }
}
void mic_tune_reset(void) { tune_init_once(); for (int i = 0; i < MT_COUNT; i++) s_tune[i] = s_tune_desc[i].def; }
void mic_trace_free(void)
{
    s_tr_active = false;
    vTaskDelay(pdMS_TO_TICKS(40));   /* let mic_task finish a frame it may be writing */
    uint16_t *b = s_tr_buf;
    s_tr_buf = NULL; s_tr_cap = 0; s_tr_n = 0;
    if (b) heap_caps_free(b);
}

bool mic_trace_start(int frames)
{
    if (frames < 1) return false;
    if (frames > 1500) frames = 1500;
    mic_trace_free();
    uint16_t *b = heap_caps_malloc((size_t)frames * BAND_COUNT * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) return false;
    s_tr_buf = b; s_tr_cap = frames; s_tr_n = 0;
    s_tr_active = true;
    return true;
}

void mic_trace_info(int *n, int *cap, bool *active)
{
    if (n) *n = s_tr_n;
    if (cap) *cap = s_tr_cap;
    if (active) *active = s_tr_active;
}

bool mic_trace_read(int frame, uint16_t out[MIC_BAND_COUNT])
{
    if (!s_tr_buf || s_tr_active || frame < 0 || frame >= s_tr_n) return false;
    memcpy(out, s_tr_buf + (size_t)frame * BAND_COUNT, BAND_COUNT * sizeof(uint16_t));
    return true;
}

void mic_trace_floor(float out[MIC_BAND_COUNT])
{
    for (int b = 0; b < BAND_COUNT; b++) out[b] = s_tr_floor[b];
}

void mic_get_stats(mic_stats_t *st)
{
    st->frames = s_st_frames; st->read_fail = s_st_fail; st->gated = s_st_gated;
    st->spike_holds = s_st_holds; st->adc_last = s_last_raw;
    st->gate_val = s_st_gate_val; st->gate_thr = s_st_gate_thr;
    st->gate_eval = s_st_eval;
    for (int i = 0; i < 9; i++) st->gate_open[i] = s_st_open[i];
}

/* Band edges: geometric midpoints between centres; the bottom edge is wide enough to include the 250 Hz
 * bin (with a tighter /1.06 edge band 0 owned zero bins and bar 1 could never light). */
static void band_edges(const float *centres, float edge[BAND_COUNT + 1])
{
    edge[0] = centres[0] / 1.15f;
    for (int b = 1; b < BAND_COUNT; b++)
        edge[b] = sqrtf(centres[b - 1] * centres[b]);
    edge[BAND_COUNT] = centres[BAND_COUNT - 1] * 1.06f;
}

/* (Re)build everything that depends on the band layout: centres, tilt weights, the bin->band map and
 * which bands sit inside an accent-LED ripple range (see LED_RIPPLE_HZ). */
static bool in_ripple_range(float hz, float margin)
{
    if (!s_mask_on) return false;
    for (int r = 0; r < LED_RIPPLE_RANGES; r++)
        if (hz >= LED_RIPPLE_HZ[r][0] - margin && hz <= LED_RIPPLE_HZ[r][1] + margin) return true;
    return false;
}

static void mic_build_bands(uint16_t want_hz)
{
    s_built_top_hz = want_hz;   /* what was asked for, so an out-of-range value can't retrigger a rebuild */
    float tv[MT_COUNT];
    mic_tune_get(tv);
    s_mask_on = tv[MT_MASK] >= 0.5f;
    LED_RIPPLE_HZ[0][0] = tv[MT_RIP_LO];
    LED_RIPPLE_HZ[0][1] = tv[MT_RIP_HI] > tv[MT_RIP_LO] ? tv[MT_RIP_HI] : tv[MT_RIP_LO];
    s_built_mask = tv[MT_MASK]; s_built_rlo = tv[MT_RIP_LO]; s_built_rhi = tv[MT_RIP_HI]; s_built_tilt = tv[MT_TILT];
    const uint16_t top_hz = want_hz < 1200 ? 1200 : (want_hz > 8000 ? 8000 : want_hz);
    const float ratio = (float)top_hz / BAND_LOWEST_HZ;
    for (int b = 0; b < BAND_COUNT; b++) {
        BAND_FREQS[b]    = BAND_LOWEST_HZ * powf(ratio, (float)b / (float)(BAND_COUNT - 1));
        s_band_weight[b] = powf(BAND_FREQS[b] / BAND_FREQS[0], tv[MT_TILT]);
    }

    float edge[BAND_COUNT + 1];
    band_edges(BAND_FREQS, edge);

    int n_all[BAND_COUNT] = {0}, n_ok[BAND_COUNT] = {0};
    for (int k = 0; k < SPEC_NBINS; k++) {
        const float f = (float)(SPEC_BIN_LO + k) * SPEC_BIN_HZ;
        s_bin_band[k] = -1;
        for (int b = 0; b < BAND_COUNT; b++)
            if (f >= edge[b] && f < edge[b + 1]) {
                n_all[b]++;
                if (!in_ripple_range(f, 0.0f)) { s_bin_band[k] = (int8_t)b; n_ok[b]++; }
                break;
            }
    }
    int centre_only = 0, masked = 0;
    for (int b = 0; b < BAND_COUNT; b++) {
        s_band_center_only[b] = (n_all[b] == 0);   /* narrower than one bin: one Goertzel at the centre */
        centre_only += s_band_center_only[b];
        /* A centre Goertzel leaks ~1.5 bins either side, so it counts as inside a range from that close. */
        s_band_masked[b] = s_band_center_only[b] ? in_ripple_range(BAND_FREQS[b], 1.5f * SPEC_BIN_HZ)
                                                 : (n_ok[b] == 0);
        masked += s_band_masked[b];
    }

    ESP_LOGI(TAG, "bands: 280..%u Hz, %d of 24 narrower than one 62.5 Hz bin (centre-only), %d masked (LED ripple)",
             (unsigned)top_hz, centre_only, masked);
}

/* ── Mic sampling / analysis task ────────────────────────────────────── */
static void mic_task(void *arg)
{
    /* Static buffers — mic_task is single-instance and these would otherwise
     * cost ~3 KB of task stack (rawbuf 1 KB + samples/windowed 1 KB).      */
    static uint8_t rawbuf  [RAW_FRAME_BYTES];   /* 512 × 2 B DMA results    */
    static float   samples [FRAME_SIZE];        /* DC-removed; Hann-windowed in place before the Goertzel */
    float   dc   = 2048.0f;
    float   peak[BAND_COUNT];
    float   ref [BAND_COUNT];   /* per-band running average level (see SPEC_EQ) */
    memset(peak, 0, sizeof(peak));
    memset(ref,  0, sizeof(ref));
    bool    smooth_reset = false;   /* set when the band layout changes */

    const float norm = (float)(FRAME_SIZE * FRAME_SIZE) / 4.0f;

    ESP_LOGI(TAG, "mic_task running  (adc_continuous %d Hz ÷%d → %d Hz, DMA-clocked)",
             ADC_HW_RATE, DECIM, SAMPLE_RATE);

    while (1) {
        uint8_t want_ch;
        bool    mic_enabled;
        bool    spectrum_en;
        app_mode_t cur_mode;
        float   silence_gate;
        uint16_t top_hz;
        uint8_t rise_lv, fall_lv;
        float   tv[MT_COUNT];   /* live tuning snapshot for this pass */

        mic_tune_get(tv);
        config_lock();
        const nextube_config_t *cfg = config_get();
        want_ch      = cfg->mic_adc_channel < 8 ? cfg->mic_adc_channel : 7;
        mic_enabled  = cfg->mic_enabled;
        spectrum_en  = (cfg->enabled_modes & (1u << APP_MODE_SPECTRUM)) != 0;
        cur_mode     = cfg->current_mode;
        silence_gate = cfg->mic_silence_gate;
        top_hz       = cfg->mic_top_hz;
        rise_lv      = cfg->spectrum_bar_rise;
        fall_lv      = cfg->spectrum_bar_fall;
        config_unlock();
        const float bar_attack  = RISE_ATTACK [(rise_lv < 1 ? 1 : (rise_lv > 10 ? 10 : rise_lv)) - 1];
        const float bar_release = FALL_RELEASE[(fall_lv < 1 ? 1 : (fall_lv > 10 ? 10 : fall_lv)) - 1];
        const float bar_decay   = FALL_DECAY  [(fall_lv < 1 ? 1 : (fall_lv > 10 ? 10 : fall_lv)) - 1];

        /* ── Capture gate ───────────────────────────────────────────────────
         * Capture only when the mic is enabled AND (Spectrum is on screen OR
         * a calibration is in progress) AND audio playback is not holding the
         * I2S0 peripheral.  Calibration (s_cal_requested) bypasses the mode
         * test so a quiet-room baseline can be captured from any mode. */
        /* Range setting changed: new band layout, so the learned noise floor no longer lines up with it. */
        if (tv[MT_TILT] != s_built_tilt) {   /* tilt only changes the band weights */
            for (int b = 0; b < BAND_COUNT; b++)
                s_band_weight[b] = powf(BAND_FREQS[b] / BAND_FREQS[0], tv[MT_TILT]);
            s_built_tilt = tv[MT_TILT];
        }
        if (top_hz != s_built_top_hz || tv[MT_MASK] != s_built_mask ||
            tv[MT_RIP_LO] != s_built_rlo || tv[MT_RIP_HI] != s_built_rhi) {
            mic_build_bands(top_hz);
            taskENTER_CRITICAL(&s_mux);
            memset(s_noise_floor, 0, sizeof(s_noise_floor));
            s_noise_cal = 0;
            taskEXIT_CRITICAL(&s_mux);
            memset(peak, 0, sizeof(peak));
            memset(ref,  0, sizeof(ref));
            smooth_reset = true;
        }
        if (s_test_capture &&
            (int32_t)((uint32_t)(esp_timer_get_time() / 1000) - s_test_until_ms) >= 0)
            s_test_capture = false;   /* test window over */
        bool want_capture =
            mic_enabled &&
            !s_hold_capture &&
            ((spectrum_en && cur_mode == APP_MODE_SPECTRUM) || s_cal_requested || s_test_capture);

        if (!want_capture) {
            /* Stop converting but KEEP the handle: re-running the ~10 KB
             * DMA allocation at runtime is the failure mode mic_hw_init()
             * exists to avoid. */
            acq_stop();
            taskENTER_CRITICAL(&s_mux);
            memset(s_bands, 0, sizeof(s_bands));
            taskEXIT_CRITICAL(&s_mux);
            /* Capture is gated off — clear peak-hold too so re-entering
             * Spectrum mode starts from a clean state instead of displaying
             * against a stale peak from potentially minutes earlier. */
            memset(peak, 0, sizeof(peak));
            memset(ref,  0, sizeof(ref));
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
            s_st_fail++;
            continue;
        }
        s_st_frames++;

        adc_digi_output_data_t *d = (adc_digi_output_data_t *)rawbuf;

        /* Capture geometry, verified on hardware (MICDIAG, since removed):
         * frames arrive at 32 124 Hz (config is honest), and samples come in
         * mildly time-skewed pairs (the I2S stereo slots read the SAR twice
         * in quick succession: ~26 µs + ~37 µs alternating instead of a
         * uniform 31.25 µs).  Simulating the full chain with that exact skew
         * changed band energies by <1 % — harmless, no compensation needed. */

        /* ── Decimate ×2 (32 → 16 kHz) with a 4-tap [1 3 3 1]/8 low-pass + DC removal ──
         * The kernel's gain is cos^3(pi f / 32 kHz): -0.5 dB at 2 kHz, -2 dB at 4, -4.8 at 6, -9 at 8 kHz
         * (the mic is already weak up there), while 9 / 10 / 12 kHz fold back to 7 / 6 / 4 kHz attenuated
         * by 12 / 15 / 25 dB. Two samples of history carry over from the previous frame. */
        static int s_hist_a = 2048, s_hist_b = 2048;
        {
            int a = s_hist_a, b = s_hist_b;
            for (int i = 0; i < FRAME_SIZE; i++) {
                const int c = (int)d[2 * i].type1.data, e = (int)d[2 * i + 1].type1.data;
                const float raw = (float)(a + 3 * b + 3 * c + e) * 0.125f;
                a = c; b = e;
                s_last_raw  = (int)raw;
                dc          = DC_ALPHA * dc + (1.0f - DC_ALPHA) * raw;
                samples[i]  = raw - dc;
            }
            s_hist_a = a; s_hist_b = b;
        }
        if (!s_scope) s_scope = heap_caps_calloc(MIC_SCOPE_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_scope) {
            memmove(s_scope, s_scope + FRAME_SIZE, (MIC_SCOPE_SAMPLES - FRAME_SIZE) * sizeof(int16_t));
            for (int i = 0; i < FRAME_SIZE; i++) {
                const float v = samples[i];
                s_scope[MIC_SCOPE_SAMPLES - FRAME_SIZE + i] = (int16_t)(v > 32767.0f ? 32767 : (v < -32768.0f ? -32768 : (int)v));
            }
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
            samples[i] *= s_hann[i];

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
                bandE[b] += goertzel(samples, FRAME_SIZE,
                                     (float)(SPEC_BIN_LO + k) * SPEC_BIN_HZ);
        }
        for (int b = 0; b < BAND_COUNT; b++)
            if (s_band_center_only[b] && !s_band_masked[b])
                bandE[b] = goertzel(samples, FRAME_SIZE, BAND_FREQS[b]);

        float power[BAND_COUNT], pdisp[BAND_COUNT];   /* pdisp: same with the bars' own dead zone */
        const bool tracing = s_tr_active && s_tr_buf && s_tr_n < s_tr_cap;
        if (tracing && s_tr_n == 0) memcpy(s_tr_floor, s_noise_floor, sizeof(s_tr_floor));
        for (int b = 0; b < BAND_COUNT; b++) {
            const float raw = bandE[b] / norm * MIC_GAIN;   /* 0 for a masked band */
            if (tracing) {
                const float q = sqrtf(raw) * 100.0f + 0.5f;
                s_tr_buf[(size_t)s_tr_n * BAND_COUNT + b] = (uint16_t)(q > 65535.0f ? 65535.0f : q);
            }

            /* Accumulate raw (pre-floor) energy for manual baseline capture.
             * Critical section guards against a concurrent mic_calibrate()
             * timeout-then-retry resetting s_cal_accum mid-write (see
             * mic_calibrate()'s timeout path). */
            if (s_cal_requested) {
                taskENTER_CRITICAL(&s_mux);
                s_cal_accum[b] += raw;
                taskEXIT_CRITICAL(&s_mux);
            }

            if (s_noise_cal < PHASE1_FRAMES) {
                /* Phase 1: fast unconstrained convergence */
                s_noise_floor[b] += PHASE1_ALPHA * (raw - s_noise_floor[b]);
            } else if (raw < s_noise_floor[b] * 4.0f) {
                /* Phase 2: slow guarded tracking */
                s_noise_floor[b] += tv[MT_NALPHA] * (raw - s_noise_floor[b]);
            }
            if (s_noise_cal >= PHASE1_FRAMES) {
                const float cap = s_noise_ref[b] * FLOOR_CAP_RATIO;
                if (s_noise_floor[b] > cap) s_noise_floor[b] = cap;
                if (s_noise_floor[b] < s_noise_ref[b]) s_noise_ref[b] = s_noise_floor[b] > FLOOR_REF_MIN ? s_noise_floor[b] : FLOOR_REF_MIN;
            }
            power[b] = raw - s_noise_floor[b] * (1.0f + tv[MT_DEADZONE]);
            pdisp[b] = raw - s_noise_floor[b] * (1.0f + tv[MT_DZDISP]);
            if (pdisp[b] < 0.0f) pdisp[b] = 0.0f;
            if (power[b] < 0.0f) power[b] = 0.0f;
        }
        if (tracing) {
            s_tr_n++;
            if (s_tr_n >= s_tr_cap) s_tr_active = false;
        }
        if (s_noise_cal == PHASE1_FRAMES - 1)
            memcpy(s_noise_ref, s_noise_floor, sizeof(s_noise_ref));   /* Phase 1 just ended */
        if (s_noise_cal < PHASE1_FRAMES) s_noise_cal++;

        /* ── Temporal smoothing — TWO separate EMAs from here on ──
         *
         * s_gate_smooth: unchanged symmetric EMA (τ ≈ 3 frames ≈ 45 ms), used
         * ONLY for the gate decision and WiFi-spike rejection below.  Do NOT
         * give this one a fast attack — the WiFi-spike heuristic explicitly
         * relies on "a single loud frame can only move this by 35 %" to tell
         * a genuine sudden-loud-onset apart from an RF spike; speeding up
         * attack here would make real drum hits trip the same rejection.
         *
         * s_disp_smooth: feeds the peak-hold / displayed bars only (below).
         * Asymmetric — fast attack, slow release (see RISE_ATTACK / FALL_RELEASE)
         * — so transients register almost immediately while floor-noise
         * jitter on the way down gets averaged out instead of flickering. */
        static float s_gate_smooth[BAND_COUNT];
        static float s_disp_smooth[BAND_COUNT];
        static float s_prev_disp[BAND_COUNT];   /* s_disp_smooth before this frame, to undo a burst frame */
        if (smooth_reset) {
            memset(s_gate_smooth, 0, sizeof(s_gate_smooth));
            memset(s_disp_smooth, 0, sizeof(s_disp_smooth));
            smooth_reset = false;
        }
        memcpy(s_prev_disp, s_disp_smooth, sizeof(s_prev_disp));
        for (int b = 0; b < BAND_COUNT; b++) {
            s_gate_smooth[b] += 0.35f * (power[b] - s_gate_smooth[b]);
            const float pw = pdisp[b] * s_band_weight[b];   /* spectral tilt: display only */
            float a = (pw > s_disp_smooth[b]) ? bar_attack : bar_release;
            s_disp_smooth[b] += a * (pw - s_disp_smooth[b]);
        }

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
        for (int b = 0; b < BAND_COUNT; b++) total_power += s_gate_smooth[b];

        /* WiFi background-scan RF-coupling spike rejection: the ESP32 WiFi
         * binary scans channels periodically for roaming, and RF switching
         * noise couples into the ADC as a broadband burst (observed
         * 400-3800 total_power). Detection: previous frame moderate-or-lower
         * (<200) AND current frame jumps to >400 — thresholds calibrated
         * from field recordings (max legitimate peak ~190, min spike ~533).
         * Runs on total_power/s_gate_smooth (slow α=0.35 EMA), not the
         * fast-attack s_disp_smooth.
         *
         * A jump like that is also what a loud, close drum hit looks like, and
         * discarding it on sight dropped every such beat. So the jump frame is
         * only HELD (outputs unchanged for 16 ms) and judged on the next frame:
         * an RF burst has vanished by then (frame power < 25 % of the jump
         * frame's), a drum hit is still ringing. A confirmed spike resets both
         * smoothed arrays so it doesn't contaminate future EMA state; the
         * silence gate below then suppresses output. */
        static float s_prev_total  = 0.0f;
        static bool  s_spike_pend  = false;
        static float s_spike_psum  = 0.0f;
        float        frame_power   = 0.0f;
        for (int b = 0; b < BAND_COUNT; b++) frame_power += power[b];
        if (s_spike_pend) {
            s_spike_pend = false;
            if (frame_power < tv[MT_SPIKE_RATIO] * s_spike_psum) {
                for (int b = 0; b < BAND_COUNT; b++) {
                    s_gate_smooth[b] = 0.0f;
                    s_disp_smooth[b] = 0.0f;
                }
                total_power = 0.0f;
            }
        } else if (tv[MT_SPIKE] >= 0.5f && s_prev_total < tv[MT_SPIKE_LO] && total_power > tv[MT_SPIKE_HI]) {
            s_st_holds++;
            s_spike_pend = true;
            s_spike_psum = frame_power;
            s_prev_total = total_power;
            continue;   /* hold the previous output for this one frame */
        }
        s_prev_total = total_power;

        /* Gate decision. Mode 0 (the original): the SUM of smoothed post-floor power must reach the Noise Floor setting.
         * Modes 1-3: each band's smoothed power is first expressed in units of its own noise threshold
         * (floor x (1 + dead zone), never below GTHR) and capped at GCAP, so a quiet treble tone counts as much as
         * the same SNR in the bass; the sum must reach GSNR x (Noise Floor / 30). Mode 2 also needs 2 of the last 3
         * frames to pass. Mode 3 (default) only counts a band once it has stayed above GUMIN units for GRUN frames
         * in a row, which drops the one-to-three-frame noise bursts that a sum over 24 bands is otherwise open to.
         * Once open it stays open for ghk x (frames it has been open), at most ghm frames, after the last pass, so the
         * gaps between beats of real music (21 closures of up to 192 ms in a 23 s trace without it) don't blank the bars
         * while a brief noise-triggered opening only lingers a frame or two.
         * (Chosen by replaying recorded idle, polled-idle and tone traces: it opens for every tone, even quiet 1-5 kHz
         * ones the original gate misses, and opens less at idle.) */
        bool gate_closed = false;
        {
            static uint8_t s_hist_active = 0;      /* last 3 pass/fail results of the active gate (mode 2)  */
            static uint8_t s_hist_shadow[4];       /* same, for each shadow GATE_SNR below (mode 1 shadows)  */
            static uint8_t s_run[BAND_COUNT];      /* frames in a row each band has been above GUMIN units  */
            static bool    s_g3_open = false;      /* mode 3 hold state                                     */
            static int     s_g3_since = 0, s_g3_openrun = 0;
            static const float SHADOW_SNR[4]  = { 4.0f, 6.0f, 10.0f, 16.0f };
            static const float SHADOW3_SNR[4] = { 2.0f, 3.0f, 4.0f, 6.0f };
            float units = 0.0f, units3 = 0.0f;
            for (int b = 0; b < BAND_COUNT; b++) {
                float thr = s_noise_floor[b] * (1.0f + tv[MT_DEADZONE]);
                if (thr < tv[MT_GTHR]) thr = tv[MT_GTHR];
                const float u = s_gate_smooth[b] / thr;
                const float uc = u > tv[MT_GCAP] ? tv[MT_GCAP] : u;
                units += uc;
                if (u >= tv[MT_GUMIN]) { if (s_run[b] < 255) s_run[b]++; } else s_run[b] = 0;
                if (s_run[b] >= (int)tv[MT_GRUN]) units3 += uc;
            }
            const float gscale = silence_gate / 30.0f;
            float gv = total_power, gt = silence_gate;
            if (silence_gate > 0.0f) {
                bool pass;
                if (tv[MT_GMODE] < 0.5f) {
                    pass = total_power >= silence_gate;
                } else if (tv[MT_GMODE] >= 2.5f) {
                    gv = units3;
                    gt = tv[MT_GSNR] * gscale;
                    if (units3 >= gt) {
                        s_g3_open = true; s_g3_since = 0;
                    } else if (s_g3_open) {
                        float hold = (float)s_g3_openrun * tv[MT_GHK];
                        if (hold > tv[MT_GHM]) hold = tv[MT_GHM];
                        if ((float)(++s_g3_since) > hold) s_g3_open = false;
                    }
                    s_g3_openrun = s_g3_open ? (s_g3_openrun < 100000 ? s_g3_openrun + 1 : s_g3_openrun) : 0;
                    pass = s_g3_open;
                } else {
                    gv = units;
                    gt = tv[MT_GSNR] * gscale;
                    pass = units >= gt;
                    s_hist_active = (uint8_t)(((s_hist_active << 1) | (pass ? 1 : 0)) & 7);
                    if (tv[MT_GMODE] >= 1.5f) pass = __builtin_popcount(s_hist_active) >= 2;
                }
                gate_closed = !pass;
                /* Shadow counters: what every candidate gate would have done on this very frame. */
                s_st_eval++;
                if (total_power >= silence_gate) s_st_open[0]++;
                for (int k = 0; k < 4; k++) {
                    const bool pk = units >= SHADOW_SNR[k] * gscale;
                    if (pk) s_st_open[1 + k]++;
                    if (units3 >= SHADOW3_SNR[k] * gscale) s_st_open[5 + k]++;
                    (void)s_hist_shadow[k];
                }
            }
            s_st_gate_val = gv; s_st_gate_thr = gt;
        }
        if (gate_closed && !s_cal_requested) {
            s_st_gated++;
            for (int b = 0; b < BAND_COUNT; b++) peak[b] *= bar_decay;
            if (s_scope) memset(s_scope, 0, MIC_SCOPE_SAMPLES * sizeof(int16_t));
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
                for (int b = 0; b < BAND_COUNT; b++) {
                    s_noise_floor[b] = s_cal_accum[b] / (float)MIC_CAL_FRAMES;
                    s_noise_ref[b]   = s_noise_floor[b];
                }
                s_noise_cal = PHASE1_FRAMES;   /* skip Phase 1 — floor is now precisely set */
                taskEXIT_CRITICAL(&s_mux);
                s_cal_requested = false;
                xSemaphoreGive(s_cal_done);
            }
        }

        /* ── Burst rejection (see BURST_BARS): count the bars this frame would push to about max, using last frame's scale ── */
        static int   s_burst_run = 0;
        static float s_gref_prev = MIC_NOISE_FLOOR;
        if (tv[MT_BURSTN] >= 1.0f && s_burst_run < BURST_MAX_RUN) {
            int sat = 0;
            const float burst_ratio = tv[MT_SQUELCH] + (1.0f - tv[MT_SQUELCH]) * powf(BURST_LEVEL, 1.0f / tv[MT_GAMMA]);
            for (int b = BURST_FIRST_BAND; b < BAND_COUNT; b++) {
                if (s_band_masked[b]) continue;
                float ru = tv[MT_GAIN] * ref[b];
                const float mr = tv[MT_SNR] * s_noise_floor[b] * (1.0f + tv[MT_DZDISP]) * s_band_weight[b];
                if (ru < mr) ru = mr;
                if (ru < MIC_NOISE_FLOOR) ru = MIC_NOISE_FLOOR;
                if (s_disp_smooth[b] >= burst_ratio * powf(ru, tv[MT_EQ]) * powf(s_gref_prev, 1.0f - tv[MT_EQ])) sat++;
            }
            if ((float)sat >= tv[MT_BURSTN]) {
                memcpy(s_disp_smooth, s_prev_disp, sizeof(s_disp_smooth));   /* drop this frame's attack */
                s_burst_run++;
            } else {
                s_burst_run = 0;
            }
        } else {
            s_burst_run = 0;
        }

        /* ── Beat-onset detector (issue #43: Breath/Rainbow beat-reactive nudge
         * when spectrum_led_source == "follow accent mode") ──
         * Feeds off s_disp_smooth (fast attack, ~16-24 ms, RISE_ATTACK above)
         * — deliberately NOT s_gate_smooth/total_power, which is the slow-
         * attack signal the silence gate and WiFi-spike rejection above rely
         * on specifically staying blind to single-frame transients. This runs
         * after the silence-gate `continue` above, so gated-silent frames
         * never fire a false beat.
         *
         * Broadband sum across all 24 bands, not just bass: the lowest
         * analysis band starts at 280 Hz (see the band table), so sub-bass
         * kick-drum fundamentals aren't resolved here — this reacts to
         * general percussive transients (snare/hihat/kick harmonics), not a
         * strictly kick-locked beat. BEAT_MIN_ENERGY and the 1.5x ratio below
         * are starting-point constants, not calibrated against real music —
         * same situation the noise-floor/WiFi-spike thresholds were in
         * before those got tuned from field recordings (see above); expect
         * to retune these by ear once this is on real hardware. */
        #define BEAT_MIN_ENERGY     40.0f          /* absolute floor — ignore quiet ambient noise */
        #define BEAT_REFRACTORY_US  200000LL       /* 200 ms — caps detection at 300 BPM */
        static float   s_beat_baseline = 0.0f;
        static int64_t s_beat_last_us  = 0;
        {
            float beat_energy = 0.0f;
            for (int b = 0; b < BAND_COUNT; b++) beat_energy += s_disp_smooth[b];
            int64_t now_us = esp_timer_get_time();
            if (beat_energy > s_beat_baseline * 1.5f && beat_energy > BEAT_MIN_ENERGY &&
                    (now_us - s_beat_last_us) > BEAT_REFRACTORY_US) {
                taskENTER_CRITICAL(&s_mux);
                s_beat_pulse = true;
                taskEXIT_CRITICAL(&s_mux);
                s_beat_last_us = now_us;
            }
            s_beat_baseline += 0.3f * (beat_energy - s_beat_baseline);
        }

        /* ── Peak-hold, plus each band's running average level ── */
        float ref_use[BAND_COUNT], gref = MIC_NOISE_FLOOR;
        for (int b = 0; b < BAND_COUNT; b++) {
            if (s_disp_smooth[b] > peak[b]) peak[b]  = s_disp_smooth[b];
            else                            peak[b] *= bar_decay;
            ref[b] += (s_disp_smooth[b] > ref[b] ? tv[MT_UP] : tv[MT_DOWN]) * (s_disp_smooth[b] - ref[b]);
            const float min_ref = tv[MT_SNR] * s_noise_floor[b] * (1.0f + tv[MT_DZDISP]) * s_band_weight[b];
            ref_use[b] = tv[MT_GAIN] * ref[b] > min_ref ? tv[MT_GAIN] * ref[b] : min_ref;
            if (ref_use[b] < MIC_NOISE_FLOOR) ref_use[b] = MIC_NOISE_FLOOR;
            if (ref_use[b] > gref) gref = ref_use[b];
        }
        s_gref_prev = gref;

        /* ── Publish normalised 0.0–1.0 values + debug snapshot ──
         * Perceptual gamma applied at publish (see SPEC_GAMMA), with the
         * pre-gamma squelch remap (see SPEC_SQUELCH_RATIO) so near-zero
         * ratios don't get boosted into visible noise.  Debug snapshots
         * below stay in linear power units for diagnostics.  powf computed
         * outside the critical section (ints stay enabled). */
        float disp_out[BAND_COUNT];
        for (int b = 0; b < BAND_COUNT; b++) {
            const float ref_eff = powf(ref_use[b], tv[MT_EQ]) * powf(gref, 1.0f - tv[MT_EQ]);
            float ratio = peak[b] / ref_eff;
            if (ratio > 1.0f) ratio = 1.0f;
            ratio = (ratio - tv[MT_SQUELCH]) / (1.0f - tv[MT_SQUELCH]);
            if (ratio < 0.0f) ratio = 0.0f;
            disp_out[b] = powf(ratio, tv[MT_GAMMA]);
        }
        for (int b = 0; b < BAND_COUNT; b++) {
            if (!s_band_masked[b]) continue;
            int lo = b - 1, hi = b + 1;
            while (lo >= 0 && s_band_masked[lo]) lo--;
            while (hi < BAND_COUNT && s_band_masked[hi]) hi++;
            const float vl = lo >= 0 ? disp_out[lo] : -1.0f, vh = hi < BAND_COUNT ? disp_out[hi] : -1.0f;
            disp_out[b] = (vl >= 0.0f && vh >= 0.0f) ? 0.5f * (vl + vh) : (vl >= 0.0f ? vl : (vh >= 0.0f ? vh : 0.0f));
        }
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

void mic_hw_init(void)
{
    config_lock();
    const nextube_config_t *cfg = config_get();
    bool    mic_en = cfg->mic_enabled;
    uint8_t cfg_ch = cfg->mic_adc_channel;
    config_unlock();

    if (!mic_en) {
        ESP_LOGI(TAG, "mic_hw_init: mic disabled — ADC hardware not allocated");
        return;
    }
    if (cfg_ch > 7) cfg_ch = 7;
    s_active_ch   = cfg_ch;
    s_active_chan = ADC1_CHAN_MAP[cfg_ch];

    /* Initialise ADC1 oneshot unit. Both this and the adc_continuous handle
     * below are created once, here, and live for the device's lifetime —
     * see the s_acq declaration comment for why they're never torn down and
     * recreated per Spectrum-mode session. Failure here (unlike
     * adc_continuous_new_handle() below) returns a normal error rather than
     * hitting an ESP-IDF internal-abort landmine, so it's handled gracefully
     * — log and leave the mic unallocated for this boot rather than crash
     * the whole device over it. */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mic_hw_init: adc_oneshot_new_unit failed (%s) — mic unavailable this boot",
                 esp_err_to_name(err));
        s_adc = NULL;
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(s_adc, s_active_chan, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mic_hw_init: adc_oneshot_config_channel failed (%s) — mic unavailable this boot",
                 esp_err_to_name(err));
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return;
    }

    /* adc_continuous_new_handle() needs ~10 KB of INTERNAL+DMA-capable RAM
     * (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA — a smaller, more contended pool
     * than general heap). If it fails partway through, its own cleanup path
     * unconditionally calls adc_apb_periph_free() even though it never
     * claimed the periph, aborting the device — an ESP-IDF bug that happens
     * inside this call, before it can return an error to us, so no return-
     * value check here catches it. The only real fix is calling this EARLY
     * (from app_main(), before wifi_manager_start()) while the pool is
     * still abundant, not after WiFi/MQTT have fragmented it. Kept as
     * ESP_ERROR_CHECK for consistency with other boot-time driver setup,
     * not because it adds protection beyond a plain call. */
    ESP_LOGI(TAG, "mic_hw_init: pre-adc_continuous DMA-capable internal RAM — free %u B, largest block %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));

    adc_continuous_handle_cfg_t hcfg = {
        .max_store_buf_size = RAW_FRAME_BYTES * 4,
        .conv_frame_size    = RAW_FRAME_BYTES,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&hcfg, &s_acq));

    ESP_LOGI(TAG, "mic_hw_init: ADC1 CH%u (GPIO%d) hardware ready", cfg_ch, ADC1_GPIO_MAP[cfg_ch]);
}

bool mic_init(void)
{
    if (!s_adc || !s_acq) {
        ESP_LOGW(TAG, "mic_init: ADC hardware was not allocated (mic was disabled at boot, or "
                       "mic_hw_init failed) — mic will not start this boot");
        return false;
    }

    config_lock();
    uint8_t cfg_ch = config_get()->mic_adc_channel;
    config_unlock();
    if (cfg_ch > 7) cfg_ch = 7;
    /* mic_hw_init() already set the channel from config at boot; only
     * reconfigure if it changed since then (e.g. user edited it via the web
     * UI during the AP-PIN setup window before this deferred call runs). */
    if (cfg_ch != s_active_ch)
        reconfigure_channel(cfg_ch);

    /* Precompute the Hann window (see s_hann). */
    for (int i = 0; i < FRAME_SIZE; i++)
        s_hann[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i
                                        / (float)(FRAME_SIZE - 1)));

    /* Band layout (centres, tilt, bin map, LED-noise weights) from the configured top frequency. */
    config_lock();
    uint16_t top_hz_cfg = config_get()->mic_top_hz;
    config_unlock();
    mic_build_bands(top_hz_cfg);

    ESP_LOGI(TAG, "mic_init: CH%u (GPIO%d), ADC hardware already allocated by mic_hw_init()",
             cfg_ch, ADC1_GPIO_MAP[cfg_ch]);

    /* Raw-frame export completion (see mic_capture_raw_frame). */
    s_dump_done = xSemaphoreCreateBinary();
    configASSERT(s_dump_done);

    /* Capture itself is started/stopped on demand by mic_task (gated to
     * Spectrum mode / calibration, released during audio playback) —
     * nothing to start here. */
    ESP_LOGI(TAG, "Capture: adc_continuous %d Hz ÷%d → %d Hz effective — gated to Spectrum mode",
             ADC_HW_RATE, DECIM, SAMPLE_RATE);
    return true;
}

int mic_read_raw(void)
{
    /* When capture is gated (not in Spectrum mode) the cached value would be
     * stale, so take a fresh one-shot read.  While the continuous capture is
     * running we return the cached value mic_task keeps updating — a oneshot
     * read would contend with the digital controller for the SAR anyway. */
    if (!s_acq_running && s_adc) {
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

void mic_hold_capture(bool hold)
{
    s_hold_capture = hold;
}

void mic_get_scope(int16_t out[MIC_SCOPE_SAMPLES])
{
    if (s_scope) memcpy(out, s_scope, MIC_SCOPE_SAMPLES * sizeof(int16_t));
    else         memset(out, 0, MIC_SCOPE_SAMPLES * sizeof(int16_t));
}

/* Consume-and-clear: returns true at most once per detected beat onset (see
 * the detector in mic_task above). Callers poll this once per their own
 * tick — a caller ticking slower than BEAT_REFRACTORY_US won't miss a beat
 * (the flag is level-held between mic frames, not a narrow edge window),
 * but calling this more than once per intended "check" will make the second
 * call see it already cleared. */
bool mic_get_beat_pulse(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool p = s_beat_pulse;
    s_beat_pulse = false;
    taskEXIT_CRITICAL(&s_mux);
    return p;
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
        /* Same critical section pattern as the rest of this file's s_cal_*
         * state: without it, a second mic_calibrate() call starting right
         * after this timeout could reset s_cal_accum/s_cal_frame_cnt while
         * mic_task is still mid-write to s_cal_accum[] for this abandoned
         * session (see the accumulation write in mic_task). */
        taskENTER_CRITICAL(&s_mux);
        s_cal_requested = false;
        taskEXIT_CRITICAL(&s_mux);
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

void mic_set_test_capture(bool on, uint32_t duration_s)
{
    if (on) {
        if (duration_s == 0)    duration_s = 1200;
        if (duration_s > 3600)  duration_s = 3600;
        s_test_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + duration_s * 1000u;
    }
    s_test_capture = on;
    ESP_LOGI(TAG, "test capture %s%s", on ? "ON" : "OFF", on ? " (display idles blank)" : "");
}

bool mic_test_capture_active(void)
{
    return s_test_capture;
}

void mic_apply_calibration(const float floor[MIC_BAND_COUNT])
{
    taskENTER_CRITICAL(&s_mux);
    memcpy(s_noise_floor, floor, BAND_COUNT * sizeof(float));
    memcpy(s_noise_ref,   floor, BAND_COUNT * sizeof(float));
    s_noise_cal = PHASE1_FRAMES;   /* skip Phase 1 — saved floor is already accurate */
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "mic_apply_calibration: saved baseline applied, Phase 1 skipped");
}
