# Changelog


## [1.17.8] - 2026-08-17

### Added
- Spectrum mode: optional beat-reactive nudge for the Breath and Rainbow LED accent modes — when LED source is set to "Follow accent mode," detected beats now give brightness/hue an extra pulse in time with the music. Off by default.

### Fixed
- Custom TTF font selection could silently revert to the default font (Logisoso) after toggling out of Spectrum mode or restarting — caused by a race condition in the web UI's font list loader.
- Dashboard Quick Actions buttons didn't correctly reflect the currently active mode (the Clock button was always shown as active regardless of the actual mode).
- Weather data (wttr.in) could fail to load because responses larger than 4 KB — common for detailed forecasts — were silently truncated and rejected.
- WeatherLive's Indoor Humidity/Temperature panel would repaint every frame without recovering after a sensor read failure, instead of falling back gracefully like other panel types.
- Rare crash/failure to start the microphone task during Spectrum mode under boot-time memory pressure.
- Rare instability related to microphone ADC hardware allocation under boot-time memory pressure.
- WiFi could associate with a much weaker access point than necessary on networks with multiple access points sharing the same SSID (mesh / whole-home WiFi), causing a slow or briefly unreachable web UI after roaming.
- Glyph drop-shadow color could render incorrectly on WeatherLive text redrawn from cache.
- Burn-in-protection and snow-overlay tubes could still be drawn over by other WeatherLive elements underneath.

### Security
- The web UI self-update mechanism now requires and verifies a SHA-256 checksum during its brief post-update authentication window, closing a gap where update content could previously be substituted unchecked.
- Release notes fetched from GitHub and shown in the web UI are now sanitized before rendering, closing a stored-XSS risk.
- `/api/network_info` and `/api/themes` now require authentication (previously reachable without logging in).

### Changed
- Internal cleanup pass across the codebase: removed dead code, deduplicated repeated logic, and fixed miscellaneous concurrency edge cases. No user-visible behavior change.

## [1.17.7] - 2026-08-09

### Added
- **First-boot setup wizard** — on a fresh flash or after a full factory reset, the web UI now opens with a mandatory setup wizard (no way to skip past it) offering two paths: **Restore from backup** (upload a previously-downloaded `config.json` to restore WiFi and every other setting in one step) or **Set up manually** (scan for your network or type the SSID, enter the password, connect — status is tracked live).

### Fixed
- **Color picker bars barely showing the selected color in Chrome** — Custom Face / DotMatrix color fields rendered as mostly plain background with only a thin sliver of actual color in Chrome, correct in Firefox. Now renders full in both.

## [1.17.6] - 2026-08-08

### Added
- **Independent Font/Digit shadow controls for Custom Face** — the drop shadow behind clock digits and the shadow behind font/label text were previously a single shared setting. They're now fully independent (on/off + color, day and night), so you can e.g. run a dark digit color with a matching dark digit shadow while keeping font/label text light with its own lighter (or disabled) shadow, without the two fighting each other. Grouped under clear "Font" / "Digit" headers in the UI.

### Changed
- **Setup AP instructions clarified** — new users were missing that the WiFi password for initial setup *is* the PIN scrolling on the tubes. That step now has its own prominent callout and clearer wording, and the Setup AP section itself moved to be the first topic under Web Management UI (matching the actual order a new user encounters it).

- README reorganized for easier navigation — deep hardware/audio/mic reference material moved later in the document, Table of Contents corrected and expanded.

- New confirmed-compatible replacement LCD panel added to the hardware guide (LY096X1608TBBIG09C08).

## [1.17.5] - 2026-07-18

### Added
- **Debug logging controls** — new collapsible section under System → Device Logs. Per-subsystem checkboxes toggle each subsystem between normal (INFO) and verbose (DEBUG) logging live, with "All on"/"All off" shortcuts. Makes it possible to capture verbose diagnostics from the web UI alone — no serial connection or firmware rebuild required.

