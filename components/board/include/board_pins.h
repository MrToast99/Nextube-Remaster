/**
 * @file board_pins.h
 * @brief Nextube hardware pin definitions
 *
 * Reverse-engineered from Rotrics Nextube clock (HW Rev 1.31 2022/01/19)
 * ESP32-WROVER-E: 16MB Flash, 8MB PSRAM
 */
#pragma once

/* ── SPI Bus (shared by all 6 LCD displays) ────────────────────────── */
#define PIN_LCD_SCK       12
#define PIN_LCD_MOSI      13
#define PIN_LCD_DC        14
#define PIN_LCD_RST       27
#define PIN_LCD_BACKLIGHT  19   /* PWM-capable */

/* ── LCD Chip Selects (active low) ─────────────────────────────────── */
#define PIN_LCD1_CS       33   /* leftmost tube */
#define PIN_LCD2_CS       26
#define PIN_LCD3_CS       21
#define PIN_LCD4_CS        0
#define PIN_LCD5_CS        5
#define PIN_LCD6_CS       18   /* rightmost tube */

#define LCD_COUNT          6

/* ── WS2812 RGB LEDs ───────────────────────────────────────────────── */
#define PIN_LED_DATA      32
#define LED_COUNT          6

/* ── Touch Pads ────────────────────────────────────────────────────── */
#define PIN_TOUCH_LEFT     2   /* TOUCH2 */
#define PIN_TOUCH_MIDDLE   4   /* TOUCH0 */
#define PIN_TOUCH_RIGHT   15   /* TOUCH3 */

/* ── I²C Bus (RTC + optional sensors) ──────────────────────────────── */
#define PIN_I2C_SCL       22
#define PIN_I2C_SDA       23
#define I2C_PORT          I2C_NUM_0
#define I2C_FREQ_HZ       100000

/* ── Audio (DAC → LTK8002D amplifier) ──────────────────────────────── */
#define PIN_AUDIO_DAC     25   /* DAC_CHANNEL_1 */

/* ── RTC ───────────────────────────────────────────────────────────── */
#define RTC_I2C_ADDR      0x51 /* PCF8563 */

/* ── SHT30 Temperature/Humidity (optional) ─────────────────────────── */
#define SHT30_I2C_ADDR    0x44

/* ── Display characteristics ───────────────────────────────────────── */
#define LCD_WIDTH          80
#define LCD_HEIGHT        160
#define LCD_OFFSET_X        0  /* ST7735 column offset   */
#define LCD_OFFSET_Y       26  /* ST7735 row offset      */
