/**
 * @file microphone.c
 * @brief CMC-4015-25T electret + LMV321IDBVR preamp – ADC continuous (DMA) + Goertzel
 *
 * Hardware: CMC-4015-25T electret capsule → LMV321IDBVR op-amp preamp → GPIO35 (ADC1_CH7).
 * Both components confirmed via hardware inspection and runtime debug panel.
 *
 * PREVIOUS APPROACH (adc_oneshot + busy-wait) — REMOVED:
 *   The 125 µs busy-wait loop between samples held core 1 at 100% CPU, starving the
 *   IDLE1 task and triggering the task watchdog (confirmed in hardware testing).
 *   Additionally, adc_oneshot reads during heavy SPI bus activity (6× ST7735 displays)
 *   produced corrupted results because the SPI switching noise corrupted the ADC
 *   supply rail mid-conversion — the raw value appeared stable at 40–50% and did not
 *   track audio at all.
 *
 * CURRENT APPROACH (adc_continuous / DMA):
 *   • ESP-IDF 5.x adc_continuous uses GDMA — does NOT conflict with dac_continuous
 *     (which uses I2S DMA).  The old "I2S conflict" no longer applies.
 *   • Hardware clocks samples at exactly SAMPLE_RATE Hz with no CPU involvement.
 *   • mic_task blocks on adc_continuous_read() (yields to scheduler — no spin).
 *   • The IDLE1 watchdog is no longer starved.
 *   • mic_read_raw() returns s_last_raw captured by the DMA stream — safe at all times.
 *
 * Sensitivity tuning (compile-time):
 *   MIC_GAIN        – software multiplier after the op-amp (1.0 = let hardware do it).
 *   MIC_NOISE_FLOOR – minimum Goertzel normalisation divisor for active frames.
 * Silence gate (runtime, via web debug panel):
 *   cfg->mic_silence_gate – frame RMS² threshold; 0 = disabled.  Default 250.
 */

#include "microphone.h"
#include "config_mgr.h"
#include "board_pins.h"
#include "esp_adc/adc_continuous.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "mic";

/* ── Sampling parameters ─────────────────────────────────────────────── */
#define SAMPLE_RATE     8000
#define FRAME_SIZE      128             /* samples per Goertzel frame → 16 ms */
#define BAND_COUNT      6
#define DECAY           0.85f           /* peak-hold per-frame multiplier */
#define DC_ALPHA        0.999f          /* DC removal IIR (HPF ~1.3 Hz) */

/* ── Sensitivity (compile-time) ──────────────────────────────────────── */
/* LMV321 provides hardware gain; software gain stays at 1.0.
 * Raise MIC_NOISE_FLOOR if bars react to background noise in a quiet room. */
#define MIC_GAIN        1.0f
#define MIC_NOISE_FLOOR 1.0f

/* ── DMA buffer sizing ───────────────────────────────────────────────── */
/* SOC_ADC_DIGI_RESULT_BYTES = 4 on ESP32 (TYPE1 format: 12-bit data + channel).
 * conv_frame_size must be a multiple of this and sets how many bytes the DMA
 * transfers per interrupt.  We choose exactly one Goertzel frame per interrupt. */
#define ADC_BYTES       ((int)sizeof(adc_digi_output_data_t))   /* 4 on ESP32 */
#define FRAME_BYTES     (FRAME_SIZE * ADC_BYTES)                /* 512 bytes  */
#define RING_BYTES      (FRAME_BYTES * 8)   /* 8-frame ring — covers gate drain delay */

/* Centre frequencies for the 6 bands (logarithmically spaced, 125 Hz – 4 kHz) */
static const float BAND_FREQS[BAND_COUNT] = {125.0f, 250.0f, 500.0f,
                                              1000.0f, 2000.0f, 4000.0f};

/* ── Shared output state ─────────────────────────────────────────────── */
static float        s_bands[BAND_COUNT];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* Last raw ADC count captured by the DMA stream — exposed to debug panel */
static volatile int s_last_raw = -1;   /* -1 = not yet sampled */

/* ── ADC1 channel index → GPIO number ───────────────────────────────── */
static const int ADC1_GPIO_MAP[8] = { 36, 37, 38, 39, 32, 33, 34, 35 };

