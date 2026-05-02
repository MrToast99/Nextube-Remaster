/**
 * @file microphone.h
 * @brief CMEJ-0413-42-SMT-TR electret condenser microphone – public API
 *
 * The ADC1 channel used for sampling is runtime-configurable via
 * cfg->mic_adc_channel (0-7), allowing GPIO selection without a rebuild.
 * ADC1 channel → GPIO map: 0=36, 1=37, 2=38, 3=39, 4=32, 5=33, 6=34, 7=35.
 * Provides Goertzel-based 24-band audio energy values used by Spectrum mode.
 * Bands are log-spaced 280–3800 Hz, grouped 4 per LCD tube (tube 0 = bands 0-3, etc.).
 */
#pragma once

#include <stdint.h>

/**
 * Initialise the ADC1 unit and configure the channel from cfg->mic_adc_channel.
 * Must be called before mic_task_start().
 */
void mic_init(void);

/**
 * Create and pin the microphone sampling task on core 1.
 * Samples at 8 kHz, computes Goertzel energy for MIC_BAND_COUNT logarithmic
 * bands, applies peak-hold with exponential decay, and publishes normalised
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
