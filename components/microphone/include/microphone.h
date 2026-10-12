/**
 * @file microphone.h
 * @brief Electret condenser microphone (unmarked 4 mm SMT capsule) – public API
 *
 * The ADC1 channel used for sampling is runtime-configurable via
 * cfg->mic_adc_channel (0-7), allowing GPIO selection without a rebuild.
 * ADC1 channel → GPIO map: 0=36, 1=37, 2=38, 3=39, 4=32, 5=33, 6=34, 7=35.
 * Provides Goertzel-based 24-band audio energy values used by Spectrum mode.
 * Bands are log-spaced from 280 Hz up to a configurable top (1200–8000 Hz, default 2000), grouped 4 per LCD tube (tube 0 = bands 0-3, etc.).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * Allocate the ADC hardware (oneshot unit + adc_continuous handle) — the
 * memory-hungry part of setup (~10 KB of MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA
 * for the continuous handle alone). Must be called EARLY in boot, before
 * WiFi/MQTT/audio get a chance to claim that same small, contended memory
 * pool — calling it late (after WiFi connects) can leave too little free for
 * adc_continuous_new_handle() to succeed, and ESP-IDF's own cleanup path
 * aborts the device on that failure with no way for application code to
 * catch it. No-op (leaves the ADC unallocated) if the mic is disabled in
 * config at the moment this runs. Call once, before mic_init().
 */
void mic_hw_init(void);

/**
 * Finish mic setup: precompute analysis tables, create semaphores, and pick
 * up the channel from cfg->mic_adc_channel (reconfiguring if it changed
 * since mic_hw_init()). Must be called after mic_hw_init() and before
 * mic_task_start(). Returns false (and skips all of the above) if the ADC
 * hardware was never allocated — mic was disabled at boot when mic_hw_init()
 * ran, or that allocation failed — in which case the caller should NOT call
 * mic_task_start() either.
 */
bool mic_init(void);

/**
 * Create and pin the microphone analysis task on core 0.
 * Capture is hardware-clocked (adc_continuous / I²S0 DMA, 32 kHz ÷2 → 16 kHz);
 * the task computes Goertzel energy for MIC_BAND_COUNT logarithmic bands,
 * applies peak-hold with exponential decay, and publishes normalised
 * 0.0–1.0 band values readable via mic_get_bands().
 */
void mic_task_start(void);

/** Number of frequency bands published by mic_get_bands().
 *  4 bands per LCD tube × 6 tubes = 24 bands total. */
#define MIC_BAND_COUNT 24

/**
 * Copy the latest normalised band energies into out[MIC_BAND_COUNT] (thread-safe).
 * Values are in the range 0.0 (silence) to 1.0 (loudest band this frame).
 * Band layout: see this file's header comment.
 */
void mic_get_bands(float out[MIC_BAND_COUNT]);

/** Latest MIC_SCOPE_SAMPLES DC-removed samples at 16 kHz (oldest first) for the oscilloscope view, all zero while
 *  the silence gate is closed. */
#define MIC_SCOPE_SAMPLES 768
void mic_get_scope(int16_t out[MIC_SCOPE_SAMPLES]);

/** While held, capture stays off even in Spectrum mode (the display holds it during the boot splash so the analysis
 *  doesn't compete with Wi-Fi / network start-up and the noise floor isn't learned from boot noise). */
void mic_hold_capture(bool hold);

/*
 * Consume-and-clear beat-onset flag, true at most once per detected beat.
 * Broadband transient detector (not true BPM/tempo tracking) — reacts to
 * general percussive hits (snare/hihat/kick harmonics; sub-bass kick
 * fundamentals below 280 Hz aren't resolved by the analysis bands). Only
 * produces pulses while the mic is actively capturing (Spectrum mode on
 * screen — see mic_task's capture gate). Call at most once per tick from
 * a single caller; a second call in the same tick sees it already cleared.
 */
bool mic_get_beat_pulse(void);

