/**
 * @file microphone.c
 * @brief Analog microphone input – ADC oneshot + Goertzel 6-band analyser
 *
 * Hardware: GPIO35 (ADC1_CH7) confirmed via runtime debug panel.
 * Mic type: suspected analog output (electret capsule or analog MEMS).
 *   If the ADC reading barely moves with loud sounds (< ±15 counts), the
 *   module is likely a digital MEMS mic (I²S / PDM) and this driver will
 *   not work — a full I²S/PDM driver rewrite would be required.
 *   Quiescent ADC reading should be ~1800–2750 counts (mid-scale) for an
 *   analog mic biased to ~1.5–2.2 V.
 *
 * NOTE: ADC_ATTEN_DB_12 used (0–3.3 V full-scale range).
 *
 * Approach:
 *   • adc_oneshot API – uses the SAR ADC directly, no I2S dependency
 *     (adc_continuous / adc_dma would conflict with dac_continuous on I2S0)
 *   • Dedicated FreeRTOS task on core 1, esp_timer_get_time() busy-wait for
 *     precise 125 µs sample spacing (8 kHz)
 *   • DC removal: exponential moving average subtracted each sample
 *   • Goertzel algorithm: energy at 6 logarithmic centre frequencies
 *   • MIC_GAIN / MIC_NOISE_FLOOR tuneable at top of file for different mic types
 *   • Peak-hold with exponential decay (attack = instant, release per frame)
 *   • Normalised float[6] output published under a portMUX_TYPE spinlock
 */

#include "microphone.h"
#include "config_mgr.h"
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

/* ── Sensitivity tuning ───────────────────────────────────────────────── */
/*
 * Hardware context: LMV321IDBVR single op-amp on the PCB acts as an analog
 * preamp between the electret capsule and GPIO35.  Hardware gain is already
 * applied; no large software gain is needed or helpful.
 *
 * MIC_GAIN: additional software multiplier after the op-amp.
 *   1.0  = no extra boost   ← default (op-amp does the work)
 *   2.0  = gentle boost     (if op-amp gain resistors are small)
 *
 * MIC_SILENCE_GATE is now runtime-configurable via cfg->mic_silence_gate
 * (set through the web debug panel without a rebuild).  The #define below
 * is kept only as a reference value and is not used in code.
 *   250.0  = gate at ~16 counts RMS  (typical ADC noise floor)
 *   500.0  = stricter (only respond to nearby speech)
 *   100.0  = looser  (shows distant or quiet sounds)
 *     0.0  = disable gate entirely (always run Goertzel)
 *
 * MIC_NOISE_FLOOR: minimum Goertzel normalisation divisor (active frames
 * only — silence-gated frames always output 0.0).
 *   1.0  = bars are relative to the loudest band in each frame  ← default
 *   0.5  = slightly more sensitive within a non-silent frame */
#define MIC_GAIN          1.0f
#define MIC_SILENCE_GATE  250.0f   /* reference only — actual value from config */
#define MIC_NOISE_FLOOR   1.0f

/* Centre frequencies for the 6 bands (logarithmically spaced, 125 Hz – 4 kHz) */
static const float BAND_FREQS[BAND_COUNT] = {125.0f, 250.0f, 500.0f,
                                              1000.0f, 2000.0f, 4000.0f};

/* ── Shared output state ─────────────────────────────────────────────── */
static float        s_bands[BAND_COUNT];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ── ADC1 channel → GPIO mapping ─────────────────────────────────────── */
static const adc_channel_t ADC1_CHAN_MAP[8] = {
    ADC_CHANNEL_0, ADC_CHANNEL_1, ADC_CHANNEL_2, ADC_CHANNEL_3,
    ADC_CHANNEL_4, ADC_CHANNEL_5, ADC_CHANNEL_6, ADC_CHANNEL_7,
};
static const int ADC1_GPIO_MAP[8] = { 36, 37, 38, 39, 32, 33, 34, 35 };

