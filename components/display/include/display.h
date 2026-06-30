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
 *
 * Available themes: NixieOY, FlipClock, DarkSlate, DotMatrixRG, DotMatrixY,
 *   Formula1, GlitchGR, LightFuture, NotionRain, RedDigits, RetroPaper,
 *   WireMesh
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

/** Send INVON (0x21) or INVOFF (0x20) to each tube according to mask.
 *  Bit N set → tube N gets INVON (colour-inverted replacement panel).
 *  Takes effect immediately; no reboot required.
 *  Call after display_init() and again when lcd_invert_mask changes. */
void display_apply_invert_mask(uint8_t mask);

/** Apply per-tube panel profile (VCOM voltage + gamma curve).
 *  0 = Standard — tuned for the original Green-Tab ST7735 panels.
 *  1 = Vivid    — ST7735S replacement panels (e.g. LH096NT-IF09W) that appear
 *                 washed/low-contrast at Standard.  Raises VCOM from 0x0E → 0x3C
 *                 and recalibrates gamma for the ST7735S transfer curve.
 *  Takes effect immediately by re-sending VCOM + gamma registers; no full
 *  reinit or screen clear.  Call after display_init() and on settings change. */
void display_apply_init_profiles(const uint8_t profiles[6]);

/** Set per-tube VMCTR1 VCOM value (0x00–0x3F; default 0x0E = 14).
 *  VCOM controls the AC driving voltage; raising it restores contrast and colour
 *  saturation on replacement panels that look washed at the original 0x0E setting.
 *  Changes require a per-tube SWRESET + full register reload (same as profile
 *  changes) because VMCTR1 is only latched during the SLPOUT→DISPON window.
 *  The display task is suspended for the duration so the SPI bus is not contested.
 *  Typical values: 0x0E (14) Standard / 0x3C (60) Vivid preset / 0x3F (63) max.
 *  Call after display_init() and again when lcd_vcom changes. */
void display_apply_tube_vcom(const uint8_t vcom[6]);

/** Apply per-tube software gamma correction to every pixel in display_show_digit().
 *  Builds a 32-entry (R/B) and 64-entry (G) lookup table per tube from
 *  out = in^gamma, then applies it per-pixel inside the SPI chunk loop
 *  (pure integer math, no float ops per pixel).  Takes effect on the next
 *  render tick.  gamma[i] = 1.0 → identity, LUT bypassed for that tube
 *  (zero overhead).  gamma[i] > 1.0 → darkens midtones; fixes washed /
 *  low-contrast panels.  gamma[i] < 1.0 → brightens midtones.
 *  Each element clamped to [0.5, 3.0].  Call after display_init(). */
void display_apply_tube_gamma(const float gamma[6]);

/** Set per-tube CASET/RASET user fine-tuning offsets.
 *  col_off[i] / row_off[i] are added to the effective column/row origin,
 *  which is already LCD_OFFSET_X/Y + the profile-level GRAM offset (profile 1
 *  "Vivid" contributes +2/+1 automatically for ST7735S replacement panels).
 *  Use these for additional per-unit trimming beyond the profile default.
 *  Range -8..+8.  Thread-safe; takes effect on the next render tick.
 *  NOTE: Users who previously set col_off=+1 as a workaround for replacement
 *  panel misalignment should set it back to 0 after switching to profile 1,
 *  since the profile now handles the base +1 column offset automatically.
 *  Panels that need COLSTART=26 (vs the more common 25) should set
 *  lcd_col_offset=+1 as additional fine-tuning on top of the profile. */
void display_apply_tube_offsets(const int8_t col_off[6], const int8_t row_off[6]);

/** Set per-tube software brightness scale (0-100; 100 = no scaling, default).
 *  RGB565 pixel components (R5, G6, B5) are multiplied by br/100 per-pixel
 *  during each display_show_digit() call. Overhead is negligible at 5 Hz
 *  (~12 800 pixel ops/tube, all integer arithmetic, no memory allocation).
 *  Use to match replacement panels that are noticeably brighter than originals.
 *  Thread-safe: values are cached; takes effect on the next render tick.
 *  Call after display_init() and again when lcd_tube_brightness changes. */
