#include "rtc_pcf8563.h"
#include "board_pins.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

static const char *TAG = "rtc";
static bool s_initialized = false;

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

static uint8_t bcd2dec(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }
static uint8_t dec2bcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

/* Write [reg, data...] then immediately re-start and read len bytes back. */
static esp_err_t i2c_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len,
                                       pdMS_TO_TICKS(100));
}

/* Write [reg, data...] in a single transaction. */
static esp_err_t i2c_write_reg(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[8];   /* reg byte + up to 7 RTC data bytes */
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    return i2c_master_transmit(s_dev, buf, len + 1, pdMS_TO_TICKS(100));
}

void pcf8563_init(void)
{
    ESP_LOGI(TAG, "Initialising I2C for PCF8563 RTC");

    /* ── Create I2C master bus ── */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_PORT,
        .sda_io_num        = PIN_I2C_SDA,
        .scl_io_num        = PIN_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed");
        return;
    }

    /* ── Register PCF8563 device on the bus ── */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = RTC_I2C_ADDR,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed");
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return;
    }

    /* Clear PCF8563 control registers: 0x00/0x01 disable alarms + timer
     * interrupts; 0x0D is the CLKOUT control — the chip powers up with
     * CLKOUT ENABLED at 32.768 kHz (FE=1), a continuous square wave on the
     * CLKOUT pin (EMI source) unless explicitly cleared.  The old code only
     * wrote 0x00/0x01 and never touched 0x0D despite claiming clock-out was
     * disabled.  Writes are checked: if they fail the chip isn't responding,
     * so leave s_initialized false rather than paying an I²C timeout on
     * every rtc_get_time() call. */
    uint8_t zero = 0;
    if (i2c_write_reg(0x00, &zero, 1) != ESP_OK ||
        i2c_write_reg(0x01, &zero, 1) != ESP_OK ||
        i2c_write_reg(0x0D, &zero, 1) != ESP_OK) {
        ESP_LOGW(TAG, "PCF8563 not responding — RTC disabled");
        return;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "PCF8563 RTC ready");
}

bool rtc_get_time(struct tm *t)
{
    if (!s_initialized || !t) return false;

    uint8_t data[7];
    esp_err_t err = i2c_read_reg(0x02, data, 7);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I²C read failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Bit 7 of the seconds register is the Voltage Low (VL) flag.
     * When set the PCF8563 is signalling that the RTC crystal has been
     * without power long enough that the clock data may be corrupt.
     * Reject the read so callers don't seed the system clock with
     * garbage and show wildly wrong times. */
    if (data[0] & 0x80) {
        ESP_LOGW(TAG, "VL flag set — RTC data unreliable (raw: "
                 "%02x %02x %02x %02x %02x %02x %02x)",
                 data[0], data[1], data[2], data[3], data[4], data[5], data[6]);
        return false;
    }

    int sec  = bcd2dec(data[0] & 0x7F);
    int min  = bcd2dec(data[1] & 0x7F);
    int hour = bcd2dec(data[2] & 0x3F);
    int mday = bcd2dec(data[3] & 0x3F);
    int mon  = bcd2dec(data[5] & 0x1F);   /* 1-based, pre-decrement */
    int year = bcd2dec(data[6]);           /* 0-99 → add 100 for tm_year */

    /* Validate decoded fields before trusting them.  Out-of-range values
     * indicate a corrupt or uninitialised RTC rather than a BCD glitch. */
    if (sec  > 59 || min  > 59 || hour > 23 ||
        mday < 1  || mday > 31 || mon  < 1  || mon > 12) {
        ESP_LOGW(TAG, "RTC decoded out-of-range (%02d:%02d:%02d %02d/%02d/%04d) — ignoring",
                 hour, min, sec, mday, mon, 2000 + year);
        return false;
    }

    t->tm_sec  = sec;
    t->tm_min  = min;
    t->tm_hour = hour;
    t->tm_mday = mday;
    t->tm_wday = bcd2dec(data[4] & 0x07);
    t->tm_mon  = mon - 1;                  /* struct tm months are 0-based */
    t->tm_year = year + 100;               /* PCF8563 year 0-99 → 2000-2099 */
    t->tm_isdst = -1;                      /* let mktime determine DST */

    ESP_LOGD(TAG, "RTC read: %04d-%02d-%02d %02d:%02d:%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
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

i2c_master_bus_handle_t pcf8563_get_bus_handle(void)
{
    return s_bus;
}
