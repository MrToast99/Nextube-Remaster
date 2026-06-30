#include "sht30.h"
#include "board_pins.h"
#include "rtc_pcf8563.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "sht30";

/* Single-shot measurement, high repeatability, no clock-stretching.
 * Sensor needs ~15 ms; we wait 20 ms to be safe. */
#define CMD_MEAS_HI_NCS_MSB  0x24
#define CMD_MEAS_HI_NCS_LSB  0x00
#define MEAS_WAIT_MS         20

/* Sensirion CRC-8: poly = 0x31, init = 0xFF, no reflect, no XOR-out */
static uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x31) : (crc << 1);
    }
    return crc;
}

static i2c_master_dev_handle_t s_dev       = NULL;
static bool                    s_present   = false;
static sht30_reading_t         s_last      = { .valid = false };
static SemaphoreHandle_t       s_mutex     = NULL;
static float                   s_offset_c  = 0.0f;  /* temperature correction offset */

bool sht30_init(void)
{
    i2c_master_bus_handle_t bus = pcf8563_get_bus_handle();
    if (!bus) {
        ESP_LOGE(TAG, "I²C bus not initialised (call pcf8563_init first)");
        return false;
    }

    /* Probe: send address byte only and check for ACK */
    esp_err_t probe = i2c_master_probe(bus, SHT30_I2C_ADDR, 50);
    if (probe != ESP_OK) {
        ESP_LOGI(TAG, "SHT30 not found at 0x%02X (probe: %s) — sensor absent",
                 SHT30_I2C_ADDR, esp_err_to_name(probe));
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = SHT30_I2C_ADDR,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register SHT30 on I²C bus: %s", esp_err_to_name(err));
        return false;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex — sensor disabled");
        return false;
    }
    s_present = true;
    ESP_LOGI(TAG, "SHT30 found at 0x%02X", SHT30_I2C_ADDR);
    return true;
}

bool sht30_is_present(void) { return s_present; }

bool sht30_read(sht30_reading_t *out)
{
    if (!s_present || !out) return false;

    /* Trigger measurement */
    uint8_t cmd[2] = { CMD_MEAS_HI_NCS_MSB, CMD_MEAS_HI_NCS_LSB };
    esp_err_t err = i2c_master_transmit(s_dev, cmd, sizeof(cmd), pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Measurement trigger failed: %s", esp_err_to_name(err));
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(MEAS_WAIT_MS));

    /* Read 6 bytes: [T_msb, T_lsb, T_crc, H_msb, H_lsb, H_crc] */
    uint8_t buf[6];
    err = i2c_master_receive(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Read failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Validate CRC for temperature and humidity words */
    if (crc8(buf, 2) != buf[2]) {
        ESP_LOGW(TAG, "Temperature CRC mismatch");
        return false;
    }
    if (crc8(buf + 3, 2) != buf[5]) {
        ESP_LOGW(TAG, "Humidity CRC mismatch");
        return false;
    }

    uint16_t raw_t = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t raw_h = ((uint16_t)buf[3] << 8) | buf[4];

    out->temp_c   = -45.0f + 175.0f * (float)raw_t / 65535.0f;
    out->humidity = 100.0f * (float)raw_h / 65535.0f;
    out->valid    = true;
    return true;
}

static void sht30_task(void *arg)
{
    (void)arg;
    if (!s_present) { vTaskDelete(NULL); return; }

    ESP_LOGI(TAG, "Sensor task started (30 s interval)");

    for (;;) {
        sht30_reading_t reading = {0};
        if (sht30_read(&reading)) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_last = reading;
            xSemaphoreGive(s_mutex);
            ESP_LOGI(TAG, "%.1f °C  %.1f %%RH  (stack hwm: %u)",
                     reading.temp_c, reading.humidity,
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        vTaskDelay(pdMS_TO_TICKS(30 * 1000));
    }
}

void sht30_set_offset(float offset_c)
{
    /* Clamp to ±20 °C to catch obvious misconfiguration */
    if (offset_c >  20.0f) offset_c =  20.0f;
    if (offset_c < -20.0f) offset_c = -20.0f;
    /* s_offset_c is a single float — atomic on Xtensa; no mutex needed */
    s_offset_c = offset_c;
    ESP_LOGI(TAG, "Temperature offset set to %+.1f °C", (double)offset_c);
}

void sht30_task_start(void)
{
    if (xTaskCreate(sht30_task, "sht30", 4096, NULL, 4, NULL) != pdPASS)
        ESP_LOGE(TAG, "sht30_task creation failed");
}

const sht30_reading_t *sht30_get(void)
{
    static sht30_reading_t copy;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        copy = s_last;
        xSemaphoreGive(s_mutex);
    } else {
        copy = s_last;
    }
    /* Apply calibration offset — corrects for ESP32 self-heating or sensor
     * placement.  Humidity is not affected; only temperature is offset. */
    if (copy.valid)
        copy.temp_c += s_offset_c;
    return &copy;
}
