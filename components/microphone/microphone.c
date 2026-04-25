/**
 * @file microphone.c
 * @brief CMEJ-0413-42-SMT-TR electret condenser microphone – ADC + Goertzel analyser
 *
 * Hardware: GPIO36 (ADC1_CH0 / SENSOR_VP) with external 2.2 kΩ bias resistor to 3.3 V.
 * The mic quiescent output is ~1.5–2.2 V; with ADC_ATTEN_DB_11 the full 0–3.3 V range
 * is captured, giving a 12-bit idle reading of roughly 1800–2750 counts.
 *
 * Approach:
 *   • adc_oneshot API – uses the SAR ADC directly, no I2S dependency
 *     (adc_continuous / adc_dma would conflict with dac_continuous on I2S0)
 *   • Dedicated FreeRTOS task on core 1, esp_timer_get_time() busy-wait for
 *     precise 125 µs sample spacing (8 kHz)
 *   • DC removal: exponential moving average subtracted each sample
 *   • Goertzel algorithm: energy at 6 logarithmic centre frequencies
 *   • Peak-hold with exponential decay (attack = instant, release per frame)
 *   • Normalised float[6] output published under a portMUX_TYPE spinlock
 */

#include "microphone.h"
#include "board_pins.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "mic";

/* ── Sampling parameters ─────────────────────────────────────────────── */
#define SAMPLE_RATE     8000            /* Hz */
#define FRAME_SIZE      128             /* samples per Goertzel frame → 16 ms */
#define BAND_COUNT      6
#define SAMPLE_US       (1000000 / SAMPLE_RATE)   /* 125 µs per sample */
#define DECAY           0.85f           /* peak-hold per-frame decay multiplier */
#define DC_ALPHA        0.999f          /* DC removal IIR coefficient */

/* Centre frequencies for the 6 bands (logarithmically spaced, 125 Hz – 4 kHz) */
static const float BAND_FREQS[BAND_COUNT] = {125.0f, 250.0f, 500.0f,
                                              1000.0f, 2000.0f, 4000.0f};

/* ── Shared output state ─────────────────────────────────────────────── */
static float        s_bands[BAND_COUNT];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ── ADC handle ──────────────────────────────────────────────────────── */
static adc_oneshot_unit_handle_t s_adc;

/* ── Goertzel single-bin DFT energy ─────────────────────────────────── */
/**
 * Compute the Goertzel energy for a single frequency `freq` over `N` samples.
 * Returns the squared magnitude (not normalised — caller divides by norm factor).
 */
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

/* ── Microphone sampling task ────────────────────────────────────────── */
static void mic_task(void *arg)
{
    float   samples[FRAME_SIZE];
    float   dc   = 2048.0f;         /* initial DC estimate (mid-scale 12-bit) */
    float   peak[BAND_COUNT];
    memset(peak, 0, sizeof(peak));

    /* Goertzel normalisation factor: N² / 4 converts raw Goertzel energy to a
     * dimensionless power estimate comparable across different frame sizes. */
    const float norm = (float)(FRAME_SIZE * FRAME_SIZE) / 4.0f;

    ESP_LOGI(TAG, "Mic task started – sampling at %d Hz, %d bands", SAMPLE_RATE, BAND_COUNT);

    while (1) {
        /* ── Collect FRAME_SIZE samples at exactly SAMPLE_RATE ── */
        int64_t t = esp_timer_get_time();
        for (int i = 0; i < FRAME_SIZE; i++) {
            int raw = 2048;
            adc_oneshot_read(s_adc, PIN_MIC_ADC_CHAN, &raw);
            /* Exponential moving average DC removal */
            dc        = DC_ALPHA * dc + (1.0f - DC_ALPHA) * (float)raw;
            samples[i] = (float)raw - dc;
            /* Advance deadline and busy-wait */
            t += SAMPLE_US;
            while (esp_timer_get_time() < t) { /* spin – < 125 µs */ }
        }

        /* ── Goertzel energy + peak-hold per band ── */
        float max_power = 1.0f;   /* avoid divide-by-zero; floor at 1 */
        float power[BAND_COUNT];
        for (int b = 0; b < BAND_COUNT; b++) {
            power[b] = goertzel(samples, FRAME_SIZE, BAND_FREQS[b]) / norm;
            if (power[b] > peak[b])
                peak[b]  = power[b];              /* instant attack */
            else
                peak[b] *= DECAY;                 /* slow decay */
            if (peak[b] > max_power)
                max_power = peak[b];
        }

        /* ── Publish normalised 0.0–1.0 values ── */
        taskENTER_CRITICAL(&s_mux);
        for (int b = 0; b < BAND_COUNT; b++)
            s_bands[b] = peak[b] / max_power;
        taskEXIT_CRITICAL(&s_mux);
        /* No vTaskDelay — the busy-wait loop above sets the frame rate */
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void mic_init(void)
{
    ESP_LOGI(TAG, "Initialising ADC1/CH0 for electret mic on GPIO%d", PIN_MIC_ADC);

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_11,   /* 0–3.3 V full-scale */
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, PIN_MIC_ADC_CHAN, &chan_cfg));
}

void mic_task_start(void)
{
    /* Stack 4096: samples[128] = 512 B, work vars, task overhead < 2 KB */
    xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "Mic sampling task started on core 1");
}

void mic_get_bands(float out[6])
{
    taskENTER_CRITICAL(&s_mux);
    memcpy(out, s_bands, BAND_COUNT * sizeof(float));
    taskEXIT_CRITICAL(&s_mux);
}