/* ── ADC handle + currently configured channel ───────────────────────── */
static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t             s_active_chan = ADC_CHANNEL_0;

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
        /* ── Gate: only sample when mic is enabled AND Spectrum mode is active ── */
        {
            const nextube_config_t *cfg = config_get();
            bool spectrum_enabled = (cfg->enabled_modes & (1u << APP_MODE_SPECTRUM)) != 0;
            if (!cfg->mic_enabled ||
                !spectrum_enabled ||
                cfg->current_mode != APP_MODE_SPECTRUM) {
                /* Clear the published bands so Spectrum mode shows silence */
                taskENTER_CRITICAL(&s_mux);
                memset(s_bands, 0, sizeof(s_bands));
                taskEXIT_CRITICAL(&s_mux);
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
        }

        /* ── Switch ADC channel if config changed since last frame ── */
        {
            uint8_t cfg_ch = config_get()->mic_adc_channel;
            if (cfg_ch > 7) cfg_ch = 0;
            adc_channel_t want = ADC1_CHAN_MAP[cfg_ch];
            if (want != s_active_chan) {
                adc_oneshot_chan_cfg_t ccfg = {
                    .atten    = ADC_ATTEN_DB_12,
                    .bitwidth = ADC_BITWIDTH_12,
                };
                if (adc_oneshot_config_channel(s_adc, want, &ccfg) == ESP_OK) {
                    ESP_LOGI(TAG, "ADC channel switched to CH%d (GPIO%d)",
                             cfg_ch, ADC1_GPIO_MAP[cfg_ch]);
                    s_active_chan = want;
                }
            }
        }

        /* ── Collect FRAME_SIZE samples at exactly SAMPLE_RATE ── */
        int64_t t = esp_timer_get_time();
        for (int i = 0; i < FRAME_SIZE; i++) {
            int raw = 2048;
            adc_oneshot_read(s_adc, s_active_chan, &raw);
            /* Exponential moving average DC removal */
            dc        = DC_ALPHA * dc + (1.0f - DC_ALPHA) * (float)raw;
            samples[i] = (float)raw - dc;
            /* Advance deadline and busy-wait */
            t += SAMPLE_US;
            while (esp_timer_get_time() < t) { /* spin – < 125 µs */ }
        }

        /* ── Silence gate: skip Goertzel if frame energy is below noise floor ──
         * Computes per-sample RMS² (mean squared amplitude after DC removal).
         * ADC board noise on ESP32 is typically 10–20 counts RMS (RMS²≈100–400).
         * Threshold read from config each frame so the web debug panel takes
         * effect immediately without a rebuild or reboot. */
        float rms_sq = 0.0f;
        for (int i = 0; i < FRAME_SIZE; i++)
            rms_sq += samples[i] * samples[i];
        rms_sq /= (float)FRAME_SIZE;

        const float gate = config_get()->mic_silence_gate;
        if (gate > 0.0f && rms_sq < gate) {
            /* Silent frame — decay peaks but publish zeros */
            for (int b = 0; b < BAND_COUNT; b++)
                peak[b] *= DECAY;
            taskENTER_CRITICAL(&s_mux);
            memset(s_bands, 0, sizeof(s_bands));
            taskEXIT_CRITICAL(&s_mux);
            continue;
        }

        /* ── Goertzel energy + peak-hold per band (active frames only) ── */
        float max_power = MIC_NOISE_FLOOR;
        float power[BAND_COUNT];
        for (int b = 0; b < BAND_COUNT; b++) {
            power[b] = goertzel(samples, FRAME_SIZE, BAND_FREQS[b]) / norm
                       * MIC_GAIN;
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
    uint8_t cfg_ch = config_get()->mic_adc_channel;
    if (cfg_ch > 7) cfg_ch = 0;
    s_active_chan = ADC1_CHAN_MAP[cfg_ch];

    ESP_LOGI(TAG, "Initialising ADC1/CH%d for mic on GPIO%d",
             cfg_ch, ADC1_GPIO_MAP[cfg_ch]);

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,    /* 0–3.3 V full-scale */
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_active_chan, &chan_cfg));
}

int mic_read_raw(void)
{
    if (!s_adc) return -1;
    /* Ensure the active channel matches config (in case channel changed while
     * mic was gated and mic_task hasn't re-entered the active loop yet). */
    uint8_t cfg_ch = config_get()->mic_adc_channel;
    if (cfg_ch > 7) cfg_ch = 0;
    adc_channel_t want = ADC1_CHAN_MAP[cfg_ch];
    if (want != s_active_chan) {
        adc_oneshot_chan_cfg_t ccfg = {
            .atten    = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        if (adc_oneshot_config_channel(s_adc, want, &ccfg) == ESP_OK)
            s_active_chan = want;
    }
    int raw = 2048;
    adc_oneshot_read(s_adc, s_active_chan, &raw);
    return raw;
}

int mic_gpio_num(void)
{
    uint8_t cfg_ch = config_get()->mic_adc_channel;
    if (cfg_ch > 7) cfg_ch = 0;
    return ADC1_GPIO_MAP[cfg_ch];
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
