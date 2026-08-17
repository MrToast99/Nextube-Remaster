/**
 * @file microphone.h
 * @brief Electret condenser microphone (unmarked 4 mm SMT capsule) – public API
 *
 * The ADC1 channel used for sampling is runtime-configurable via
 * cfg->mic_adc_channel (0-7), allowing GPIO selection without a rebuild.
 * ADC1 channel → GPIO map: 0=36, 1=37, 2=38, 3=39, 4=32, 5=33, 6=34, 7=35.
 * Provides Goertzel-based 24-band audio energy values used by Spectrum mode.
 * Bands are log-spaced 280–3800 Hz, grouped 4 per LCD tube (tube 0 = bands 0-3, etc.).
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
 * Capture is hardware-clocked (adc_continuous / I²S0 DMA, 32 kHz ÷4 → 8 kHz);
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
 * Bands are log-spaced 280–3800 Hz, grouped 4 per tube (tube 0 = bands 0-3, etc.).
 */
void mic_get_bands(float out[MIC_BAND_COUNT]);

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

/** Number of frames averaged during a manual baseline capture (~320 ms at 8 kHz/128-sample frames). */
#define MIC_CAL_FRAMES 20

/**
 * Audio playback ↔ microphone I2S0 arbitration (ESP32: dac_continuous and
 * adc_continuous both ride the I2S0 peripheral and cannot coexist).
 * The audio component calls (true) before bringing the DAC up — this blocks
 * spectrum capture and waits (bounded, ≤400 ms) for the mic to release the
 * peripheral — and (false) after DAC teardown so capture resumes.
 * Safe to call when the mic is disabled / never initialised.
 */
void mic_set_audio_active(bool active);

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
 *  before the ×4 decimation to the 8 kHz analysis rate). */
#define MIC_RAW_FRAME_SAMPLES 512

/** Decimated (analysis-rate) samples per frame. */
#define MIC_FRAME_SAMPLES 128

/**
 * Atomic debug capture of ONE DMA frame in both views: raw 32 kHz samples
 * and the decimated/DC-removed 8 kHz values actually fed to the Goertzel.
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
