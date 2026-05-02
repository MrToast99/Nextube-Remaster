/**
 * @file display.h
 * @brief Nextube display driver – 6x ST7735 LCDs + JPEG asset renderer
 *
 * Each tube LCD is 80×160 px RGB565.  Images are loaded from SPIFFS
 * using the active theme.  Asset layout (mirrored from original firmware):
 *
 *   /images/themes/{theme}/Numbers/{0-9}.jpg
 *   /images/themes/{theme}/AMPM/{am,pm,colon,blank,dot,…}.jpg
 *   /images/themes/{theme}/MutiInfo/Weather/{sun,rain,…}.jpg
 *   /images/themes/{theme}/MutiInfo/Temperature/{degreec,degreef,minus}.jpg
 *   /images/themes/{theme}/MutiInfo/Humidity/{degree,humidity}.jpg
 *   /images/themes/{theme}/MutiInfo/WeekDate/week/{monday,…}.jpg
 *   /images/themes/{theme}/MutiInfo/WeekDate/date/{0-9}.jpg
 *   /images/system/{matrix,setting,waiting}/
 *
 * Available themes: NixieOY, FlipClock, DarkSlate, DotMatrixRG, DotMatrixY,
 *   Formula1, GlitchGR, LightFuture, NotionRain, RedDigits, RetroPaper,
 *   WireMesh, Custom, Custom01, Custom02, Custom03
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ── Hardware constants ────────────────────────────────────────────── */
#define LCD_WIDTH    80
#define LCD_HEIGHT  160
#define LCD_COUNT     6

/* ── Low-level hardware ────────────────────────────────────────────── */
void display_init(void);
void display_set_brightness(uint8_t pct);
void display_fill(int tube, uint16_t colour);
void display_show_digit(int tube, const uint8_t *rgb565_data, int w, int h);

/* ── JPEG asset loader ─────────────────────────────────────────────── */
/** Load JPEG from SPIFFS, decode RGB565, push to tube.  Falls back to
 *  black fill on any error.  Uses 8 MB PSRAM decode buffer. */
void display_show_image(int tube, const char *path);

/** Returns a human-readable description of the last JPEG decode failure
 *  (path + decoded dimensions), or NULL if no error has occurred since
 *  the last theme change.  String is valid until the next theme switch. */
const char *display_get_theme_error(void);

/* ── Path builders ─────────────────────────────────────────────────── */
void display_path_number     (char *buf, size_t n, const char *theme, int digit);
void display_path_ampm       (char *buf, size_t n, const char *theme, const char *name);
void display_path_weather    (char *buf, size_t n, const char *theme, const char *cond);
void display_path_temperature(char *buf, size_t n, const char *theme, const char *name);
void display_path_humidity   (char *buf, size_t n, const char *theme, const char *name);
void display_path_weekday    (char *buf, size_t n, const char *theme, int wday);
void display_path_date_digit (char *buf, size_t n, const char *theme, int digit);
void display_path_system     (char *buf, size_t n, const char *cat,   const char *name);

/* ── High-level helpers ────────────────────────────────────────────── */
void display_show_number(int tube, int digit,        const char *theme);
void display_show_ampm  (int tube, const char *name, const char *theme);

/* ── Debug helpers ─────────────────────────────────────────────────── */
/**
 * Override LEDC timer 0 / channel 0 (backlight GPIO) frequency and duty at
 * runtime for hardware diagnostics.
 *   freq_hz:    1–80 000 Hz
 *   duty_pct:   0–100 (note active-LOW: 0 = fully off, 100 = full bright)
 * Changes are immediate.  The display task's next render tick (≤200 ms) will
 * re-write the duty register via display_set_brightness(), overriding any
 * previous duty_pct — call display_debug_restore_pwm() to reset the frequency
 * back to 50 kHz and let the task take over.
 */
void display_debug_set_pwm(uint32_t freq_hz, uint8_t duty_pct);

/** Restore LEDC timer 0 to 50 kHz.  The display task corrects duty within
 *  200 ms.  Safe to call from any task. */
void display_debug_restore_pwm(void);

/* ── Display task ──────────────────────────────────────────────────── */
/** Launch the FreeRTOS display task (core 1, 5 Hz).  Re-renders
 *  whenever mode / time / weather / subscriber count changes.
 *  Call once after display_init(). */
void display_task_start(void);

/** Reset the countdown / pomodoro internal timer (call on mode entry). */
void display_timer_reset(void);

/** Toggle the countdown / pomodoro timer between running and paused.
 *  Safe to call from any task; uses an internal mutex. */
void display_timer_toggle(void);

/** Invalidate the album image cache.  Call after adding or removing files
 *  under /spiffs/images/album/ so the display task re-scans the directory
 *  on its next render cycle. */
void display_album_invalidate(void);

#ifdef __cplusplus
}
#endif
