#include "rtc_pcf8563.h"
#include "board_pins.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include <string.h>

static const char *TAG = "rtc";
static bool s_initialized = false;

static uint8_t bcd2dec(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }
static uint8_t dec2bcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

static esp_err_t i2c_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(I2C_PORT, RTC_I2C_ADDR, &reg, 1, data, len, pdMS_TO_TICKS(100));
}

static esp_err_t i2c_write_reg(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[len + 1];
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    return i2c_master_write_to_device(I2C_PORT, RTC_I2C_ADDR, buf, len + 1, pdMS_TO_TICKS(100));
}

void rtc_init(void)
{
    ESP_LOGI(TAG, "Initialising I2C for PCF8563 RTC");

    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = PIN_I2C_SDA,
        .scl_io_num       = PIN_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    i2c_param_config(I2C_PORT, &conf);
    esp_err_t err = i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(err));
        return;
    }

    /* Clear control registers */
    uint8_t zero = 0;
    i2c_write_reg(0x00, &zero, 1);
    i2c_write_reg(0x01, &zero, 1);
    s_initialized = true;
    ESP_LOGI(TAG, "PCF8563 RTC ready");
}

bool rtc_get_time(struct tm *t)
{
    if (!s_initialized || !t) return false;

    uint8_t data[7];
    esp_err_t err = i2c_read_reg(0x02, data, 7);
    if (err != ESP_OK) return false;

    t->tm_sec  = bcd2dec(data[0] & 0x7F);
    t->tm_min  = bcd2dec(data[1] & 0x7F);
    t->tm_hour = bcd2dec(data[2] & 0x3F);
    t->tm_mday = bcd2dec(data[3] & 0x3F);
    t->tm_wday = bcd2dec(data[4] & 0x07);
    t->tm_mon  = bcd2dec(data[5] & 0x1F) - 1;
    t->tm_year = bcd2dec(data[6]) + 100;  /* PCF8563 year 0-99 -> 2000-2099 */
    return true;
}

bool rtc_set_time(const struct tm *t)
{
    if (!s_initialized || !t) return false;

    uint8_t data[7] = {
        dec2bcd(t->tm_sec),
        dec2bcd(t->tm_min),
        dec2bcd(t->tm_hour),
        dec2bcd(t->tm_mday),
        dec2bcd(t->tm_wday),
        dec2bcd(t->tm_mon + 1),
        dec2bcd(t->tm_year - 100),
    };
    return i2c_write_reg(0x02, data, 7) == ESP_OK;
}