/**
 * Read one raw 12-bit ADC sample (0-4095) from the currently configured
 * channel. Intended for the hardware debug panel — call when NOT in active
 * Spectrum sampling (i.e. when mic_task is gated/sleeping) to avoid
 * contention. Returns -1 if the ADC unit is not initialised.
 */
int mic_read_raw(void);

/**
 * Return the GPIO number for the currently active ADC1 channel.
 * Useful for displaying the pin label in the debug UI.
 */
int mic_gpio_num(void);

/** Number of frames averaged during a manual baseline capture (~320 ms: 20 frames of 16 ms). */
#define MIC_CAL_FRAMES 20

/**
 * Capture a noise baseline: averages MIC_CAL_FRAMES raw Goertzel frames and
 * writes the result into the active noise floor.  Blocks the caller for up to
 * timeout_ms milliseconds.  Returns true on success, false on timeout (mic
 * task not running or not processing frames).
 * out[MIC_BAND_COUNT] receives the captured floor values (may be NULL).
 * Works from any mode — bypasses the Spectrum-mode gate automatically.
 */
bool mic_calibrate(float out[MIC_BAND_COUNT], uint32_t timeout_ms);

/** Raw samples per exported debug frame (one 16 ms DMA frame at 32 kHz,
 *  before the ×2 decimation to the 16 kHz analysis rate). */
#define MIC_RAW_FRAME_SAMPLES 512

/** Decimated (analysis-rate) samples per frame. */
#define MIC_FRAME_SAMPLES 256

/**
 * Atomic debug capture of ONE DMA frame in both views: raw 32 kHz samples
 * and the decimated/DC-removed 16 kHz values actually fed to the Goertzel.
 * Either pointer may be NULL.  Requires capture running; see
 * mic_capture_raw_frame for timeout semantics.
 */
bool mic_capture_frame_pair(uint16_t raw[MIC_RAW_FRAME_SAMPLES],
                            float dec[MIC_FRAME_SAMPLES], uint32_t timeout_ms);

/**
 * Per-band pipeline snapshot for /api/debug/micbands (diagnostics).
 * raw   = band energy before noise-floor subtraction
 * floor = current adaptive/saved noise floor
 * power = post-floor, post-tilt power feeding peak-hold
 * bands = final normalised 0..1 display values
 * Any pointer may be NULL to skip that array.  Thread-safe.
 */
void mic_get_band_debug(float raw[MIC_BAND_COUNT], float floor[MIC_BAND_COUNT],
                        float power[MIC_BAND_COUNT], float bands[MIC_BAND_COUNT]);

/**
 * Copy the next raw 32 kHz capture frame (pre-decimation) into out[].
 * Diagnostic for /api/debug/micframe — requires capture to be running
 * (Spectrum mode on screen).  Blocks up to timeout_ms; returns false on
 * timeout (capture gated, audio holding I2S0, or mic disabled).
 */
bool mic_capture_raw_frame(uint16_t out[MIC_RAW_FRAME_SAMPLES], uint32_t timeout_ms);

/**
 * Reset the noise floor to zero and restart the Phase 1 auto-calibration
 * (~4 s ramp-up).  Thread-safe; call from any task.
 */
void mic_reset_calibration(void);

/**
 * Apply a previously-saved noise floor directly, skipping Phase 1 entirely.
 * Call after mic_init() / mic_task_start() to restore a persisted baseline.
 * Thread-safe.
 */
void mic_apply_calibration(const float floor[MIC_BAND_COUNT]);

/**
 * Diagnostic test capture: keeps the capture + analysis pipeline running in ANY mode
 * (normally it only runs while Spectrum is on screen), so mic noise can be measured
 * without the 20 Hz Spectrum drawing. While on, the display task idles with the tubes
 * blank (see display_task), which keeps the web server responsive. Not persisted;
 * auto-stops after duration_s (0 = 20 minutes, capped at 1 hour).
 */
void mic_set_test_capture(bool on, uint32_t duration_s);

