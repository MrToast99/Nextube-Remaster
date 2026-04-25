/**
 * @file microphone.h
 * @brief CMEJ-0413-42-SMT-TR electret condenser microphone – public API
 *
 * Connected on GPIO36 / ADC1_CH0 (SENSOR_VP).
 * Provides Goertzel-based 6-band audio energy values used by Spectrum mode.
 */
#pragma once

/**
 * Initialise the ADC1 unit and configure channel 0 for mic input.
 * Must be called before mic_task_start().
 */
void mic_init(void);

/**
 * Create and pin the microphone sampling task on core 1.
 * Samples at 8 kHz, computes Goertzel energy for 6 logarithmic bands,
 * applies peak-hold with exponential decay, and publishes normalised
 * 0.0–1.0 band values readable via mic_get_bands().
 */
void mic_task_start(void);

/**
 * Copy the latest normalised band energies into out[6] (thread-safe).
 * Values are in the range 0.0 (silence) to 1.0 (loudest band this frame).
 * Bands correspond to: 125, 250, 500, 1000, 2000, 4000 Hz.
 */
void mic_get_bands(float out[6]);
