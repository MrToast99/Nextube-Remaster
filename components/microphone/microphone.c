/**
 * @file microphone.c
 * @brief CMC-4015-25T electret + LMV321IDBVR preamp – ADC ISR timer + Goertzel
 *
 * Hardware: CMC-4015-25T capsule → LMV321IDBVR op-amp → GPIO35 (ADC1_CH7).
 *
 * WHY NOT adc_continuous:
 *   On the original ESP32 (LX6 / WROVER-E) the adc_continuous driver still uses
 *   I2S0 for DMA.  dac_continuous also uses I2S0.  They cannot coexist — confirmed
 *   by "i2s controller 0 has been occupied by dac_dma" abort at boot.
 *   (GDMA-based adc_continuous is only available on S2/S3/C3/S3 targets.)
 *
 * WHY NOT adc_oneshot + busy-wait:
 *   The 125 µs spin loop held core 1 at 100%, starving IDLE1 and triggering the
 *   task watchdog — confirmed by backtrace showing mic task running while IDLE1
 *   had not reset the WDT.
 *
 * CURRENT APPROACH — esp_timer ISR + adc_oneshot_read_isr():
 *   • A hardware esp_timer fires every SAMPLE_US (125 µs) with ISR dispatch.
 *   • The ISR calls adc_oneshot_read_isr() (~2–5 µs) and stores the raw sample
 *     into a ping-pong int16_t buffer.  No floats, no locks in the ISR.
 *   • After FRAME_SIZE samples the ISR flips the buffer and gives a binary
 *     semaphore to wake the analysis task.
 *   • mic_task blocks on the semaphore (yields properly) then runs Goertzel on
 *     the completed buffer.  Core 1 is idle between frames — no watchdog.
 *   • ISR overhead: ~3–8 µs per 125 µs tick ≈ 3–6 % of core time.
 *
 * Silence gate (runtime, debug panel): cfg->mic_silence_gate  (RMS² threshold).
 * Gain / noise floor: MIC_GAIN, MIC_NOISE_FLOOR  (compile-time).
 */

#include "microphone.h"
#include "config_mgr.h"
#include "board_pins.h"
#include "esp_adc/adc_oneshot.h"
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
#define BAND_COUNT      6
#define SAMPLE_US       (1000000 / SAMPLE_RATE)   /* 125 µs */
#define DECAY           0.85f
#define DC_ALPHA        0.999f

/* ── Sensitivity (compile-time) ──────────────────────────────────────── */
#define MIC_GAIN        1.0f    /* LMV321 handles hardware gain */
#define MIC_NOISE_FLOOR 1.0f   /* Goertzel normalisation floor for active frames */

/* ── Frequency bands ─────────────────────────────────────────────────── */
/* Log-spaced bands: 300 Hz → 4 kHz (Nyquist at 8 kHz sample rate).
 * Ratio ≈ 1.68× per step — avoids the 125/250 Hz range that attracts
 * SPI switching noise and mains hum on this hardware.
 *   T0=300  T1=500  T2=850  T3=1400  T4=2400  T5=4000 Hz */
static const float BAND_FREQS[BAND_COUNT] = {
    300.0f, 500.0f, 850.0f, 1400.0f, 2400.0f, 4000.0f
};

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

/* ── ISR ping-pong buffer ────────────────────────────────────────────── */
/* The ISR writes into buf[s_isr_write]; the task reads buf[1-s_isr_write].
 * s_isr_write is flipped atomically (single-byte store) after each frame. */
static int16_t            s_raw[2][FRAME_SIZE];
static volatile uint8_t   s_isr_write = 0;   /* which buffer the ISR is filling */
static volatile int       s_isr_pos   = 0;   /* sample index within current buffer */

/* ── Inter-task synchronisation ──────────────────────────────────────── */
static SemaphoreHandle_t   s_frame_sem = NULL;   /* binary; given by ISR each frame */