/** Spectrum display / noise constants that can be changed live from the hidden diagnostics panel
 *  (runtime only, reset on reboot; the compile-time values in microphone.c are the defaults).
 *  Table-driven: mic_tune_desc() lists key / default / range for each MT_* index. */
enum {
    MT_EQ, MT_GAIN, MT_UP, MT_DOWN, MT_SNR, MT_DEADZONE,      /* per-bar scaling + noise dead zone  */
    MT_SQUELCH,                                               /* bar feel (bar speeds are a user setting) */
    MT_GMODE, MT_GSNR, MT_GCAP, MT_GTHR, MT_GUMIN, MT_GRUN,   /* silence gate: mode + SNR-mode parameters */
    MT_GHK, MT_GHM,                                           /* mode 3 hold: k x frames open so far, capped */
    MT_TILT,                                                  /* spectral tilt                      */
    MT_NALPHA,                                                /* noise-floor adaptation rate        */
    MT_SPIKE, MT_SPIKE_LO, MT_SPIKE_HI, MT_SPIKE_RATIO,       /* Wi-Fi spike guard                  */
    MT_MASK, MT_RIP_LO, MT_RIP_HI,                            /* LED ripple masking                 */
    MT_DZDISP,                                                /* dead zone for the bars only        */
    MT_GAMMA,                                                 /* bar height curve: lower = quiet bands taller */
    MT_BURSTN,                                                /* bars at max that make a frame a burst (0 = off) */
    MT_COUNT
};
typedef struct { const char *key; float def, lo, hi; } mic_tune_desc_t;
const mic_tune_desc_t *mic_tune_desc(void);          /* MT_COUNT entries */
void mic_tune_get(float out[MT_COUNT]);
void mic_tune_set(const float in[MT_COUNT]);         /* clamps each value to its range */
void mic_tune_reset(void);

/** Capture health counters (cumulative since boot) for the diagnostics panel. */
typedef struct {
    uint32_t frames;       /* 16 ms frames analysed                                   */
    uint32_t read_fail;    /* DMA reads that timed out or came back short             */
    uint32_t gated;        /* frames held back by the silence gate                    */
    uint32_t spike_holds;  /* frames held by the Wi-Fi spike guard                    */
    int      adc_last;     /* last decimated ADC sample (0-4095)                      */
    float    gate_val;     /* the value the silence gate compared with gate_thr (mode-dependent) */
    float    gate_thr;
    uint32_t gate_eval;    /* frames that reached the silence gate (Noise Floor > 0) */
    uint32_t gate_open[9]; /* frames each candidate gate WOULD have opened on, whatever mode is active:
                              [0] mode 0 | [1..4] mode 1 at GATE_SNR 4/6/10/16 | [5..8] mode 3 at GATE_SNR 2/3/4/6 */
} mic_stats_t;
void mic_get_stats(mic_stats_t *st);

/** Per-frame band-energy trace for offline analysis (diagnostics). mic_trace_start() allocates a PSRAM buffer and
 *  records the RAW energy of all bands for the next `frames` mic frames (16 ms each, max 1500); read it back with
 *  mic_trace_read() once mic_trace_info() reports not-active, then mic_trace_free(). Values are sqrt-companded:
 *  raw = (v / 100)^2. */
bool mic_trace_start(int frames);
void mic_trace_info(int *n, int *cap, bool *active);
bool mic_trace_read(int frame, uint16_t out[MIC_BAND_COUNT]);
void mic_trace_floor(float out[MIC_BAND_COUNT]);   /* noise floor when the trace began */
void mic_trace_free(void);

/** Centre frequency (Hz) of each of the 24 bands in the layout currently in use. */
void mic_get_band_freqs(float out[MIC_BAND_COUNT]);

/** Bands that sit inside an accent-LED ripple frequency range: not analysed, shown as the average of
 *  their neighbours (see LED_RIPPLE_HZ in microphone.c). */
void mic_get_band_masked(bool out[MIC_BAND_COUNT]);
bool mic_test_capture_active(void);