void display_apply_tube_brightness(const uint8_t br[6]);

/* ── JPEG asset loader ─────────────────────────────────────────────── */
/** Load JPEG from SPIFFS, decode RGB565, push to tube.  Falls back to
 *  black fill on any error.  Uses 8 MB PSRAM decode buffer. */
void display_show_image(int tube, const char *path);

/** Returns a human-readable description of the last JPEG decode failure
 *  (path + decoded dimensions), or NULL if no error has occurred since
 *  the last theme change.
 *  The returned pointer is into an internal static buffer — valid only until
 *  the next theme change.  Callers must NOT free() it or store it across a
 *  theme switch. */
const char *display_get_theme_error(void);

/* ── Path builders ─────────────────────────────────────────────────── */
void display_path_number     (char *buf, size_t n, const char *theme, int digit);
void display_path_ampm       (char *buf, size_t n, const char *theme, const char *name);
void display_path_weather    (char *buf, size_t n, const char *theme, const char *cond);
void display_path_temperature(char *buf, size_t n, const char *theme, const char *name);
void display_path_humidity   (char *buf, size_t n, const char *theme, const char *name);

/* ── High-level helpers ────────────────────────────────────────────── */
void display_show_number(int tube, int digit,        const char *theme);
void display_show_ampm  (int tube, const char *name, const char *theme);

/** Invalidate the PNG-vs-JPG format cache for theme assets.
 *  Call after any hotpatch or WebUI pull that adds or removes theme PNGs,
 *  so the next render re-probes rather than serving the stale cached result. */
void display_theme_cache_flush(void);

/* ── Update indicator ──────────────────────────────────────────────── */
/** Activate or clear the clock-face firmware-update indicator.
 *  When active, a 4-row red bar is drawn at the physical bottom of tube 6
 *  (LCD_COUNT-1) on every display frame, on top of whatever mode is shown.
 *  Intended to be driven by the web UI's update-check logic:
 *    - set true  when an update is available and the user has enabled
 *                "clock face update notification" in Display settings
 *    - set false when the update toast is dismissed or the feature disabled
 *  Thread-safe: safe to call from any task. */
void display_set_update_indicator(bool active);

/* ── Anti burn-in ──────────────────────────────────────────────────── */
/** Start or stop per-tube burn-in colour-cycle mode.
 *  mask:       bitmask, bit N = tube N.  0x3F = all six.  0x00 = restore all.
 *  duration_s: 0 = run until manually stopped (mask=0 call required).
 *              Non-zero = auto-restore after this many seconds
 *              (3600 = 1 h, 7200 = 2 h, 10800 = 3 h, 14400 = 4 h).
 *  While any bit is set, masked tubes cycle through red→green→blue→white→black
 *  (30 s per step) to exercise every sub-pixel at both voltage extremes.
 *  Unmasked tubes render normally.  Backlight is unchanged.
 *  The display task also shifts the CASET window by ±2 px every hour
 *  automatically — no API call needed for that.
 *  Thread-safe: safe to call from any task. */
void display_set_burnin_mask(uint8_t mask, uint32_t duration_s);

/** Static-snow burn-in mode: each display tick writes truly random RGB565
 *  pixels to every tube in mask, exercising each sub-pixel independently.
 *  More thorough than the colour-cycle because every pixel address receives
 *  a unique random level on every frame rather than a global solid colour.
 *  mask:       6-bit field, bit N = tube N (0x3F = all six tubes).
 *  duration_s: session length in seconds; 0 = run until stopped (mask = 0).
 *  Calling with mask = 0 stops an active session immediately.
 *  Thread-safe: safe to call from any task. */