/* ── Shared output ───────────────────────────────────────────────────── */
static float        s_bands[BAND_COUNT];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ── Per-band adaptive noise floor ──────────────────────────────────── */
/* Long-term exponential average of each band's Goertzel energy.
 * Subtracted before peak-hold so the display reads zero in silence even
 * when the ADC / LMV321 preamp has a non-trivial electrical noise floor.
 * Adapts to the quiet-period baseline within ~8 s of startup. */
static float s_noise_floor[BAND_COUNT];   /* zero-initialised; adapts automatically */
#define NOISE_ALPHA  0.002f               /* time constant ≈ 500 frames × 16 ms ≈ 8 s */

/* Last raw ADC count (updated by ISR every sample) for the debug panel */
static volatile int s_last_raw = -1;

/* ── Timer handle ────────────────────────────────────────────────────── */
static esp_timer_handle_t s_timer = NULL;

/* ── Timer callback: fired every SAMPLE_US by esp_timer (ESP_TIMER_TASK) ── */
/* Runs in the esp_timer service task (priority 22, FreeRTOS task context).
 * adc_oneshot_read() is safe here — task context, no ISR-safe variant needed.
 * xSemaphoreGiveFromISR is safe from a high-priority task too (no harm using it). */
static void mic_sample_cb(void *arg)
{
    int raw = 2048;
    adc_oneshot_read(s_adc, s_active_chan, &raw);

    s_last_raw = raw;
    s_raw[s_isr_write][s_isr_pos] = (int16_t)raw;
    s_isr_pos++;

    if (s_isr_pos >= FRAME_SIZE) {
        s_isr_pos  = 0;
        s_isr_write ^= 1u;   /* flip write buffer */
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(s_frame_sem, &woken);
        portYIELD_FROM_ISR(woken);
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

/* ── ADC channel reconfigure (called from mic_task, not ISR) ─────────── */
static void reconfigure_channel(uint8_t cfg_ch)
{
    if (cfg_ch > 7) cfg_ch = 7;
    /* Stop timer so the ISR cannot call adc_oneshot_read_isr during reconfigure */
    if (s_timer) esp_timer_stop(s_timer);

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

    if (s_timer) esp_timer_start_periodic(s_timer, SAMPLE_US);
}

/* ── Mic sampling / analysis task ────────────────────────────────────── */
static void mic_task(void *arg)
{
    float   samples[FRAME_SIZE];
    float   dc   = 2048.0f;
    float   peak[BAND_COUNT];
    memset(peak, 0, sizeof(peak));
    int     noise_cal = 0;    /* counts frames toward noise floor calibration */

    const float norm = (float)(FRAME_SIZE * FRAME_SIZE) / 4.0f;

    ESP_LOGI(TAG, "mic_task running  (ISR timer @ %d Hz, no busy-wait)", SAMPLE_RATE);

    while (1) {
        /* Block here until the ISR signals a completed frame (or timeout 500 ms).
         * This yields the CPU — IDLE1 and other tasks run freely. */
        if (xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
            /* Timeout — probably gated; just loop */
            continue;
        }

        const nextube_config_t *cfg = config_get();

        /* ── Reconfigure ADC channel if debug panel changed it ── */
        uint8_t want_ch = cfg->mic_adc_channel;
        if (want_ch > 7) want_ch = 7;
        if (want_ch != s_active_ch) {
            reconfigure_channel(want_ch);
        }

        /* ── Gate: discard frame when mic disabled or not in Spectrum mode ── */
        bool spectrum_en = (cfg->enabled_modes & (1u << APP_MODE_SPECTRUM)) != 0;
        if (!cfg->mic_enabled || !spectrum_en ||
            cfg->current_mode != APP_MODE_SPECTRUM) {
            taskENTER_CRITICAL(&s_mux);
            memset(s_bands, 0, sizeof(s_bands));
            taskEXIT_CRITICAL(&s_mux);
            /* Drain stale semaphore signals that accumulated while gated */
            while (xSemaphoreTake(s_frame_sem, 0) == pdTRUE) { /* flush */ }
            continue;
        }

        /* ── Copy completed buffer (the one the ISR just finished writing) ─
         * s_isr_write is the buffer the ISR is NOW filling; read the OTHER one. */
        uint8_t read_buf = s_isr_write ^ 1u;   /* snapshot — ISR may flip again */

        /* DC removal + float conversion */
        for (int i = 0; i < FRAME_SIZE; i++) {
            float raw = (float)s_raw[read_buf][i];
            dc          = DC_ALPHA * dc + (1.0f - DC_ALPHA) * raw;
            samples[i]  = raw - dc;
        }

        /* ── Silence gate (runtime tuneable via debug panel) ── */
        float rms_sq = 0.0f;
        for (int i = 0; i < FRAME_SIZE; i++)
            rms_sq += samples[i] * samples[i];
        rms_sq /= (float)FRAME_SIZE;

        const float gate = cfg->mic_silence_gate;
        if (gate > 0.0f && rms_sq < gate) {
            for (int b = 0; b < BAND_COUNT; b++) peak[b] *= DECAY;
            taskENTER_CRITICAL(&s_mux);
            memset(s_bands, 0, sizeof(s_bands));
            taskEXIT_CRITICAL(&s_mux);
            continue;
        }

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
        float max_power = MIC_NOISE_FLOOR;
        float power[BAND_COUNT];
        for (int b = 0; b < BAND_COUNT; b++) {
            float raw = goertzel(samples, FRAME_SIZE, BAND_FREQS[b]) / norm * MIC_GAIN;
            if (noise_cal < 250) {
                /* Phase 1: fast unconstrained convergence */
                s_noise_floor[b] += 0.02f * (raw - s_noise_floor[b]);
            } else if (raw < s_noise_floor[b] * 4.0f) {
                /* Phase 2: slow guarded tracking */
                s_noise_floor[b] += NOISE_ALPHA * (raw - s_noise_floor[b]);
            }
            power[b] = raw - s_noise_floor[b];
            if (power[b] < 0.0f) power[b] = 0.0f;
        }
        if (noise_cal < 250) noise_cal++;

        /* ── Peak-hold ── */
        for (int b = 0; b < BAND_COUNT; b++) {
            if (power[b] > peak[b])  peak[b]  = power[b];
            else                     peak[b] *= DECAY;
            if (peak[b] > max_power) max_power = peak[b];
        }

        /* ── Publish normalised 0.0–1.0 values ── */
        taskENTER_CRITICAL(&s_mux);
        for (int b = 0; b < BAND_COUNT; b++)
            s_bands[b] = peak[b] / max_power;
        taskEXIT_CRITICAL(&s_mux);
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void mic_init(void)
{
    uint8_t cfg_ch = config_get()->mic_adc_channel;
    if (cfg_ch > 7) cfg_ch = 7;
    s_active_ch   = cfg_ch;
    s_active_chan  = ADC1_CHAN_MAP[cfg_ch];

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

    /* Binary semaphore: ISR gives, task takes */
    s_frame_sem = xSemaphoreCreateBinary();
    configASSERT(s_frame_sem);

    /* Periodic timer — fires every SAMPLE_US in the esp_timer service task.
     * Task dispatch (default) lets us call adc_oneshot_read() safely.
     * No ISR-safe ADC variant needed; no busy-wait; watchdog stays happy. */
    esp_timer_create_args_t targs = {
        .callback              = mic_sample_cb,
        .arg                   = NULL,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "mic_samp",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_timer, SAMPLE_US));

    ESP_LOGI(TAG, "Sampling timer started: %u µs period  (%.0f Hz, TASK dispatch)", SAMPLE_US, (float)SAMPLE_RATE);
}

int mic_read_raw(void)
{
    /* Returns the most recent ADC count captured by the ISR.
     * Valid immediately after mic_init(); -1 before first sample. */
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
    /* Stack 4096: samples[512 B] + Goertzel locals + overhead */
    xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "mic_task started (core 1)");
}

void mic_get_bands(float out[6])
{
    taskENTER_CRITICAL(&s_mux);
    memcpy(out, s_bands, BAND_COUNT * sizeof(float));
    taskEXIT_CRITICAL(&s_mux);
}