### Fixed
- **Multi-second web UI delays** (GitHub issue #82) — root cause found and fixed: `/api/status` was recomputing LittleFS storage usage on every single poll (every 5 seconds), which could take ~2.7s on devices with a lot of stored files/themes. Usage is now cached and only recomputed on boot or after an actual file change (upload/delete/hotpatch), not on every poll. Eliminates the "browser stuck waiting to connect" symptom reported.

- **Network Info showing a garbled disconnect reason** (e.g. "Code -54") — WiFi disconnect reason codes can exceed 127 and were being stored in a signed 8-bit field, wrapping negative. Now stored correctly and shown as the proper mapped reason (e.g. "Auth fail (202)").

## [1.17.4] - 2026-07-16

### Added
- **Static IP configuration** — set IP/netmask/gateway/DNS under Network → WiFi Configuration, with client-side validation. Optional; DHCP remains the default.

- **Network Info panel** — collapsible diagnostics panel under WiFi Configuration: disconnect/reconnect log (count + reason), connection uptime, signal strength, MAC/BSSID/channel, and active netmask/gateway/DNS. Includes a manual refresh button.

- **Follow Sun/Moon LED mode** — the 6 RGB accent LEDs track the sun during the day and moon at night, with proximity-based brightness handoff between adjacent tubes.

### Changed
- Auto mode rotation interval now supports 5s/10s granularity (previously 15s minimum).

### Fixed
- Backup/restore config download no longer appears to hang — added visible busy state and a navigation guard during the download.

## [1.17.3] - 2026-07-12

### Added

- Home Assistant: autonomous update check — the clock now checks GitHub for new releases on its own (once on boot, then daily), independent of the web UI. Two new MQTT entities: `Nextube Update Available` and `Nextube Latest Version` (both under Diagnostics).

### Changed
- Refreshed visual design across the whole interface — cleaner cards, icons on the nav tabs, smoother transitions, and a batch of small alignment/contrast bugs fixed along the way.

 - LittleFS Recovery warning now correctly explains your config is backed up automatically and restored after the flash.

## [1.17.2] - 2026-07-09

### Added

- Night color set for Custom Clock Face and WeatherLive themes - allows you to define a second Font color / Digit color / Shadow set for nighttime. The clock crossfades between your day and night sets in step with the WeatherLive sky — tracking your **real geocoded sunrise/sunset** (the same signal that moves the sun and fades the stars in), not the Night Brightness hour window.

### Fixed
- Gated potential first boot issue that could ask user for admin password even when one is not set. This could happen if the network stack is to congested and does not return AUTH state to the browser in time. New warning message added.

## [1.17.1] - 2026-07-06

### Fixed
- Corrected new assetless themes not being in theme rotation (WeatherLive and DotMatrix)

## [1.17.0] - 2026-07-06

This update moves two hard asset themes (jpgs) to a single procedural theme. This makes adding new features much easier and takes a lot less space. 

### Added

- New procedural DotMatrix Theme

  - Two colors you control: "Dot color (on)" and "Dot color (off)" in the theme settings. The lit dots use your "on" color, and the background dots between them use your "off" color (dim, not black) — so it always looks like a real lit-up panel, not text floating on empty space.

  - Tube 5/6 dynamically scale characters to fit single tube and keep the "DotMatrix" visual styling

- New Custom color / gradient background when using Custom Clock Face

### Removed
- removed "DotMatrixRG" and "DotMatrixY" themes (saves ~612 KB of space)

- removed "Pomodoro" and "Countdown" theme assets (saves ~220 KB of space) 

- removed unused audio files (saves ~62 KB of space)

### Removed
The following were removed as they were impractiacal to actually use.

- Pomodoro

- Countdown

## [1.16.4] - 2026-07-05

### Fixed
- DotMatrixRG jpg error fix

## [1.16.3] - 2026-07-04

### Added
-Night LED Brightness

-Tube 5/6 Combined Temp/Humid panel

-Added font support for tube 5/6 in Asset based themes

### Fixed
-Folded per-tube brightness scaling into the gamma LUT — eliminates a per-pixel divide 

-ST7735S edge-line fix

-Fixed a deadlock risk where vTaskSuspend-from-outside

## [1.16.2] - 2026-06-30

### Added

- AQI (Air Quaility Index) for 24hr custom tube 5/6

  - US and EU AQI support

  - Health-band color coding on the AQI value

  - Home Assistant MQTT auto-discovery sensor

### Changed

- FreeType layout memo — caches univ_adj + baseline per face/size/fb, ~16 pt drop in display CPU

- Cloud falloff divide → fixed-point multiply

- Dirty-row per-span prev copy (skip full 25.6 KB memcpy on clean spans)

### Fixed
- A lot of small backend performance tweaks (mostly around WeatherLive)

- Fixed ZIP bounds check pointer-overflow 

- Additonal task error logging if something fails

- ST7735S edge rendering

## [1.16.1] - 2026-06-25

Small bug fix but an important one as it missed the final merge before v1.16.0 was released.

### Fixed
- 24 hours custom time, Tube5/6 Hi/Lo digit overlap when using TTF fonts.

## [1.16.0] - 2026-06-24

### Added

- **WeatherLive theme** — fully procedural animated clock (no image assets), HH:MM over a live sky

  - **Time-of-day sky** — gradient cycles night → dawn → day → dusk with warm twilight horizons

  - **Sun/moon arc** — travels left→right across all 6 tubes on a real geocoded sunrise/sunset, rising/falling in an arc (gap-aware of the physical bezels)

  - **True moon** — real phase shape (crescent→full) + geolocation-anchored position (absent near new moon, full moon up all night)

  - **Drifting clouds** — density/colour by condition (few → overcast/storm)

  - **Precipitation** — rain streaks and snow, condition-driven

  - **Wind** — fetched live; drives cloud drift speed, gusts, rain slant, snow blow

  - **Lightning** — random flashes on thunderstorms; optional **yellow-white LED flash** (LED accent "weather override" checkbox)

  - **Tube 6 panel** — rotates weekday + DD + localized month abbreviation ↔ current temp over today's forecast high/low range bar

- New Tube 5/6 options in 24 hour custom mode 

  - New Wind Speed Info Panel

  - New Humidity Info Panel

  - New Daily Hi/Lo Info Panel

  - Allow custom Images to be pushed [(readme)](https://github.com/MrToast99/Nextube-Remaster#pushed-images-on-tube-56-24h-custom)

- Weather mode panels

  - New Wind Speed

  - New Daily Hi/Lo

- TTF Font support (only in Custom and WeatherLive clock faces)

  - There are some caveats to Font shape (avoid overly wide/thick or super thin fonts)

  - Fonts should be subsetted (removing unused glyphs, this saves storage and PSRAM allocation) this documented on [Fonts](https://github.com/MrToast99/Nextube-Remaster#custom-ttf-fonts)

- LttleFS now shows file timestamps

### Changed
- Greatly reduced httpd cpu and flash demand

  - Additional error handling

  - Favicon added to website

- Improvements to online updater

### **Note that a mode change might be required for new fonts to update**

## [1.15.0] - 2026-06-15

### Added
- Online Updater - you can now one click update the clock on releases past this one. The option to manually flash OTA and WebUI are still there. Thanks to @bagl3y for the Idea and initial PR.

- Setting City in Weather services now has live validation

### Changed
- Compressed index.html to be served as gzip, this nearly 1/4s the data read from flash 71 KB vs 307 KB, cuts ESP CPU load. 

### Fixed
- Corrected bug in Weather services that caused locations with a space (Los Angeles) to be invalid

## [1.14.0] - 2026-06-12

### Added
- Spectrum mode bar colors can be set to match WLED if configured

- Option to play sound clip on MQTT ticker start

- MQTT logging options

### Changed
- Clarified NTP sync line

- Added system uptime in minutes to serial log heap msg (5 min interval)

### Fixed
- Tackled spectrum modes resource usage, bar distribution, and responsiveness (some polish needed)

- OTA flash guard, If spectrum mode is active when flash is trigger, forces to clock then flashes. otherwise clock would start crash/reboot cycle

- Permanent display-task hang after X uptime

- Clock-minute freeze (FlipClock + 24H_CX/NS)

## [1.13.7] - 2026-06-10

After initially uncleanly yanking out dedug lines *coughs v1.13.6..* here is a proper release

### Changed
- Weather/geocoding HTTP timeout 5 s → 10 s (cold-boot connects no longer fail-fast).

- Hardware RSA accelerator enabled (MBEDTLS_HARDWARE_MPI) — ~4–8× faster TLS handshakes.

- Fixed first-boot freeze on the AP-PIN setup screen (display task stack 12 → 16 KB).

- Tube 6 shows weekday + date immediately on cold boot — weather-dependent panels fall back to the weekdate panel until weather data arrives instead on a black tube.

- Sunrise/sunset animation runs 1.5× faster.

- AP-PIN marquee no longer repaints unchanged frames (4 of 5 ticks were redundant full-tube SPI pushes).

### Fixed
- **AUDIO !!!** 

  - Idle noise floor fixed — root-caused via: GPIO25's driven-LOW idle conducted the ESP32's ground/supply transients into the amplifier. The pad is now fully isolated at idle (rtc_gpio_isolate, the stock firmware's pad state). Constant static, activity hiss, and the 1 Hz tick are gone; boot pop eliminated.

** Note that a full power off (unplug) might be required after flash to allow residual charge to drain.

## [1.13.5] - 2026-06-08

### Changed
- Switched to PCF8563 slave discipline (mode 2, default) keeping the clock within ~1 ms/m of NTP between syncs and reports its max deviation at each sync (hourly). Due to the PCF8563 not being able to store or count sub seconds (ms) and the ESP's internal oscillator being all over the place, this is likely our accuracy limit.

## [1.13.4] - 2026-06-05

### Fixed
- Corrected Time freezing when tube 5 was used is 24 hour custom mode.

## [1.13.3] - 2026-06-05

### Added
- 24 hour (Custom) mode tube 5 and 6

  - Now tube 5 can be used to display an info panel, enabling removed the colon from the clock face. 

### Fixed

- Guard to prevent OTA triggering a double back to back flash

- Reduced time drift

## [1.13.2] - 2026-06-03

### Added

- External weather API

  - POST /api/weather to push your own readings (for users averaging multiple providers in home automation). Fields all optional: temp_c/temp_f (send one), humidity, condition, icon, WMO weather_code, lat/lon.

  - New "External" weather source (UI dropdown + push instructions); internal poller is gated off so it never overwrites pushed data.

  - Sunrise/Sunset works offline in this mode: pushed lat/lon persisted to flash (written only on change) and restored on boot.

  - Temperature stays canonical Celsius internally; temp_f converted on ingest; display °C/°F remains an independent UI setting.

### Fixed

### Audio / noise floor
- DAC now per-clip: brought up only while a sound plays (120 ms fade in/out), then fully torn down — GPIO25 driven LOW at idle in both enabled and disabled states. Eliminates continuous-DMA idle switching noise.

- WiFi power-save reverted WIFI_PS_NONE → WIFI_PS_MIN_MODEM

- GPIO25 boot clamp to OUTPUT-LOW

### Display / SPI
- Bulk LCD pixel pushes moved to DMA (spi_device_transmit, yields CPU) from busy-wait polling; small command/param writes stay on polling. Lowers SPI-induced noise during every redraw.

- Colon-blink partial push (diff-box) — per-second blink rewrites only the colon-dot rectangles, not the whole tube.

## [1.13.1] - 2026-05-31

### Added
- On LittleFS flashes, config is now copied to NVS and restored on reboot (Flash OTA first then LittleFS). This essentially removes most needs to full flash and reimport configs

### Changed

- moved Clock Language to Display tab

- Added translations for "In" and "Out" on tube 6

- Corrected NotionRain and GlitchGR themes using missing / old tiny assets

### Fixed

- Corrected time acceleration after HA Ticker scroll task finished

- Improved auto color selection for Tube6 info

## [1.13.0] - 2026-05-30

### Added

- Languages! The Webui and clock (tube6) now support Western European + Nordic languages, 11 in total. (Cyrillic and CJK will be reviewed. Note that text strings were all AI translated so if something is wrong or missing let me know.)

- AP recovery mode - hold the two outer touch pads (1 + 3) for 15 seconds → the setup AP comes up and the PIN shows on the tubes.

- MQTT Message Ticker task

### Fixed

- FINALLY fixed the root cause of mdns probe spam. Config saves caused a retrigger of SNTP's resolver to dereference the now-dangling pointers → read garbage from reused stack → fired garbage DNS queries....

- Corrected Wled parsing error, now displays primary color from wled controller (solid works, likely need more work for animation handling)

- Changed wifi from WIFI_PS_MIN_MODEM to WIFI_PS_NONE so it runs at a steady power, less ripples on the shared power causing audio static

## [1.12.5] - 2026-05-27

### Fixed
- Another fix for mdns spam

- Fixed Occasional WDT httpd crash

- Fixed Main socials disabled but sub socials enabled kept the mode in touch rotation 

- 'Save and Reboot' banner now persistent across tabs till acknowledged 

- 2nd fix for Weather not appearing in rotation when older configs restored

## [1.12.4] - 2026-05-26

### Changed

- More Webui optimization and polish

- MQTT now supports mode rotation and Theme selection 

- Album mode now has slide timer and shuffle

- Added force fetch to socials (useful when reducing interval time)

### Fixed

- Social media counters now show blanks instead of 0 before fetch

- Fixed inconsistent weather state when older config restored (If OTA upgrading to next release you might need to re-restore config)   

### Removed

- Removed stale tube 6 comments/placeholders

- Removed Scoreboard mode place holders (could not see anyway to accommodate this mode)

## [1.12.3] - 2026-05-25

### Changed

- Webui Dashboard is now dynamic based on "modes" actually enabled

- Webui optimization and polish

### Fixed

- 24hrs (Custom) Tube 6: date, outside temp, and indoor temp did not show update notification bar

## [1.12.2] - 2026-05-24

Changes this release

### Added

- Indoor sensor temperature offset option

- Additional Mode Rotation options

### Fixed

- Youtube Data API v3 fetch error

- Tube 6 not following set DDMM or MMDD

## [1.12.1] - 2026-05-24

Changes this release

### Added

- Dirty flash detection and WebUI banner - system will now warn user if a OTA flash failed and reverted to previous version

### Fixed

- Removed accidental inclusion of Theme specific Social media placeholder images (must full flash or use LittleFS recovery to clear)

- Removed old youtube/bili components

- Sync'd sunrise/sunset animations

- Config load did not fully load Panel 2 settings for weather (sunrise/sunset)

- corrected -2 pixel shift on some boots causing edge artifacts

## [1.12.0] - 2026-05-22

Changes this release

### Added

- wled sync support #44

### Changed

- Changed default subscriber poll to 1 hr

- Wifi password is now visible as plain text on first entry and changes

- Weather panel 2 (sunrise/senset) now animated

### Fixed

- Corrected "Custom clock" showing in MQTT updated naming to "Date"

- Fixed "Set Relay host warning" on fresh flash, switched youtube to disabled by default

## [1.11.0] - 2026-05-21

Changes this release

### Added

- Support for HomeAssistant via MQTT [Readme Section](https://github.com/MrToast99/Nextube-Remaster#home-assistant-mqtt) suggested in #41 

### Changed

- Istagram Support added to social_relay server

- Increased Default/fallback Social media icon size

## [1.10.0] - 2026-05-21

Changes this release:

Tiktok and Youtube both have heavy BOT gaurds in place preventing the clock itself from being able to query followers. The current solution is a relay server that runs anywhere in your network that can run more sofisticated queries and pass the data to the clock. The relay does not require API keys, but if you choose to setup API keys for Tiktok and/or Youtube then you do not need the relay server.   

### Added

- Instagram, TikTok, Youtube, Mastodon Follower Counters #34 

  - new display modes: (tube 0 icon + follower count). Configured under *Services › Social Media Counters*. Instagram and Mastodon use public profile APIs — no OAuth or API keys required.

  - TikTok and Youtube: Default to use new 'social_relay.py' server [Read the How to](https://github.com/MrToast99/Nextube-Remaster#local-relay-social_relaypy). Or use a Developer API Key, Fetches piggyback on the existing subscriber task and poll interval.

- Social Counter Decimal Display: K and M suffix counts now show up to 2 decimal places when the integer part is a single digit (e.g. `1.23M`, `4.5K`).

- Weather — Sunrise & Sunset Panel: New panel 2 in *Services › Weather › Display Panels*. Shows sunrise icon + local time on tubes 1 & 2, sunset icon + local time on tubes 5 & 6. Calculated locally via NOAA algorithm from the configured city — no extra API call. Defaults off; requires city to be configured.

### Fixed

- FIxed sunrise/sunset not working from all weather providers due to none parsed geocode.

### Changed

```

helpers/

+-- image_converter/    nextube_image_converter.py

+-- social_relay/       social_relay.py + requirements.txt

```

## [1.9.6] - 2026-05-18

Change this release

### Added
- Added 5th panel option to 24 hour (Custom) - Sunrise & Sunset

## [1.9.5] - 2026-05-18

Chnages this release

### Changed
- OTA update now suspends all tasks while updating

- new OTA progress UI bar

- Reorder task start up (RTC seed before Display) should solve any outstanding "stopwatch" display's

- corrected Advanced Display row/col offsets not updating right away and waiting for panel refresh

## [1.9.4] - 2026-05-18

Changes this release

### Added

- 24H Custom Mode Clock Face with custom tube 6

  - Weather icon

  - Weekdate

  - Indoor H/T

  - Outdoor H/T

### Fixed

- Corrected typo preventing F from saving and default back to C

### Changed

- new "waiting" image - Loads on initial power up and while OTA is triggered

- removed more unused assets (Weekdate) 

### Changed

- Added U8g2 graphics library 

- NTP re-sync logging - Offset and elapsed time now reported in milliseconds

- Enhanced boot log: shows exact RTC date and epoch value at seed time

## [1.9.3] - 2026-05-17

Changes this release

### Added

- NTP smooth time, smooths out hourly adjustment if clock is off sync

- Accent LED controls added under Spectrum mode

- MORE time zones!

- Set Date format 

### Fixed

- Night mode not honoring time zone

- More mitigation for first boot crashes (Deferred Mic enable till after AP Pin has been satisfied)

## [1.9.2] - 2026-05-16

Change this release

### Fixed
- Actually corrected boot issue when AP PIN is displayed on first boot.

## [1.9.1] - 2026-05-16

### DO NOT DIRECT FLASH, OTA + WEB from v1.7.5 work fine, but full flash is busted this build

Changes this release

### Added
- LittleFS Storage gauge!

### Fixed
- Boot issue when AP PIN is displayed on first boot.

## [1.9.0] - 2026-05-15

### DO NOT DIRECT FLASH, OTA + WEB from v1.7.5 work fine, but full flash is busted this build

Changes this release

### Added

### Spectrum Mode — LED Source Selector
- Web UI: LED Source dropdown in Spectrum Mode card; custom colour picker hides automatically when "Follow accent mode" is selected

### Theme Rotation
- Automatically cycles through installed themes on a configurable timer (1 min → 4 hrs, default 5 min)

- Per-theme selection: checkbox list with **All** / **None** shortcuts — leave all ticked to rotate every installed theme, or pick a subset

- Custom/user-uploaded themes discovered automatically from `/images/themes/` at each rotation event — no restart required

- Rotation order is alphabetical; any manual theme save resets the timer

- Active theme held in RAM during rotation (no flash wear); boot theme is whatever was last explicitly saved

### Weather API Rate Limiting
- Fixed-window rate limiter added to the weather component to stay comfortably within Open-Meteo's free-tier limits (600/min, 5,000/hr, 10,000/day)

- Running at 90% of each cap (540/min, 4,500/hr, 9,000/day) to leave headroom for transient bursts

- If a window is exhausted the fetch sleeps until it resets rather than dropping the request

### Security

**HTTP Body Length Handling (`web_server.c`)**

- Addressed PR #18: the claimed integer overflow was not present (signed `int` bounded to [1, 4096] before `malloc`), but the pattern was tidied anyway — validation stays on the signed `content_len`, then widened to `size_t` for all subsequent arithmetic, receive loop counter, and pointer arithmetic

- `api_themes()` insertion sort: all `strncpy` calls capped to `THEME_NAME_MAX - 1` with explicit null-termination replaced by `strlcpy`

## [1.8.0] - 2026-05-14

Many of you likely have worn stressed LCD panels. Fret not I have tested out and adjusted the Firmware to accommodate a modern replacement (LH096NT-IF09W).  

### Added

- Per-tube gamma correction

- Per-tube VCOM control 

- Per-tube panel profile 

- Replacement LCD documentation — README: new Replacement LCD Panels subsection confirming LH096NT-IF09W (ST7735S, 0.96″) as a verified drop-in, with an 8-step calibration guide. New Advanced Display (LCD Calibration) section documents all per-tube controls with reference tables.

### Changed

- NTP time re-resolves DNS every 24hrs to ensure IP is not stale

## [1.7.5] - 2026-05-12

Changes this release:

### Fixed
- Restored Touch and Spectrum mode

- Hopefully fixed Hostname flapping and mdns garbage

## [1.7.3] - 2026-05-10

Changes this release:

### Added
- Webui Admin auth changed to optional, Enable via system tab.

## [1.7.2] - 2026-05-09

Changes this release:

### Changed
- First time Admin password set and logins now faster

### Fixed
- DHCP requests should now adhere to set hostname

## [1.7.0] - 2026-05-09

Changes this release:

### Changed

- AP Pin now slow scrolls acoss the LCD's instead of the previous split view

### Fixed

- mDNS fix, reordered so hostname is set before network broadcast and not default "espressif" causing DNS garbage.

## [1.6.0] - 2026-05-08

While I aim for a smooth transition with this release, the growing prevalence of cyber threats targeting IoT devices necessitates stronger built-in security. To proactively protect your equipment. I have implemented mandatory setup pairing codes and required Admin authentication for all WebUI access.

### Security

- Setup AP is now WPA2 with a per-device 8-digit PIN (NVS-backed, survives reflash); PIN displays on the LCD tubes while AP is active; AP only starts on first boot and as a 90-second fallback if STA fails to connect and get IP

- URL decoder rejects %00 and ASCII control characters

- CORS wildcard (Access-Control-Allow-Origin: *) removed from all responses

- Admin password system: first-boot set-password flow + login modal in web UI

- 2 MB hard cap on file uploads (returns 413)

### Performance & stability

- Heap telemetry task logging internal vs PSRAM free/largest-block every 5 minutes 

- DAC log rate limiting, httpd config tuning

### Bugs

- Corrected mDNS being not set at boot causing mDNS annouce garage to loop back and flood DNS logs

- Corrected incorrect Heap math (caused by only 4MB of RAM being addressable of 8MB on board) ** This is a Hardware limitation, not software

### Added

- New Task toggles — weather, youtube, mdns now let users disable unwanted background tasks

- RTC seed timing fix — TZ apply + RTC clock seed now happens before the WiFi-wait in ntp_task; display shows correct saved time from first boot tick instead of 1970

## [1.5.0] - 2026-05-07

Changes this release:

### Firmware

- Corrected mDNS bug where mDNS started before IP and Hostname were set causing mDNS multicast spam

## [1.4.0] - 2026-05-06

Changes this release:

### Display

- Scheduled Anti Burn-in / LCD Refresh — new Display Settings card; fires the colour-cycle recovery session automatically at a user-configured day, hour, and duration

  - Schedule: weekly (every Sunday) or monthly (1st of month)

  - Hour picker: 0–23 (12-hour AM/PM labels, midnight/noon annotated)

  - Duration: 30 min / 1 hr / 2 hr / 3 hr / 4 hr

  - Per-tube selection with All / None shortcuts

  - Persisted to config as burnin_auto_enabled, burnin_auto_mask, burnin_auto_duration_s, burnin_auto_interval, burnin_auto_hour

  - Display task checks at the configured hour using a day-of-year guard to prevent re-trigger within the same window

### System Tab — Reorganised

- "⚡ Hot Patch (Web UI Delta)" → "Web UI Update"; button renamed "Upload & Apply"; status gap between file picker and button fixed

- "Web UI Update (LittleFS)" → "LittleFS Recovery"; moved below LittleFS Files; warning copy updated to reference Backup & Restore

- Hardware Settings card removed — its only field moved to the Audio tab

- Version-mismatch banner updated to reference System → LittleFS Recovery

### Audio Tab
- Enable Microphone moved from System → Hardware Settings to Audio, above Enable Audio Output

## [1.3.0] - 2026-05-05

Changes this release:

### Firmware
- Enabled on tube indicator for available updates (red line appears on the top of Tube 6) this can be disabled in the WebUI

### WebUI
- Added controls for on tube 6 update indicator

- Increase OTA timeout 

 

**NOTE you can install the firmware and then simply upload a new web/index.html over the current (till I can figure out why the hotpatch.zip is not creating).

## [1.2.0] - 2026-05-05

Changes this Release:

Since most updates are either Firmware or the Webui itself there is no reason to constantly ask folks to reload the LittleFS partition for 1 or 2 files so.... I hereby grant HOT PATCHS! keep an eye out for them with future releases. 

The OTA (Firmware) still reboots, but now when only the Webui is updated and a release pushed there will be a new "hotpatch" zip that you can live load into the Webui. 

 

### Firmware (api_fs_hotpatch)

- Receives the ZIP raw into PSRAM in one shot

- Walks local file headers sequentially — no external ZIP library needed since -0 means STORE (raw bytes, no decompression)

- Creates missing parent directories, then fwrites each entry straight to /spiffs/

- config.json blocked server-side even if somehow included

- Filesystem stays mounted throughout — no reboot, changes live immediately

### Changed

- New ⚡ Hot Patch card above the full LittleFS OTA card

- Shows which files were patched in the response (web/index.html, etc.)

- Full LittleFS OTA stays untouched as fallback for clean installs or recovery

## [1.1.4] - 2026-05-05

Changes this Release:

### Changed

- Automatic firmware update check on page load and every 24 hours (in addition to existing manual check)

- Update notification toast (bottom-right) with See what's new ↗ (GitHub release link), Dismiss once (suppresses for 24 h), and Dismiss version (permanent per-version dismiss)

- Added Noise Floor threshold slider to Display → Spectrum Mode — previously only accessible in the debug panel

- Aligned LED Glow Colour and LCD Bar Colour pickers using CSS grid for consistent layout

## [1.1.3] - 2026-05-04

Changes This Release:

### Security
- GET /api/settings no longer returns the WiFi password; replaced with a has_password boolean

- Added GET /api/backup endpoint that returns the full config including credentials (for explicit backup use only)

- POST /api/settings — password field is optional; omitting it preserves the stored password

### Fixed
- Config backup/restore: export now uses /api/backup so WiFi credentials survive a full restore without losing the password

- WiFi reconnect on restore: password-only changes no longer trigger a full disconnect/reconnect; only SSID changes do. Eliminates ~60 s of connectivity loss on restore.

- Audio idle noise: DAC continuous driver now stays live at level 0 when audio is disabled. Keeps the amp's AC-coupled input terminated at low impedance (~50 Ω), preventing WS2812/SPI rail switching noise from coupling in. Hi-Z and GPIO LOW both produced audible hiss.

- app_main stack overflow fix: mic calibration moved out of a large local array context

### Performance / Reliability
- Display: 8-row SPI batching (160 → 20 transactions per tube digit)

- Spectrum renderer: PSRAM framebuffer replaced with row-driven SRAM line buffer

- Mic task pinned to core 0 to prevent starvation by the display task

- Thread safety: all config reads across tasks (NTP, weather, YouTube/Bili, LEDs, mic, touch) now copy fields under the config mutex before use

### Changed
- fread/fwrite return values checked in config save/load

- Out-of-range JSON values clamped (volume, brightness) at parse time

- OTA upload rejects requests with missing Content-Length before calling esp_ota_begin()

- WiFi scan fixed for zero-AP result (prevented potential NULL dereference)

- File upload rejects requests that would exceed available partition space

## [1.1.2] - 2026-05-02

Changes in this release include:

### Microphone / Spectrum
•	24-band expansion —24 (4 per tube), log-spaced 280–3800 Hz. 

•	Selectable LCD bar color 

•	Ability to disable MIcrophone if desired

### Display / Themes
•	Theme JPEG error detection With Theme error banner in web UI 

### Web UI / File Browser
•	Folder renaming 

•	Spectrum Mode card — description updated to 24 bands/4 per tube; with color picker 

### Image Converter (helpers/nextube_image_converter.py)
•	Output filenames preserved — output files no longer appended with _{width}x{height}; original stem kept on both crop and stretch paths

## [1.1.1] - 2026-04-26

This release add support for:

- Deletion of folders

- Disable Microphone (Pending full implementation still)

## [1.1.0] - 2026-04-26

This release contains a big move from a SPIFFS partiton to LittleFS. To read a comarison [SPIFFS vs LittleFS](https://www.techrm.com/file-management-on-esp32-spiffs-and-littlefs-compared/).

## [1.0.10] - 2026-04-11

### New theme upload workflow is now

1. Browse to /images/themes/ → New Folder → MyTheme

2. New Folder → Numbers → upload 0.jpg…9.jpg

3. Back to MyTheme/ → New Folder → AMPM → upload blank.jpg, colon.jpg, am.jpg, pm.jpg

4. Theme appears in the dropdown immediately

The Upload Folder button is still there for bulk uploads.

## [1.0.9] - 2026-04-07

### Clock Display
- Leading zero toggle — new "Show leading zero" option in the Display tab (e.g. 09:30 vs 9:30), applies to 12H, 24H, and 24H_NS modes

- Blinking colon — colon now blinks every second on all themes except FlipClock (which retains its flip animation with solid colon)

- Bug fix — leading zero setting now takes effect immediately (within 200 ms) after saving; previously would not update on 24H_NS + FlipClock until the minute changed

### Network / Time
- Multiple NTP servers — up to 4 NTP server slots configurable in the web UI (slots 3 & 4 optional)

- Proper DST support — replaced manual UTC-offset with POSIX timezone strings (e.g. EST5EDT,M3.2.0,M11.1.0); DST transitions are now handled automatically by the system

- Timezone dropdown — ~50 named timezones grouped by region with an editable POSIX string field for custom entries

- Legacy migration — existing configs using the old numeric time_zone offset are automatically migrated on first boot

### Audio / DAC
- Expanded documentation on DAC signal chain, boot fade, APLL cold-start, LTK8002D hardware limitations, and WS2812B noise mitigations

## [1.0.8] - 2026-04-02

This Release rolls in PR #8 Thanks to @bagl3y and ability to disable the DAC entirelyfrom the Audio screen if desired. 

### 1. Scheduled Night Mode
- The display brightness can now automatically dim during night hours to prevent glare in dark rooms.

	- Primary Brightness: Your standard day-time or default level.

	- Night Mode Toggle: Enable/disable automatic switching.

	- Customizable Window: Set specific start and end hours (e.g., 22:00 to 07:00).

	- Smart Logic: Handles time ranges that wrap around midnight. If NTP is not yet synced, it safely defaults to the primary brightness level.

### 2. Settings Import/Export (Backup & Restore)
- Updating the SPIFFS partition normally wipes the config.json file. To solve this, I've added a management section in the System tab:

	- Export: Download your entire configuration (WiFi, Themes, Weather, Night Mode, etc.) as a nextube-config.json file to your PC.

	- Import: Restore your settings instantly from a backup file. The device applies the settings and reloads the interface automatically.

### UI Improvements
- Simplified the Display tab by grouping brightness controls.

- Improved the System tab with a dedicated "Backup & Restore" card.

- Cleaned up the settings API to ensure the display task is the sole owner of hardware brightness, preventing flickering during saves.

## [1.0.7] - 2026-03-30

Pulled in PR #7 thanks to @bagl3y !

### Key Changes
1.Lowered PWM Frequency (1000 Hz):

-  The original firmware uses a PWM frequency of 1 kHz (0x03E8 found in the binary), whereas this remaster was using 20 kHz.

- A lower frequency ensures more efficient switching of the backlight transistor, resulting in higher effective luminance.

2.Added ST7735 Power & Gamma Initialization:

- The previous initialization was too minimal for IPS panels.

- Added standard Power Control (PWCTR1-5) and VCOM Control (VMCTR1) sequences to properly drive the panel's internal voltages.

- Included Gamma Correction (GMCTRP1/N1) curves to prevent a "washed-out" look and ensure vibrant colors and deep blacks.

### Results
- Significantly higher maximum brightness.

- Improved contrast and color accuracy.

- Visual parity with the stock firmware's display quality.

### Other changes
- correct firmware/spiffs mismatch banner

- added save warning on Webui

## [1.0.6] - 2026-03-28

### Fixed

- **SHT30 sensor crash on mutex allocation failure** 

- **SHT30 torn read fixed** 

- **YouTube/Bilibili subscriber count data race fixed** 

- **JSON parsing length bound enforced** 

- **OTA handle leak on malloc failure** 

- **Audio DAC race on task timeout** 

- **Audio volume quantization**

- **Stale album image list after file upload/delete** 

- **Weather fetch infinite tight retry** 

### Changed

- **LRU image decode cache**

- **Cache flushed on theme change** 

- **Pre-allocated JPEG decode buffers** 

- **FlipClock animation uses pre-allocated buffer** 

- **`flip_prime_blank()` uses image cache** 

- **Blending system removed** 

- **YouTube/Bilibili poll interval** — increased from 5 minutes to 30 minutes.

- **Web server file transfer buffer** 

- **LWIP socket limit**

## [1.0.5] - 2026-03-25

### Fixed
- LED off inadvertently caused LCD's to turn off

- Partial revert of Wifi NVS code (prevents wifi loss during OTA updates)

### Changed
- New 24hr (no secs) clock (6th tube can be blank or Current weather icon)

## [1.0.4] - 2026-03-25

### Fixed

- OTA rollback —  firmware no longer silently reverts to previous version on first reboot after OTA flash

- WiFi first-connect — fixing the requirement to power cycle after first credential save

- Settings save dropping connection — display/theme saves no longer kill the web server

- Factory reset incomplete 

- RTC saving bug

- AP boot timeout — Setup AP now closes after 3 minutes if credentials are saved but STA never connects, preventing Nextube-Setup broadcasting indefinitely on deployed devices

### Added

SHT30 local sensor — Optional temperature/humidity sensor support; auto-detected on the shared I2C bus at boot; background polling every 30 s; exposed via /api/status and displayed in the web UI sensor tile

## [1.0.3] - 2026-03-22

- Added missing artwork, Some are still place holders (but its better then blanks)

- Split Weather panel into 2 screens with controls in the webui

- Added Update Checker

- Allow for custom theme packs (see notes on main Readme.md for structure), press refesh on Themes to scan uploaded files. 

- New "Update_type.txt" to package to say if a full or partial flash is needed.

## [1.0.2] - 2026-03-18

### Weather

- Met.no set as default weather source

- Temperature rounded to whole degrees in both tube display and log output

- °C/°F tube now OR-blends blank.jpg + degreec/degreef.jpg so the degree symbol renders correctly instead of looking like "0C"

- Waiting indicator changed from minus.jpg to dot.jpg (AMPM folder) on all 6 tubes while waiting for first fetch

- Removed 10-second startup delay — first fetch happens immediately on WiFi connect, retries every 5 seconds until successful, then resumes normal 10-minute polling

### Timers

- Middle touch button now pauses/resumes the timer in Countdown and Pomodoro modes (all other modes retain backlight toggle)

- Pomodoro work and break durations now configurable via Web UI (Services tab) and persisted in config.json

### Web UI / SPIFFS Browser

- Timer Settings card added to Services tab (countdown duration, Pomodoro work/break durations)

- Upload File(s) now supports multi-file selection

- New Upload Folder button — selects an entire local directory and recreates the full folder structure on SPIFFS

## [1.0.1] - 2026-03-17

### What's Fixed

**Web UI File Browser**

The SPIFFS file browser in the web interface now loads correctly. Previously, browsing audio files or images would silently fail and return an empty list. It now reliably lists all files and folders.

### What's Working Better and Nealy Fixed

**Audio Pops & Static**

The audible pop/static click that occurred at the start and end of every sound effect (button clicks, timer bells, alerts). The DAC output now ramps smoothly in and out instead of switching abruptly, making all audio transitions cleaner and but not yet silent.

**Audio Stutter**

Stuttering during longer sounds (e.g. timer.wav). The audio buffer is now correctly sized and drained, so brief storage slowdowns during playback no longer cause gaps or cut-offs in the audio but more work needed. 

---

### Under the Hood

- Hardened the settings save/load against malformed data that could previously crash the device

- Removed unused and duplicate internal code across the display and configuration modules

- Web UI status endpoint now correctly reflects the active display mode in all cases

## [1.0.0] - 2026-03-16

Initial public release, don't mind a few bugs