void display_set_snow_mask(uint8_t mask, uint32_t duration_s);

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

/** Show the system wait screen on all six tubes and permanently suspend
 *  the display task so it cannot contest the SPI bus.
 *
 *  Call this immediately before starting any flash operation (OTA firmware
 *  update or LittleFS bin flash).  The image is loaded from
 *  /images/system/wait.jpg while the filesystem is still mounted.
 *
 *  There is no matching resume call: both flash paths always end with
 *  esp_restart(), so the display task is never un-suspended.
 *
 *  Safe to call from any task after display_task_start(). */
void display_show_wait(void);

/** Push an externally-supplied JPG to a 24H_CX info-panel tube (asset themes).
 *  Decodes the JPG (which must be exactly LCD_WIDTH×LCD_HEIGHT = 80×160) to
 *  RGB565 in a per-tube PSRAM buffer; render_cx_panel() blits it whenever that
 *  tube's "Pushed image" panel is the active rotation slot.
 *  which: 0 = tube 5 (LCD 4, dual mode), 1 = tube 6 (LCD 5, rightmost).
 *  Returns true on a successful decode.  Safe to call from the httpd task. */
bool display_cx_push_image(int which, const uint8_t *jpg, size_t len);

/** Hint that a CPU/flash-bound operation is running for the next `ms`
 *  milliseconds (e.g. a config save).  While active, the WeatherLive realtime
 *  animation — the only theme that re-renders every tick — skips its frames so
 *  the higher-priority display task doesn't starve the httpd task on the same
 *  core.  Self-expiring; calls extend (never shorten) the window.  No effect on
 *  other themes (they render only on change).  Safe to call from any task. */
void display_busy_hint(uint32_t ms);

/** Cancel a pending busy hint immediately.  Call after the CPU/flash-bound
 *  operation finishes so the display re-renders at the next tick rather than
 *  waiting for the hint's original timeout to expire.  Safe to call from any
 *  task. */
void display_busy_clear(void);

/** Signal that a settings save just completed.  Clears any busy backoff,
 *  forces a full re-render on the very next display tick (same effect as a
 *  mode change but without blanking the tubes), and wakes the display task
 *  immediately.  Ensures live config edits — shadow colour, glyph colour,
 *  custom font, etc. — appear within one tick regardless of change-detection
 *  cursor state.  Safe to call from any task. */
void display_config_changed(void);

/** Reset the countdown / pomodoro internal timer (call on mode entry). */
void display_timer_reset(void);

/** Toggle the countdown / pomodoro timer between running and paused.
 *  Safe to call from any task; uses an internal mutex. */
void display_timer_toggle(void);

/** Invalidate the album image cache.  Call after adding or removing files
 *  under /spiffs/images/album/ so the display task re-scans the directory
 *  on its next render cycle. */
void display_album_invalidate(void);

/** Set the MQTT ticker scroll speed in on-screen pixels per 200 ms tick
 *  (clamped to 1–20). Driven by the Home Assistant "Ticker Speed" number
 *  entity. RAM-only — resets to the default (4) on reboot. */
void display_set_ticker_speed(int px);

/** Get the current ticker scroll speed (px per 200 ms tick). */
int  display_get_ticker_speed(void);

/** Wake the display task immediately, aborting its current sleep.
 *  Use after any RAM-only config change (e.g. config_set_mode) so the new
 *  state is rendered on the very next tick rather than after up to 200 ms.
 *  Thread-safe: safe to call from any task. */
void display_wake(void);

/** Force a full repaint of all six tubes on the next display-task tick.
 *  Use after changing display-config properties at runtime (e.g. col/row
 *  offsets, gamma, brightness) so the new settings are visible immediately
 *  rather than waiting for each tube's content to change naturally.
 *  Called automatically by display_apply_tube_offsets(). Calls display_wake().
 *  Thread-safe: safe to call from any task. */
void display_invalidate(void);

#ifdef __cplusplus
}
#endif