/* Map config channel index (0-7) → adc_channel_t */
static const adc_channel_t ADC1_CHAN_MAP[8] = {
    ADC_CHANNEL_0, ADC_CHANNEL_1, ADC_CHANNEL_2, ADC_CHANNEL_3,
    ADC_CHANNEL_4, ADC_CHANNEL_5, ADC_CHANNEL_6, ADC_CHANNEL_7,
};

/* ── ADC continuous handle ───────────────────────────────────────────── */
static adc_continuous_handle_t s_adc  = NULL;
static uint8_t s_active_ch            = 0xFF;  /* sentinel: "not yet configured" */

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

/* ── Start/restart the continuous ADC for a given config channel ─────── */
static esp_err_t adc_cont_start(uint8_t cfg_ch)
{
    if (cfg_ch > 7) cfg_ch = 7;

    /* Release any previous handle */
    if (s_adc) {
        adc_continuous_stop(s_adc);
        adc_continuous_deinit(s_adc);
        s_adc = NULL;
    }

    adc_continuous_handle_cfg_t hcfg = {
        .max_store_buf_size = RING_BYTES,
        .conv_frame_size    = FRAME_BYTES,  /* one Goertzel frame per DMA xfer */
    };
    esp_err_t err = adc_continuous_new_handle(&hcfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_new_handle: %s", esp_err_to_name(err));
        return err;
    }

    adc_digi_pattern_config_t pat = {
        .atten    = ADC_ATTEN_DB_12,        /* 0–3.3 V full-scale */
        .channel  = ADC1_CHAN_MAP[cfg_ch],
        .unit     = ADC_UNIT_1,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    adc_continuous_config_t ccfg = {
        .pattern_num    = 1,
        .adc_pattern    = &pat,
        .sample_freq_hz = SAMPLE_RATE,
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
    };
    err = adc_continuous_config(s_adc, &ccfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_config: %s", esp_err_to_name(err));
        adc_continuous_deinit(s_adc);
        s_adc = NULL;
        return err;
    }

    err = adc_continuous_start(s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_start: %s", esp_err_to_name(err));
        adc_continuous_deinit(s_adc);
        s_adc = NULL;
        return err;
    }

    s_active_ch = cfg_ch;
    ESP_LOGI(TAG, "ADC continuous: CH%u (GPIO%d) @ %u Hz  frame=%u samples  ring=%u B",
             cfg_ch, ADC1_GPIO_MAP[cfg_ch], SAMPLE_RATE, FRAME_SIZE, RING_BYTES);
    return ESP_OK;
}

/* ── Mic sampling task ───────────────────────────────────────────────── */
static void mic_task(void *arg)
{
    uint8_t   dma_buf[FRAME_BYTES];
    float     samples[FRAME_SIZE];
    float     dc   = 2048.0f;
    float     peak[BAND_COUNT];
    memset(peak, 0, sizeof(peak));

    const float norm = (float)(FRAME_SIZE * FRAME_SIZE) / 4.0f;

    ESP_LOGI(TAG, "mic_task running  (adc_continuous / DMA  no busy-wait)");

    while (1) {
        const nextube_config_t *cfg = config_get();

        /* ── Reconfigure if the debug panel changed the ADC channel ── */
        uint8_t want_ch = cfg->mic_adc_channel;
        if (want_ch > 7) want_ch = 7;
        if (want_ch != s_active_ch || s_adc == NULL) {
            ESP_LOGI(TAG, "ADC channel change: CH%u → CH%u", s_active_ch, want_ch);
            if (adc_cont_start(want_ch) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
        }

        /* ── Gate: idle when mic disabled or not in Spectrum mode ── */
        bool spectrum_en = (cfg->enabled_modes & (1u << APP_MODE_SPECTRUM)) != 0;
        if (!cfg->mic_enabled || !spectrum_en ||
            cfg->current_mode != APP_MODE_SPECTRUM) {
            /* Drain the DMA ring buffer so it never overflows while idling.
             * This also keeps s_last_raw fresh for the debug panel. */
            uint32_t out = 0;
            if (adc_continuous_read(s_adc, dma_buf, FRAME_BYTES, &out,
                                    pdMS_TO_TICKS(50)) == ESP_OK && out >= (uint32_t)ADC_BYTES) {
                adc_digi_output_data_t *d = (adc_digi_output_data_t *)dma_buf;
                s_last_raw = (int)d->type1.data;
            }
            taskENTER_CRITICAL(&s_mux);
            memset(s_bands, 0, sizeof(s_bands));
            taskEXIT_CRITICAL(&s_mux);
            /* Short yield — keeps IDLE1 fed and the ring buffer from overflowing. */
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* ── Blocking read: wait up to 50 ms for one full Goertzel frame ── */
        uint32_t out_len = 0;
        esp_err_t ret = adc_continuous_read(s_adc, dma_buf, FRAME_BYTES,
                                            &out_len, pdMS_TO_TICKS(50));
        if (ret != ESP_OK || out_len < (uint32_t)FRAME_BYTES) {
            /* Ring buffer underrun — skip frame, yield to avoid tight loop */
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        /* ── Unpack DMA results → float samples with DC removal ── */
        for (int i = 0; i < FRAME_SIZE; i++) {
            adc_digi_output_data_t *d =
                (adc_digi_output_data_t *)&dma_buf[i * ADC_BYTES];
            float raw = (float)d->type1.data;
            dc          = DC_ALPHA * dc + (1.0f - DC_ALPHA) * raw;
            samples[i]  = raw - dc;
        }
        /* Expose the last sample of the frame for the debug panel */
        s_last_raw = (int)((adc_digi_output_data_t *)
                           &dma_buf[(FRAME_SIZE - 1) * ADC_BYTES])->type1.data;

        /* ── Silence gate (threshold from config — tuneable at runtime) ── */
        float rms_sq = 0.0f;
        for (int i = 0; i < FRAME_SIZE; i++)
            rms_sq += samples[i] * samples[i];
        rms_sq /= (float)FRAME_SIZE;

        const float gate = config_get()->mic_silence_gate;
        if (gate > 0.0f && rms_sq < gate) {
            for (int b = 0; b < BAND_COUNT; b++) peak[b] *= DECAY;
            taskENTER_CRITICAL(&s_mux);
            memset(s_bands, 0, sizeof(s_bands));
            taskEXIT_CRITICAL(&s_mux);
            continue;
        }

        /* ── Goertzel energy + peak-hold per band ── */
        float max_power = MIC_NOISE_FLOOR;
        float power[BAND_COUNT];
        for (int b = 0; b < BAND_COUNT; b++) {
            power[b] = goertzel(samples, FRAME_SIZE, BAND_FREQS[b]) / norm * MIC_GAIN;
            if (power[b] > peak[b])  peak[b]  = power[b];
            else                     peak[b] *= DECAY;
            if (peak[b] > max_power) max_power = peak[b];
        }

        /* ── Publish normalised 0.0–1.0 values ── */
        taskENTER_CRITICAL(&s_mux);
        for (int b = 0; b < BAND_COUNT; b++)
            s_bands[b] = peak[b] / max_power;
        taskEXIT_CRITICAL(&s_mux);
        /* No delay needed — adc_continuous_read() already slept while waiting for DMA */
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void mic_init(void)
{
    uint8_t cfg_ch = config_get()->mic_adc_channel;
    if (cfg_ch > 7) cfg_ch = 7;

    ESP_LOGI(TAG, "mic_init: CH%u (GPIO%d)", cfg_ch, ADC1_GPIO_MAP[cfg_ch]);

    if (adc_cont_start(cfg_ch) != ESP_OK) {
        ESP_LOGE(TAG, "mic_init: ADC continuous failed to start");
    }
}

int mic_read_raw(void)
{
    /* Returns the last sample captured by the DMA stream.
     * Valid as soon as mic_task has processed at least one frame.
     * -1 = mic not yet initialised or no frame received. */
    return s_last_raw;
}

int mic_gpio_num(void)
{
    uint8_t cfg_ch = config_get()->mic_adc_channel;
    if (cfg_ch > 7) cfg_ch = 7;
    return ADC1_GPIO_MAP[cfg_ch];
}

void mic_task_start(void)
{
    /* Stack 6144: dma_buf[512 B] + samples[512 B] + Goertzel/peak work + overhead */
    xTaskCreatePinnedToCore(mic_task, "mic", 6144, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "mic_task started (core 1, DMA-driven, no busy-wait)");
}

void mic_get_bands(float out[6])
{
    taskENTER_CRITICAL(&s_mux);
    memcpy(out, s_bands, BAND_COUNT * sizeof(float));
    taskEXIT_CRITICAL(&s_mux);
}
