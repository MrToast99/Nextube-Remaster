# Nextube-Remaster Open-Source Firmware

[![Build](https://github.com/MrToast99/Nextube-Remaster/actions/workflows/build.yml/badge.svg)](https://github.com/MrToast99/Nextube-Remaster/actions/workflows/build.yml) ![GitHub Downloads (all assets, latest release)](https://img.shields.io/github/downloads/MrToast99/Nextube-Remaster/latest/total)
 ![GitHub Downloads (all assets, all releases)](https://img.shields.io/github/downloads/MrToast99/Nextube-Remaster/total?label=downloads%40total)



**Unofficial** open-source replacement firmware for the [Rotrics Nextube](https://www.rotrics.com/) split-flap–style digital clock, reverse-engineered from a full flash dump of the original ESP32 firmware.

Like the work? Help keep me Caffeinated! <br>
[!["Buy Me A Coffee"](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/mrtoast99)

---

## Contents

- [What is this?](#what-is-this)
- [Features](#features)
- [Building](#building)
  - [Prerequisites](#prerequisites)
  - [Local Build](#local-build)
  - [Versioning](#versioning)
  - [CI Build](#ci-build)
- [Flashing](#flashing)
  - [Option A — Browser / ESPConnect](#option-a--browser-based-no-tools-required)
  - [Option B — esptool full flash](#option-b--first-time--full-flash-esptool-cli)
  - [Option C — Individual partitions](#option-c--individual-partitions-esptool-cli)
  - [Over-the-Air (OTA)](#over-the-air-ota)
    - [Online Updater (one click)](#online-updater-recommended--one-click-no-downloads)
    - [Automatic Update Checks](#automatic-update-checks)
    - [Returning to factory firmware](#returning-to-the-original-factory-firmware)
- [Web Management UI](#web-management-ui)
  - [Setup AP (WiFi Provisioning and First Setup)](#setup-ap-wifi-provisioning-and-first-connection)
  - [Admin Authentication](#admin-authentication-optional)
  - [Web UI Language](#web-ui-language)
  - [Static IP & Network Diagnostics](#static-ip--network-diagnostics)
  - [Advanced Display (LCD Calibration)](#advanced-display-lcd-calibration)
- [Modes](#modes)
  - [Mode Rotation](#mode-rotation)
  - [Theme Rotation](#theme-rotation)
  - [Touch Buttons](#touch-buttons)
- [Weather](#weather)
  - [City format](#city-format)
  - [Live city verification](#live-city-verification)
  - [External weather (push your own data)](#external-weather-push-your-own-data)
- [Pushed images on tube 5/6](#pushed-images-on-tube-56-24h-custom)
- [Air Quality panel](#air-quality-panel)
- [NTP & Clock Accuracy](#ntp--clock-accuracy)
  - [How synchronisation works](#how-synchronisation-works)
  - [Time discipline modes](#time-discipline-modes)
  - [NTP sync log](#ntp-sync-log)
- [Social Media Counters](#social-media-counters)
  - [Optional API Keys](#optional-api-keys)
  - [Local Relay (`social_relay.py`)](#local-relay-social_relaypy)
- [Home Assistant MQTT](#home-assistant-mqtt)
  - [What gets exposed](#what-gets-exposed)
  - [Setup](#setup)
  - [MQTT topics](#mqtt-topics)
  - [Notes](#notes)
  - [Ticker](#ticker)
- [Follow Sun/Moon LED Mode](#follow-sunmoon-led-mode)
- [WLED Sync](#wled-sync)
  - [Setup (Nextube side)](#setup-nextube-side)
  - [Setup (WLED side)](#setup-wled-side)
  - [Notes](#notes-2)
- [Themes](#themes)
  - [Built-in Themes](#built-in-themes)
  - [WeatherLive theme](#weatherlive-theme)
  - [DotMatrix theme](#dotmatrix-theme)
  - [Custom Face](#custom-face)
  - [Adding a Custom Theme](#adding-a-custom-theme)
  - [Image Converter Helper](#image-converter-helper)
- [Custom TTF Fonts](#custom-ttf-fonts)
  - [How it works](#how-it-works)
  - [Shadow](#shadow)
  - [Uploading a font](#uploading-a-font)
  - [Reducing font file size](#reducing-font-file-size-with-pyftsubset)
- [REST API](#rest-api)
- [Hardware](#hardware)
  - [Audio / DAC Notes](#audio--dac-notes)
  - [Microphone Notes](#microphone-notes)
  - [SHT30 Temperature / Humidity Sensor](#sht30-temperature--humidity-sensor)
  - [Replacement LCD Panels](#replacement-lcd-panels)
  - [Flash Layout](#flash-layout-16mb)
- [Project Structure](#project-structure)
- [License](#license)
- [Community](#community)
- [Acknowledgements](#acknowledgements)

---

## What is this?

The Nextube is a desktop clock with six small IPS LCD displays that simulate a split-flap/Nixie-tube aesthetic. The original firmware relies on Rotrics' cloud servers and a mobile app, both of which are increasingly unreliable. This project replaces it with fully self-contained firmware featuring a built-in web management UI — no apps, no cloud, no accounts.

## Features

| Feature | Status |
|---|---|
| 6× ST7735 LCD display driver | ✅ Working |
| WS2812 RGB LED accent lighting (static/breath/rainbow) | ✅ Working |
| Capacitive touch pads (3 buttons) | ✅ Working |
| PCF8563 RTC (battery-backed) | ✅ Working |
| WiFi AP+STA (WPA2-secured setup network with per-device PIN) | ✅ Working |
| NTP time sync | ✅ Working |
| PCF8563 RTC-disciplined clock (PCF slave mode — ~1 ms between NTP syncs, default) | ✅ Working |
| Built-in web management UI (SPA) | ✅ Working |
| REST API (backward-compatible + extensions) | ✅ Working |
| mDNS (`http://nextube-remaster.local`) | ✅ Working |
| OTA firmware updates via web UI | ✅ Working |
| OTA web UI / LittleFS updates via web UI | ✅ Working |
| One-click Online Updater | ✅ Working |
| Firmware + LittleFS version mismatch detection | ✅ Working |
| Weather display (temp, humidity, condition icon) | ✅ Working |
| wttr.in weather (free, no key) | ✅ Working |
| Open-Meteo weather (free, no key) | ✅ Working |
| OpenWeatherMap weather (free-tier API key) | ✅ Working |
| Met.no weather (free, no key, elevation-aware) | ✅ Working |
| External weather (push your own data via `POST /api/weather`) | ✅ Working |
| Air quality panel (US / European AQI, free keyless Open-Meteo) | ✅ Working |
| Live weather city verification in web UI | ✅ Working |
| YouTube subscriber counter (direct + relay) | ✅ Working |
| Bilibili follower counter (direct) | ✅ Working |
| Instagram follower counter (direct unofficial API) | ✅ Working |
| TikTok follower counter (via local relay) | ✅ Working |
| Mastodon follower counter (direct API) | ✅ Working |
| Local social counter relay (`social_relay.py`) | ✅ Working |
| DAC audio playback (LTK8002D amp, WAV files) | ✅ Working |
| Clock themes (Nixie/Digital/Flip art) | ✅ Working |
| WeatherLive procedural theme (animated sky, sun/moon phase, stars, live weather) | ✅ Working |
| Pushed images on 24H Custom tube 5/6 (`POST /api/cx_image`) | ✅ Working |
| Album/slideshow mode (sliding window — each tube shows a different image) | ✅ Working |
| Date mode (date display, DD/MM/YY) | ✅ Working |
| Spectrum mode (microphone audio visualiser — 24 Goertzel bands, 4 per tube → LED + LCD) | ✅ Working |
| Per-mode enable/disable toggles | ✅ Working |
| Auto mode rotation with configurable interval | ✅ Working |
| Auto theme rotation with configurable interval and per-theme selection | ✅ Working |
| LittleFS file browser with upload/delete/mkdir/rename | ✅ Working |
| Automatic firmware update check (compares against latest GitHub release) | ✅ Working |
| Home Assistant MQTT integration (sensor + mode + display + brightness) | ✅ Working |
| WLED Sync — receive UDP Notifier broadcasts; accent LEDs follow WLED colour | ✅ Working |
| Weather Panel 3 — animated sunrise/sunset (20 Hz rising/setting sun + mountains) | ✅ Working |
| Multilingual web UI — 11 languages (EN/DE/FR/ES/IT/PT/NL/SV/NO/DA/FI) with per-browser preference | ✅ Working |
| Tube display localisation — day-of-week abbreviation in 11 languages on clock/date panels | ✅ Working |
| Custom TrueType fonts for clock digits and panel text (stb_truetype, PSRAM-cached glyph renderer, configurable drop shadow) | ✅ Working |
| Follow Sun/Moon LED mode — accent LEDs track the sun by day, moon by night | ✅ Working |
| Static IP configuration (optional, DHCP remains default) | ✅ Working |
| Network Info panel — disconnect/reconnect log, signal strength, link details | ✅ Working |
| Debug logging controls — per-subsystem verbosity, no serial connection required | ✅ Working |

## Building

### Prerequisites

- [ESP-IDF v5.5.x](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/get-started/) installed
- Or just push to GitHub — the CI workflow builds automatically

### Local Build

```bash
# Activate ESP-IDF environment
source ~/esp/esp-idf/export.sh

# Build
idf.py build

# Flash via USB (adjust port)
idf.py -p /dev/ttyUSB0 flash monitor
```

### Versioning

Firmware and LittleFS/Web UI versions are tracked independently in `version.json`:

```json
{
  "firmware_version": "1.0.10",
  "fs_version":       "1.0.9",
  "name": "Nextube-Remaster",
  "description": "..."
}
```

| Key | Controls | When to bump |
|---|---|---|
| `firmware_version` | `nextube-fw-*-ota.bin` / `nextube-fw-*-full.bin` | Any change to `main/`, `components/`, `CMakeLists.txt`, etc. |
| `fs_version` | `nextube-littlefs-*.bin` | Any change to `data/` (web UI, theme images, audio) |

CMake reads `firmware_version` and injects it as `FW_VERSION_STR` into all components via `target_compile_definitions(PUBLIC)` in `components/config_mgr/CMakeLists.txt`. `fs_version` is written to `data/web/version.txt` and bundled into the LittleFS image — the web UI reads it at runtime to detect mismatches. Bumping only `fs_version` never triggers the mismatch banner. Bumping only `firmware_version` will show the banner until the matching `littlefs.bin` is flashed.

To release a new version, update `version.json` and tag the commit.

### CI Build

Every push to `main` triggers a GitHub Actions build. Tagged releases (`v*`) automatically create a GitHub Release with downloadable firmware binaries and generated release notes that identify which partitions changed (firmware-only OTA, LittleFS-only, or full re-flash) by comparing changed files against the previous tag.

## Flashing

### Option A — Browser-based (no tools required)

**[ESPConnect](https://thelastoutpostworkshop.github.io/ESPConnect/)** is the easiest way to flash — no Python, no drivers, no CLI. It runs entirely in the browser using the Web Serial API (Chrome / Edge only).

1. Open **https://thelastoutpostworkshop.github.io/ESPConnect/** in Chrome or Edge
2. Connect the Nextube via USB
3. Click **Connect** and select the device's COM port
4. Set baud rate to **460800**
5. Flash `nextube-fw-full.bin` at offset `0x0`
6. Once flashing is complete, click **Disconnect** in ESPConnect to release the serial port
7. **Unplug the USB cable, wait 3 seconds, then plug it back in** (or use the power switch if you have one)

> [!IMPORTANT]
> **Steps 6 and 7 are required.** The Nextube cannot start its WiFi setup network while ESPConnect is still connected to the serial port. After power-cycling you will see the display light up and the `Nextube-Setup` WiFi network will appear within about 10 seconds. If the network does not appear, unplug and replug the USB cable once more.

> **Note:** Web Serial requires Chrome or Edge. Firefox is not supported. Use USB-A to USB-C, C-C cables don't seem to work.

> **macOS:** this method works fine on macOS, but only in **Chrome** — Safari does not support Web Serial. You may need to install the [CP210x / CH34x USB-serial driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers) for the port to show up.

### Option B — First-time / Full Flash (esptool CLI)

Use the merged binary for a clean install — this writes all partitions in one shot:

```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
  write_flash 0x0 nextube-fw-full.bin
```

> **Windows:** replace `/dev/ttyUSB0` with your COM port (e.g. `COM3`).

After the flash tool resets the device, the firmware performs one automatic restart to ensure the WiFi hardware initialises cleanly — this is normal and takes about half a second.

### Option C — Individual Partitions (esptool CLI)

```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
  --flash_mode dio --flash_freq 40m --flash_size 16MB \
  write_flash \
  0x1000   bootloader.bin \
  0x8000   partition-table.bin \
  0x10000  nextube-fw-ota.bin \
  0x910000 littlefs.bin
```

### Over-the-Air (OTA)

#### Online Updater (recommended — one click, no downloads)

When the update check finds a newer GitHub release (see [Automatic Update Checks](#automatic-update-checks)), the toast includes an **Online Updater** button. Click it to install everything in one step — no manual downloads, no file picking. **The device pulls the release directly from GitHub itself:**

1. **Review** — a panel shows the release notes and confirms both assets (firmware + web UI) are present, then you click **Proceed**.
2. **Firmware** — the device downloads the new firmware from GitHub over HTTPS, **verifies it by SHA-256**, and flashes itself, with live download and flash progress.
3. **Reboot** — into the new firmware.
4. **Web UI** — the device then pulls and applies the matching web UI on its own; the browser tracks progress and finishes with **✓ Update finished** and a **Reload page** button.


#### Manual Updating

The web UI provides three separate update paths under the **System** tab (use these for sideloading a build, or when the device can't reach GitHub):

| System card | File | When to use |
|---|---|---|
| **Firmware Update (OTA)** | `nextube-fw-v{ver}-ota.bin` | New firmware, bug fixes |
| **Web UI Update** | `nextube-WebUI-v{ver}.zip` | New web interface — config-safe delta patch, no reboot |
| **LittleFS Recovery** | `nextube-littlefs-v{ver}.bin` | Full filesystem reflash — only when release notes require it, or to recover a corrupted filesystem |

**Step by step — updating firmware:**

1. Download `nextube-fw-v{ver}-ota.bin` from the [GitHub release](https://github.com/MrToast99/Nextube-Remaster/releases) assets.
2. Open **System → Firmware Update (OTA)**, choose the file, and click **Upload & Flash**.
3. A progress bar tracks the upload and flash; the tubes show a wait screen. When flashing completes the device **reboots itself** into the new firmware and the page reconnects automatically.

**Step by step — updating the web UI:**

1. Download `nextube-WebUI-v{ver}.zip` from the same release.
2. Open **System → Web UI Update**, choose the ZIP, and click **Upload & Apply**.
3. Changed files are written in place. Your config, custom themes, album images, and audio clips are **never touched**, and **no reboot is needed** — reload the browser page and the new UI is live.

**Updating both (a normal release):** flash the firmware first, let the device reboot, then apply the Web UI ZIP. If you do firmware only, the version-mismatch banner (below) will remind you the web UI is stale.

**LittleFS Recovery** (`nextube-littlefs-v{ver}.bin` via **System → LittleFS Recovery**) is the heavyweight alternative to the ZIP: it erases and rewrites the **entire** filesystem partition. Your settings survive automatically (saved to NVS before the erase, restored on next boot), but custom themes, album images, and audio files are wiped — back those up first via **System → LittleFS Files**. Use it only when the release notes explicitly call for a full LittleFS reflash.

> **Do not** upload `nextube-fw-full.bin` via OTA — it is the merged USB-flash image, not a valid OTA app image.

> **Spectrum mode:** if the device is in Spectrum visualiser mode when an OTA starts, the firmware automatically switches to Clock mode and waits briefly for the microphone to release the I²S peripheral before proceeding — this is expected and takes less than a second.

> **Admin password:** if an admin password is set, uploads require a signed-in session; an expired session is caught before the upload starts and re-prompts for sign-in.

#### Version mismatch detection

After a firmware-only OTA, the web UI shows a warning banner if the LittleFS web UI version doesn't match the new firmware's expected version. Follow the prompt to apply the matching web UI — the `nextube-WebUI-v{ver}.zip` via **System → Web UI Update** (recommended), or the full `nextube-littlefs-v{ver}.bin` via **System → LittleFS Recovery**.

**Settings are preserved automatically (both paths):** the Web UI ZIP never touches `config.json` at all, and a full LittleFS Recovery flash saves your `config.json` to NVS (a separate flash partition that is never erased by a LittleFS update) before wiping the partition, then restores it on the next boot. Either way you do not need to re-enter your Wi-Fi credentials, theme, brightness, or any other settings.

**Custom files are preserved only by the ZIP path:** a full LittleFS Recovery flash erases any themes, album images, or audio files you have uploaded. Back them up using the LittleFS file browser (**System → LittleFS Files**) before a recovery flash; the Web UI ZIP leaves them untouched.

#### Automatic Update Checks

The web UI automatically checks for new GitHub releases and shows a **dismissable toast notification** in the bottom-right corner if a newer version is available. The check runs once when the page loads and repeats every 24 hours while the page is open. No data leaves your network beyond the GitHub API query (`api.github.com/repos/MrToast99/Nextube-Remaster/releases/latest`).

The notification tells you which partitions changed (firmware-only, LittleFS-only, or both). Click **Online Updater** on the toast to install it automatically (see [Online Updater](#online-updater-recommended--one-click-no-downloads)), or note which files to download for a manual update. Dismiss it by clicking **✕** — it won't reappear until the next page load or 24-hour interval.

**Clock-face indicator (tube 6):** when the update toast appears, the firmware can also paint a **4-row solid red bar at the physical bottom of tube 6** (the rightmost tube) on every render frame so you know an update is waiting without having the web UI open. This is opt-in — enable it under **Display → Enable clock face update notification**. The bar appears as soon as an update is detected and clears automatically when the toast is dismissed or the option is unchecked. The indicator state is RAM-only and resets on reboot (the update check re-runs on next page load).

#### Migrating from an older SPIFFS build (v1.0.10 or older, all releases past v1.1.0 are already LittleFS)

Changing the partition subtype from `spiffs` to `littlefs` requires re-flashing the partition table — a firmware-only OTA is **not** sufficient. Perform a full USB re-flash using `nextube-fw-full.bin` (Option A or B above) once, then all subsequent OTA updates work normally.

#### Returning to the original factory firmware

The release includes `Stock Recovery Firmware/full_bak-used_flash_0x0.bin` — a complete flash dump of the original Rotrics firmware taken before any modifications. **This file is provided solely as a safety net for restoring the device to its factory state.** Flash it the same way as `nextube-fw-full.bin` (Option A or B above) at offset `0x0`. Doing so will erase all Nextube-Remaster settings, themes, and configuration and put the device back exactly as it shipped from Rotrics.

## Web Management UI

### Setup AP (WiFi Provisioning) and First connection

The device uses a **WPA2-secured** `Nextube-Setup` network for initial WiFi provisioning. The password is an **8-digit PIN** unique to each device, generated on first boot and stored in NVS.

> [!IMPORTANT]
> **The WiFi password is the PIN scrolling on the tubes** — there is no fixed or default password. Watch the display for the 8-digit code (it starts scrolling automatically once the device boots into setup mode), then type those digits into the password field when you connect to `Nextube-Setup`.

**Finding the PIN:**
- **LCD tubes** — while the setup AP is active and no client is connected, the PIN scrolls across the tubes as a repeating marquee: 3 blank tubes followed by all 8 digits, cycling continuously. Read the digits as they scroll past — the 3-blank gap gives your eye a clear reset point between repetitions.
- **Web UI** — once logged in, go to **System → WiFi Setup AP → Show** to display the PIN. You can also regenerate it there.
- **Serial monitor** — on first boot the PIN is logged: `Generated new AP PIN (first boot): 47391082`

**Connecting to the setup AP:**
1. The tubes display the 8-digit PIN (or find it in the serial log).
2. On your phone or laptop, connect to **Nextube-Setup** and enter the PIN **as the WiFi password** when prompted.
3. Navigate to **http://192.168.4.1**.
4. Set your admin password (first boot only), then enter your home WiFi credentials under **Network**.

> [!TIP]
> **No laptop nearby?** A smartphone works perfectly for initial setup. Connect your phone to **Nextube-Setup**, open a browser, and go to **http://192.168.4.1** to complete the WiFi configuration. Once the Nextube joins your home network, manage it from any device on the same network.

**AP lifecycle:**
- **No credentials saved** — AP stays up indefinitely for first-time setup.
- **Credentials saved, STA connects** — AP closes **60 seconds** after the device gets an IP, giving the browser time to finish loading the UI.
- **Credentials saved, STA fails to connect** — The AP does **not** reopen automatically. Use the touch-pad hotkey (see below) to bring it up on demand. The device keeps retrying STA in the background.
- **STA drops after connecting** — The AP does **not** return automatically. Use the touch-pad hotkey to regain access at `192.168.4.1`.

**Recovery hotkey — force the AP on demand:**

Hold the **LEFT** and **RIGHT** touch pads at the same time for **15 seconds**. The `Nextube-Setup` network starts broadcasting immediately. The AP closes automatically 60 seconds after the device next obtains a WiFi IP. This works regardless of whether STA is connected or disconnected, and is a no-op if the AP is already active.

After setup, access the management interface via:

- **http://nextube-remaster.local** (mDNS — works on most platforms without knowing the IP)
- **http://\<device-ip\>** (shown on the dashboard and in your router's DHCP table)

The web UI provides:
- **Dashboard** — live status (time, mode, weather, local sensor temp/humidity if SHT30 fitted, subscribers, heap), quick mode switching
- **Display**
  - Theme — populated dynamically from LittleFS; add a folder to `/images/themes/` and it appears in the dropdown automatically
  - **Custom Face** — select a TrueType font as the clockface digit renderer and weather-panel text font; choose from any `.ttf` uploaded to `/fonts/` on the device; selecting **None** reverts to the theme's JPEG digit artwork. Configure drop shadow colour and toggle under this section (see [Custom TTF Fonts](#custom-ttf-fonts))
  - Brightness; LED accent lighting (Static / Breath / Rainbow / Off / **Follow Sun/Moon**) with per-tube colour pickers — see [Follow Sun/Moon LED mode](#follow-sunmoon-led-mode)
  - Enabled mode toggles; auto mode rotation; auto theme rotation (cycle all or selected themes on a timer)
  - Spectrum settings — LED source (amplitude-modulated glow colour **or** follow accent mode), LCD bar colour (fixed **or** follow live WLED primary), Noise Floor threshold
  - **Advanced Display** — per-tube gamma, VCOM, panel profile, brightness trim, colour inversion, window offsets, anti-burn-in (see below)
- **Network** — WiFi SSID/password, hostname, timezone, NTP server, optional static IP (see [Static IP & Network Diagnostics](#static-ip--network-diagnostics)). Only reconnects when credentials actually change, preserving the live connection for all other saves.
- **Services**
  - Weather (source, city, units, panel rotation interval, per-panel enable/disable)
  - Social Media Counters (YouTube / Bilibili / Instagram / TikTok / Mastodon — see [Social Media Counters](#social-media-counters))
  - WLED Sync (see [WLED Sync](#wled-sync))
  - Home Assistant MQTT (broker, port, credentials, optional telemetry groups — see [Home Assistant MQTT](#home-assistant-mqtt))
- **Audio** — volume, ticker notification sound file
- **System**
  - OTA firmware update and web UI / LittleFS update
  - LittleFS file browser (browse / upload / delete / new folder / rename)
  - Device log viewer — live RAM-buffered log (last 200 lines), auto-refresh, per-subsystem enable/disable toggles, and a **Debug logging** checkbox that raises the runtime log level to DEBUG for diagnostic-only lines (not saved, resets on reboot)
  - Firmware update check (automatic on page load and every 24 h; dismissable toast; compares against latest GitHub release)
  - Lock Webui (enable/disable password protection, change password, sign out)
  - WiFi Setup AP PIN management (show / regenerate)
  - Factory reset (settings-only or full)
  - About (shows firmware + web UI versions independently)

### Admin Authentication (optional)

Authentication is **disabled by default** — the web UI is fully accessible to anyone on your network without a password, matching the behaviour of all previous firmware versions.

To enable password protection, go to **System → Lock Webui** and check **Require admin password to change settings**. On first enable you will be prompted to set a password (minimum 6 characters). The password is stored as a PBKDF2-SHA256 hash in NVS — it survives firmware OTA and is never visible in any API response. It is cleared only by a **Full factory reset**.

Once enabled:
- Every visit shows a login prompt. Sessions are stored in your browser's `localStorage` and remain valid for 7 days (sliding window).
- Five wrong password attempts in a row trigger a **60-second lockout**.
- All mutation endpoints and any endpoint that returns secrets (settings, AP PIN) require a valid bearer token.
- `/api/status`, static files, and the login endpoints remain open so the UI can always load and authenticate.

Authentication can be disabled again at any time from the same **Lock Webui** card (requires a valid session to turn off). You can also change the password there, or sign out to clear your local session.

Sessions are **RAM-only** and lost on reboot — you will be asked to log in once after each restart.

### Web UI Language

The web interface is available in **11 languages**:

| Code | Language |
|---|---|
| `en` | English *(default, always inline — no file load needed)* |
| `de` | German |
| `fr` | French |
| `es` | Spanish |
| `it` | Italian |
| `pt` | Portuguese |
| `nl` | Dutch |
| `sv` | Swedish |
| `no` | Norwegian |
| `da` | Danish |
| `fi` | Finnish |

On first load the UI auto-detects your **browser's preferred language**. If a matching translation is available it is fetched from `/lang/<code>.json` on the device and applied; otherwise the UI falls back to English. Your selection is saved in `localStorage` and persists across reloads and browser restarts.

To switch languages manually, use the **Language** dropdown at the top of any page.

**Tube display language** (the day-of-week abbreviation shown on clock and date panels — e.g. *Mon*, *Lun*, *Mo*) is a separate **device-wide** setting stored in firmware config. Configure it under **Network → Date & Time → Language**. It defaults to English and does not follow the web UI language — this lets the physical clock show one language while the management interface is in another.

### Static IP & Network Diagnostics

**Network → WiFi Configuration** includes an optional **Use static IP** toggle. When enabled, set the IP address, subnet mask, gateway, and DNS (a second DNS server is optional). DHCP is used whenever this is off (the default) or any required field is left blank on boot.

> [!IMPORTANT]
> **Requires reboot to take effect**, same as the Hostname field above. If you enter settings that make the device unreachable (wrong gateway/subnet), hold the **LEFT** and **RIGHT** touch pads for 15 seconds to bring up the `Nextube-Setup` recovery network (see [Setup AP](#setup-ap-wifi-provisioning-and-first-connection) above) and fix the settings from there — misconfigured static IP doesn't prevent the device from associating to your WiFi, so this recovery path always works.

Below the WiFi Configuration card, a collapsible **Network Info** panel shows read-only diagnostics, fetched on demand when expanded:
- **Connected since** — time since the current WiFi association was established
- **Disconnects (session)** — count of STA disconnect events since the last reboot
- **Last disconnect reason** — the most recent `wifi_err_reason_t` code, with common ones (beacon timeout, auth failure, handshake timeout, etc.) shown as human-readable text
- **Signal** — RSSI of the current AP association, in dBm
- **MAC / BSSID / Channel** — link-level details of the current AP association
- **Netmask / Gateway / DNS** — the STA interface's active IP configuration, whether obtained via DHCP or set statically

### Advanced Display (LCD Calibration)

**Display → Advanced Display** contains per-tube hardware calibration controls for mixed panel sets (original + replacement tubes, or batches of replacement panels that differ slightly from each other). All changes take effect after **Save** and are applied immediately without a reboot.

#### Gamma Correction

| Setting | Per-tube | Range | Default | Description |
|---|---|---|---|---|
| **Gamma** | ✅ Yes (6 sliders) | 0.5–3.0 | 1.0 | Software midtone correction applied in the pixel render loop. `1.0` = identity (zero overhead). Values above 1.0 darken midtones — try **1.8–2.2** for washed ST7735S replacement panels. Values below 1.0 brighten midtones. |

Gamma is implemented as a pre-computed integer lookup table (`out = in ^ γ`, one table for R/B 5-bit and one for G 6-bit per tube). There is no floating-point math per pixel at render time — the overhead is negligible even at 5 Hz.

#### Panel Profile

| Setting | Per-tube | Values | Default | Description |
|---|---|---|---|---|
| **Profile** | ✅ Yes | Standard · Vivid | Standard | Selects the hardware gamma curve (register 0xE0/0xE1) written to the ST7735 during init. **Standard** is tuned for the original Green-Tab panels; **Vivid** uses a recalibrated curve for ST7735S replacement panels. Changing the profile triggers a per-tube software reset (SWRESET) + full reinit — the display task is briefly suspended for the duration. |
| **VCOM** | ✅ Yes | 0–63 | 14 | Sets the VMCTR1 AC driving voltage (register 0xC5). Higher values increase contrast and colour saturation. Original panels use **14** (0x0E). For ST7735S replacements, start at **40** and tune from there — increase toward 60 if still washed, decrease toward 25 if over-saturated. Also requires a SWRESET+reinit to take effect (handled automatically). |

> **Why SWRESET is needed:** The ST7735S only latches VCOM and the hardware gamma registers during the `SLPOUT → DISPON` initialisation window. Writes issued while the display is already on are silently ignored. The firmware performs a CS-gated software reset on the affected tube only — the other five tubes are unaffected.

#### Individual Brightness Trim

| Setting | Per-tube | Range | Default | Description |
|---|---|---|---|---|
| **Brightness** | ✅ Yes | 0–100 % | 100 % | Scales RGB565 pixel components (R5, G6, B5) by `value / 100` in the render loop. Use when a replacement panel is noticeably brighter than the originals. Applied in the same integer-only pixel pass as gamma — no additional overhead. |

#### Colour Inversion

| Setting | Per-tube | Description |
|---|---|---|
| **Invert** | ✅ Yes (per-tube checkbox) | Sends INVON (0x21) to tubes that need colour inversion. Some ST7735S batches default to an inverted colour space — whites appear black without this. Takes effect immediately (no SWRESET needed; INVON/INVOFF survive normal display-on mode). |

#### Window Offsets

| Setting | Per-tube | Range | Default | Description |
|---|---|---|---|---|
| **Column Offset** | ✅ Yes | −8 to +8 | 0 | Shifts the CASET window start column. LH096NT-IF09W and similar ST7735S variants typically need **+2** to prevent 1 px of uninitialised frame-buffer from appearing at the right edge. |
| **Row Offset** | ✅ Yes | −8 to +8 | 0 | Shifts the RASET window start row. Same ST7735S variants typically need **+1** to prevent 1 px at the bottom edge. |

#### Anti-Burn-In

| Setting | Description |
|---|---|
| **Colour Cycle** | Cycles each selected tube through red → green → blue → white → black (30 s per step) to exercise every sub-pixel at both voltage extremes. Can run for 1–4 hours or until manually stopped. |
| **Static Snow** | Writes truly random RGB565 pixels to every selected tube each frame (5 Hz) — more thorough than the colour cycle because every pixel address receives an independent random level. Tube selection and duration are the same as colour cycle. |
| **Scheduled** | Automatic overnight burn-in recovery. Fires at a configured hour on a **weekly** (Sunday midnight) or **monthly** (1st of month) schedule. Tube bitmask, session duration, and trigger hour are all configurable. Disabled by default. |

The CASET window also drifts ±2 px every hour automatically (synchronized to the real-time hour value) as a passive column-shift anti-burn-in measure — no configuration needed.

## Modes

| Mode | Description |
|---|---|
| **Clock** | 12H or 24H digital clock. **24H Custom** shows rotating info panel(s) on the right-hand tube(s) (configurable under Display → 24H Custom). Available panels: Day+date, Indoor temp & humidity (SHT30), Outdoor temperature + today's Hi/Lo, Sunrise & Sunset times (NOAA algorithm, geocoded from weather city), Weather icon, and **Pushed image** (an 80×160 JPG you POST from an external script — see [Pushed images on tube 5/6](#pushed-images-on-tube-56-24h-custom)). **Single-panel (default):** `H H : M M` with one rotating panel on tube 6. **Dual-panel:** the colon is dropped (`H H  M M`) and tubes 5 **and** 6 each show an **independently-configured** rotating panel — each tube has its own enabled-panel set and cycles through it on the shared rotation interval. Selecting the **WeatherLive** theme replaces the clock with a fully procedural animated weather sky — see [WeatherLive theme](#weatherlive-theme). |
| **Date** | Date display (DD/MM/YY). Can be enabled alongside Clock — both appear as separate stops in the touch cycle. |
| **YouTube** | Live subscriber count. Direct fetch or via local relay (recommended — see [Social Media Counters](#social-media-counters)). |
| **Instagram** | Live follower count. Fetched directly from Instagram's unofficial public API — no account or relay required. |
| **TikTok** | Live follower count. Requires the local relay (`social_relay.py`) — TikTok's bot detection blocks direct ESP32 fetches. |
| **Mastodon** | Live follower count. Fetched directly from the configured Mastodon instance API — no relay required. |
| **Weather** | Up to three panels cycling on a configurable interval: **Panel 1** — temperature + °C/°F + condition icon; **Panel 2** — humidity + % + condition icon; **Panel 3** — animated sunrise/sunset (rising/setting sun + mountain silhouettes at 20 Hz, solar times in HH:MM). Any combination of panels can be enabled; at least one must remain on. Temperatures rounded to whole degrees; leading zeros suppressed; minus sign shifts with digit count. All 6 tubes show `······` (dots) until the first fetch completes. |
| **Album** | Slideshow of JPEGs from `/images/album/`. Each tube shows a **different** image offset by its position — with 6+ images all tubes are unique; with fewer they wrap gracefully. Images advance as a sliding window every `album_switch_ms` (default 2 s). |
| **Spectrum** | Microphone audio visualiser. 24 Goertzel bands (280–3800 Hz, log-spaced) drive **4 segmented mini-bars per tube** with a white peak-dot indicator. Tubes read left-to-right from bass to treble. Uses the onboard electret capsule + LMV321IDBVR preamp on GPIO35 (ADC1_CH7). Adaptive per-band noise floor subtraction ensures bars sit at zero in silence. **LED source**, **LED ring colour**, **LCD bar colour**, and **Noise Floor** threshold are independently configurable in **Display → Spectrum Mode**. |

### Mode Rotation

Enable **Auto Rotation** in Display settings to automatically cycle through all enabled modes on a configurable interval (15 s → 1 hour). When disabled, modes only change via the Quick Actions buttons or the physical left/right touch pads. Any manual mode change resets the rotation timer.

### Theme Rotation

Enable **Theme Rotation** in Display settings to automatically cycle through themes on a timer (1 minute → 4 hours, default 5 minutes).

- **All themes** — leave every checkbox ticked (or click **All**) to rotate through every theme installed on the device. Any custom theme you upload to `/images/themes/` is picked up automatically at the next rotation event — no restart required.
- **Selected themes** — uncheck themes you want to exclude. Only the ticked themes participate in rotation. Click **None** to start a fresh selection.
- The rotation order is alphabetical. Any manual theme change (saving a new theme via the dropdown) resets the timer.
- The active theme is held in RAM during rotation and is not written to flash on each step. The theme shown at boot will be whichever was last explicitly saved via the web UI.

### Touch Buttons

| Button | Action |
|---|---|
| LEFT | Previous enabled mode |
| MIDDLE | Toggle LCD displays on/off (backlight) |
| RIGHT | Next enabled mode |
| LEFT + RIGHT (hold 15 s) | Force the setup AP on — broadcasts `Nextube-Setup` so you can reach the web UI at `192.168.4.1` to fix WiFi credentials |

## Weather

Weather mode cycles through all enabled weather APIs until one succeeds. Supported sources:

| Source | API Key | Geocoding | Notes |
|---|---|---|---|
| **wttr.in** | None | wttr.in built-in | Accepts many formats — see below |
| **Open-Meteo** | None | Open-Meteo geocoding API | Splits `City,CC` into name + country filter |
| **OpenWeatherMap** | Free-tier key | OWM built-in | Accepts city, state, zip — see below. Register at [openweathermap.org](https://home.openweathermap.org/users/sign_up) — free tier includes 1 000 calls/day |
| **Met.no** | None | Open-Meteo geocoding API | **Default.** Uses same geocoder as Open-Meteo; elevation auto-fetched for accurate forecasts |
| **External** | None | None (you supply lat/lon) | No online fetch — you push your own reading via `POST /api/weather`. See [External weather (push your own data)](#external-weather-push-your-own-data) |

### City format

All providers share a single **City** field (**Services → Weather → City**). The same string is used by whichever provider is active, so pick a format that works for your selected source.

**Plain city name — works with all providers:**
```
London
Tokyo
Paris
```
When two cities share a name (e.g. *Springfield*, *Florence*) the provider returns the first match, which may be the wrong country. Use `City,CC` to disambiguate.

**`City,CC` — recommended for ambiguous city names, supported by all providers:**
```
Florence,IT       ← Florence, Italy  (not Florence, South Carolina)
Springfield,US
London,GB
Sydney,AU
```
`CC` is the two-letter [ISO 3166-1 alpha-2](https://en.wikipedia.org/wiki/ISO_3166-1_alpha-2) country code — or the full country name (e.g. `Canada`); both are accepted.  
Open-Meteo and Met.no: the geocoder fetches up to five candidates by name, and the firmware picks the first whose country matches the `CC` segment — comparing it against both the result's `country_code` **and** its full `country` name.  
wttr.in and OpenWeatherMap pass the whole string to their own resolvers.

**Multi-word city names** (e.g. *New York*, *Los Angeles*) work as-is on every provider — the firmware percent-encodes the city before each request, so a plain space is fine: `New York,US`.

### Live city verification

The web UI checks your **City** entry **as you type** (and on load / when you change the source), so you know whether it will actually resolve *before* saving:

- **✓ valid** — shows the resolved place and coordinates, e.g. `✓ Ottawa, Ontario, Canada (45.42, -75.70)`.
- **✗ not found** — prompts you to try the `City,CountryCode` form.
- **OpenWeatherMap** — verified through OWM's own geocoder using the API key you entered, so a missing or rejected key is reported here too.

It resolves against the same service the firmware uses (the keyless Open-Meteo geocoder for Open-Meteo/Met.no, which also closely matches wttr.in), so "valid here" means "will resolve on the device". The lookup runs in your browser against the providers' public APIs; if it can't reach them it falls back to a basic format check rather than reporting a false failure. Verification is advisory — it never blocks saving.

**OpenWeatherMap extras** (OWM only, not recognised by other providers):
```
Austin,Texas,US          ← City, state, country (useful for US cities)
94040,US                 ← US zip code
EC1A,GB                  ← UK postcode prefix
```

### External weather (push your own data)

Selecting **Services → Weather → Source → External** stops the firmware from contacting any online service. Instead, *you* push readings to the device — ideal if your home-automation system already averages several providers for a more stable result. Set the source to **External** and save (no reboot needed; the poller stops on its next cycle).

**Endpoint:** `POST /api/weather`

```json
{
  "temp_c":       15.3,
  "humidity":     65,
  "condition":    "Cloudy",
  "icon":         "overcastClouds",
  "weather_code": 3,
  "lat":          51.30,
  "lon":          -114.02
}
```

All fields are **optional** — any field you omit keeps its current value (partial updates work, exactly like the network providers):

| Field | Type | Notes |
|---|---|---|
| `temp_c` | number | Temperature in **°C**. Omit to leave unchanged. |
| `temp_f` | number | Temperature in **°F** — converted to Celsius on ingest. **Send only one of `temp_c` / `temp_f`**, in whichever unit your source produces; if both are present, `temp_c` wins. |
| `humidity` | number | Relative humidity %. Omit to leave unchanged. |
| `condition` | string | Free text shown on the panel (e.g. `"Cloudy"`). |
| `icon` | string | One of the built-in icon names (below). Unknown names are ignored (previous icon kept). |
| `weather_code` | number | [WMO weather code](https://open-meteo.com/en/docs#weathervariables). Fills `icon`/`condition` automatically when those are absent — so Open-Meteo-style data works directly. |
| `lat` / `lon` | number | Optional. Used **on-device** for the Sunrise & Sunset panel (no extra API call). Send both or neither. |

> [!IMPORTANT]
> **Temperature units:** send your reading in **only one** format — `temp_c` *or* `temp_f` — using whichever unit your source produces. The firmware stores everything in Celsius internally and converts `temp_f` on the way in. What the clock *displays* (°C or °F) is controlled separately by **Services → Weather → Units**, independent of which format you POST.

**Built-in icon names:** `sun`, `fewClouds`, `overcastClouds`, `fog`, `rain`, `snow`, `squalls`, `thunderstorm`.

**Icon selection** — send any one of:
- an explicit `"icon"` name from the list above, **or**
- a `"weather_code"` (WMO) and the firmware maps it to an icon + condition, **or**
- neither, to leave the current icon unchanged.

**Sunrise/Sunset location:** the sun panel computes rise/set times locally from `lat`/`lon`. In External mode there is no geocoding, so coordinates come only from your push. The last pushed `lat`/`lon` is **persisted to flash** (written only when it changes, so periodic pushes don't wear flash) and **restored on boot**, so the panel works immediately after a reboot. Send `lat`/`lon` once (or in every push — harmless); until coordinates are provided the panel shows `--:--`.

**Requirements & notes:**
- Weather must be **enabled** (**Services → Weather → Enable**) so the panel and subsystem are running.
- The endpoint honours auth like every other mutation route: with no admin password set it is open on the LAN; once a password is set, send the `Authorization: Bearer <token>` header.
- Pushed data is held until the next push; it does not expire. If your automation stops pushing, the last value stays on screen.

Example with `curl`:
```bash
curl -X POST http://nextube.local/api/weather \
     -H "Content-Type: application/json" \
     -d '{"temp_c":15.3,"humidity":65,"condition":"Cloudy","icon":"overcastClouds","lat":51.30,"lon":-114.02}'
```

Weather fetching (online sources): On WiFi connect the first fetch happens immediately with automatic 5-second retries until data arrives. After the first successful fetch, weather is refreshed every 10 minutes.

Weather mode auto-cycles between up to three panels on a configurable interval (default 5 s). Each panel can be individually enabled or disabled under **Services → Weather → Display Panels**; at least one must remain on.

**Panel 1 — temperature + icon:**
```
positive 1-digit:  [blank] [blank] [blank] [units] [°C/°F] [icon]
positive 2-digit:  [blank] [blank] [tens]  [units] [°C/°F] [icon]
negative 1-digit:  [blank] [blank] [−]     [units] [°C/°F] [icon]
negative 2-digit:  [blank] [−]     [tens]  [units] [°C/°F] [icon]

e.g.   2°C:  _   _   _   2   °C  ☁
e.g.  15°C:  _   _   1   5   °C  ☁
e.g.  −7°C:  _   _   −   7   °C  ☁
e.g. −23°C:  _   −   2   3   °C  ☁
```

**Panel 2 — humidity + icon:**
```
[blank] [blank] [tens/blank] [units] [%] [icon]
```

**Panel 3 — Sunrise & Sunset (animated):**
```
tube 0 : animated sunrise — full-circle sun rises from behind mountain silhouettes, holds at top, loops
tube 1 : local sunrise time "HH:MM" (or "--:--" until geocoding completes)
tube 2 : blank
tube 3 : blank
tube 4 : animated sunset  — sun descends into mountains, holds at bottom, loops
tube 5 : local sunset time "HH:MM"
```
Solar times are calculated on-device using the NOAA solar position algorithm from lat/lon — the coordinates resolved by the weather fetch (online sources), or the `lat`/`lon` you supply via `POST /api/weather` (External source, persisted across reboots). No extra API call is made. Before coordinates are available (no successful fetch yet, no city configured, or no coordinates pushed) the times show `--:--`. The animation runs at 20 Hz; switching from another weather panel to Panel 3 starts the animation within 50 ms.

**Waiting (no data yet):**
```
[·] [·] [·] [·] [·] [·]   (all tubes show dot.jpg until first fetch)
```

Required LittleFS image files — see the **Themes** section for the full folder structure. Key files for weather:

```
AMPM/              blank.jpg   dot.jpg
MutiInfo/Temperature/   degreec.jpg   degreef.jpg   minus.jpg
MutiInfo/Weather/       sun.jpg  fewClouds.jpg  overcastClouds.jpg  fog.jpg
                        rain.jpg  snow.jpg  squalls.jpg  thunderstorm.jpg
                        sand.jpg  tornado.jpg  volcanicAsh.jpg
```

## Pushed images on tube 5/6 (24H Custom)

The **Pushed image** info panel lets an external script drive the right-hand tube(s) with arbitrary artwork — a tiny status icon, a sparkline rendered elsewhere, a now-playing thumbnail, anything you can produce as a JPG. It is one of the rotating 24H Custom panels, so it shares the tube with whatever other panels you enable.

**Enable it:** Display → **24H Custom**, tick **Pushed Image** for tube 6 (and/or tube 5 in dual-panel mode). Use Clock mode with **24H Custom** time format on any normal (asset) theme.

> Asset/base themes only. The **WeatherLive** theme draws its own procedural panels and ignores the pushed-image panel.

**Endpoint:** `POST /api/cx_image?tube=5|6` — the request body is the raw JPG.

| Parameter | Notes |
|---|---|
| `tube` (query) | `6` = rightmost tube (LCD 5); `5` = 2nd-from-right (LCD 4, only visible in **dual-panel** mode). Required. |
| body | A **JPEG that decodes to exactly 80×160 px** (the tube resolution). Max 96 KB. Non-80×160 images are rejected with `400`. |

**Behaviour:**
- The image is decoded to RGB565 once on receipt and held in PSRAM. It is shown whenever the Pushed-image panel is the active rotation slot, and **persists until the next push or a reboot** (it is not saved to flash).
- A fresh push appears within one display tick (~200 ms) if its panel is currently on-screen; otherwise on the panel's next rotation.
- Until the first image is pushed, an enabled-but-empty Pushed-image slot falls back to the Day+date panel (so the tube is never blank).
- Honours auth like every other mutation route: open on the LAN with no admin password set; otherwise send `Authorization: Bearer <token>`.

Example with `curl` (push a frame to the rightmost tube):
```bash
curl -X POST "http://nextube.local/api/cx_image?tube=6" \
     -H "Content-Type: image/jpeg" \
     --data-binary @frame.jpg
```

Generate an 80×160 JPG on the fly (ImageMagick):
```bash
magick -size 80x160 xc:black -gravity center \
       -fill white -pointsize 28 -annotate 0 "OK" frame.jpg
curl -X POST "http://nextube.local/api/cx_image?tube=6" --data-binary @frame.jpg
```

## Air Quality panel

An outdoor **Air Quality Index** info panel for tube 5/6, available on both the **WeatherLive** face and the asset themes (rendered over the theme's `blank.jpg`). It shows a large **"AQI"** label over the index value, sized to fit 1–3 digits and **colour-coded by health band**.

**Data source:** the free, keyless **Open-Meteo Air Quality API**, reusing the same geocoded location as the weather provider — so it works with any weather source (Met.no, Open-Meteo, wttr.in, OWM, external) and needs **no API key**. The value refreshes on the weather poll (every 10 min, but AQI itself is only hourly in nature).

**Two scales** (Display → 24H Custom → **AQI scale**):

| Setting | Behaviour |
|---|---|
| **Auto (by location)** | European AQI (0–100) inside the Europe bounding box, US EPA AQI (0–500) elsewhere |
| **US AQI** | Always the US EPA index (0–500): green → yellow → orange → red → purple → maroon |
| **European AQI** | Always the CAMS/EEA index (0–100+): good → fair → moderate → poor → very poor → extremely poor |

The number is drawn in its band colour; the "AQI" label uses the theme/Custom-Face font colour. Until the first fetch completes it shows `--`.

**Enable it:** Display → **24H Custom**, tick **Air Quality** for tube 6 (and/or tube 5 in dual-panel mode), and pick the **AQI scale**. Also exposed to Home Assistant as the **Nextube Air Quality** sensor — see [Home Assistant MQTT](#home-assistant-mqtt).

## NTP & Clock Accuracy

The Nextube synchronises its system clock from NTP servers immediately on WiFi connect, then once per hour. Between syncs, accuracy depends on the active **Time Discipline Mode**.

### How synchronisation works

On each hourly NTP sync the firmware:

1. Measures how far the ESP32 internal clock has drifted against the NTP timestamp.
2. Hard-sets or gradually slews the clock depending on the active discipline mode and the magnitude of drift.
3. Writes the **rounded NTP second** to the PCF8563 RTC so the battery-backed time survives a power cycle. The write rounds to the nearest second (not truncates), eliminating the 0–1 s systematic bias truncation would otherwise introduce.

### Time discipline modes

| Mode | Name | How it works | Typical accuracy between NTP syncs |
|---|---|---|---|
| **0** | Off | Clock set exactly to NTP each hour; no correction applied between syncs | ESP32 XTAL drift — typically 100–350 ppm (360–1260 ms/hr, erratic) |
| **1** | ESP Rate | Learns the ESP32 XTAL drift rate via an exponential moving average and pre-compensates continuously using `adjtime()` | ~10–50 ms/hr depending on XTAL stability |
| **2** | PCF8563 Slave *(default)* | Every minute, reads the PCF8563 RTC and applies a small `adjtime()` nudge to keep the system clock aligned with the RTC crystal | ~1–2 ms steady-state; one-off ≤0.5 s realignment immediately after each NTP sync |

**Mode 2 is the default and recommended setting.** The PCF8563's crystal is far more stable than the ESP32's internal XTAL oscillator — in testing the PCF8563 shows ~0 ppm drift per hour while the ESP32 XTAL varies between 112–337 ppm (erratic, not temperature-correlated). PCF slave mode delivers ~1 ms steady-state clock accuracy on the physical tubes at the cost of one I²C read per minute.

### NTP sync log

At each NTP sync a single line is emitted at `INFO` level on the `ntp` tag:

**Mode 2 — PCF slave (default):**
```
ntp: NTP sync: XTAL (ESP) would have been +1048 ms — PCF (RTC) kept <=4 ms between syncs
```

The first number is how far the ESP32 crystal's free-running timebase drifted since the last sync — what the clock *would have been* without discipline (the actual clock was held by the PCF every minute). The number after the dash is the worst single-minute error the PCF slave saw over the past hour.

**Mode 0 — off, small drift (gradual slew):**
```
ntp: NTP sync: slewing +12 ms (~7 min)
```

**Mode 0 — off, large drift (hard set):**
```
ntp: NTP sync: +1048 ms corrected
```

Per-minute PCF corrections are silent at `INFO` level; set the `ntp` tag to `DEBUG` to see each individual tick correction.

---

## Social Media Counters

Five platforms are supported. Tube 0 shows the platform icon; tubes 1–5 show the follower/subscriber count with K/M scaling (e.g. 1 230 000 → `1230 K`).

| Platform | Fetch method | Relay required? |
|---|---|---|
| **YouTube** | Direct fetch **or** via local relay | Optional (relay strongly recommended — direct ESP32 fetches are often bot-blocked) |
| **Bilibili** | Direct unofficial API | No |
| **Instagram** | Direct unofficial public-profile API | No |
| **TikTok** | Via local relay | **Yes** — TikTok's JS fingerprinting blocks direct device fetches |
| **Mastodon** | Direct Mastodon instance API | No |

Configure all platforms under **Services → Social Media Counters** in the web UI.

### Optional API Keys

By default all platforms use unofficial or keyless fetch methods. Official API keys are optional but improve reliability and are less likely to be rate-limited or blocked.

| Platform | Field | Where to get a key |
|---|---|---|
| **YouTube** | `YouTube API Key` | [Google Cloud Console — YouTube Data API v3](https://console.cloud.google.com/apis/library/youtube.googleapis.com) — free quota is 10 000 units/day; a single subscriber-count lookup costs 1 unit. Create a project → Enable the API → Credentials → API Key. |
| **TikTok** | `TikTok Research API Key` | [TikTok for Developers — Research API](https://developers.tiktok.com/products/research-api/) — requires a developer account and application approval. Use a bearer token from the approved app. If no key is set, the relay's browser-based method is used instead. |

Both keys are entirely optional. Leave the field blank to use the default keyless method.

**Master switch (`Enable`)** — when unchecked the polling task never starts. Changes require a device restart to take effect. All social counter platforms (including YouTube/Bilibili) are **disabled by default** — enable whichever you use and save; the task only starts if at least one platform is enabled.

**Polling interval** — applies to all platforms. Preset buttons: 5 m, 10 m, 30 m, 1 h, 6 h, 12 h, 24 h. Default is **1 hour**. Minimum 5 minutes. Changes take effect after the current sleep expires — no restart needed.

### Local Relay (`social_relay.py`)

> [!TIP]
> **Do you need the relay?**
> - **No interest in social counters** — skip this section entirely. The relay is not required for any other feature.
> - **YouTube with an API key** — skip the relay. Enter your [YouTube Data API v3](https://console.cloud.google.com/apis/library/youtube.googleapis.com) key in the web UI and the device fetches counts directly.
> - **YouTube without an API key, or TikTok** — the relay is required. TikTok's bot detection blocks direct ESP32 fetches; YouTube without a key is also often blocked. Run the relay on any PC on the same network.
> - **Instagram / Mastodon / Bilibili** — no relay needed; these platforms are fetched directly from the device.

`helpers/social_relay/social_relay.py` is a lightweight Python HTTP proxy that runs on any PC on the same network as the Nextube. It fetches YouTube and TikTok counts using a real Chromium browser (Playwright), bypassing bot-detection checks that block the ESP32's plain HTTP client, then serves the result as simple JSON at `http://<relay-host>:8888/`.

#### Quick start

```bash
python helpers/social_relay/social_relay.py
```

> **Firewall note:** The relay listens on port **8888**. If your machine's firewall prompts you when the script first starts, allow access for Python on your **private/home network**. Without this the Nextube cannot reach the relay. On Windows, Defender Firewall will show a pop-up the first time — click *Allow access*. On macOS, accept the incoming-connection prompt. On Linux, allow the port with e.g. `sudo ufw allow 8888/tcp`.

**All dependencies are installed automatically on first run** — no `pip install` step needed. The script detects missing packages at startup and installs them:

- `playwright` (the browser automation library)
- Chromium browser binary (`playwright install chromium`)
- `playwright-stealth` (suppresses headless-browser fingerprint signals)

On subsequent runs the packages are already present and startup is instant.

#### What gets installed

| Package | Purpose |
|---|---|
| `playwright>=1.40` | Chromium browser control |
| `playwright-stealth>=1.0.6` | Patches ~12 browser signals (navigator.webdriver, WebGL vendor, plugin arrays, etc.) that TikTok and YouTube use to detect automation |

To install manually instead (e.g. in a virtual environment):

```bash
pip install playwright playwright-stealth
playwright install chromium
```

#### Routes

| Route | Returns |
|---|---|
| `GET /youtube?channel=<id>` | `{"subscribers": 12345}` |
| `GET /tiktok?user=<username>` | `{"followers": 12345}` |
| `GET /health` | `OK` |

#### YouTube channel identifier

The relay accepts any of these formats in the `channel=` parameter:

| Format | Example | URL built |
|---|---|---|
| Channel ID | `UCvFu9z6btYUaK3PEtsg6zoA` | `youtube.com/channel/UC…` |
| Handle with `@` | `@MrToast99` | `youtube.com/@MrToast99` |
| Bare handle | `mrtoast99` | `youtube.com/@mrtoast99` |

Find a channel ID in the channel URL or via `youtube.com/@handle/about`.

#### Fetch strategy

Both platforms use the same layered fallback:

1. **Playwright/Chromium** — real browser with stealth patches; passes TLS fingerprinting and JS bot-detection checks. Primary method.
2. **curl** — correct OS TLS fingerprint; no JS execution. TikTok fallback.
3. **urllib** — pure Python; may be WAF-blocked. Last resort.

Results are cached for 5 minutes. The ESP32 HTTP timeout for relay requests is 45 seconds, giving Playwright time to launch Chromium on cold start.

#### Configuring the relay host

1. Start the relay — it prints the relay address on startup, e.g. `http://192.168.1.50:8888`
2. Open the Nextube web UI → **Services → Social Media Counters**
3. Enter the IP address (without port) in **Relay host**
4. Save

The relay must be running whenever the device polls. It does not need to be running continuously — the Nextube caches the last received count and only re-fetches on the next polling interval.

## Home Assistant MQTT

The Nextube can connect to a Home Assistant MQTT broker and register itself automatically via **MQTT auto-discovery** — no manual HA entity configuration needed.

### What gets exposed

| Entity | HA domain | Group | What it does |
|---|---|---|---|
| **Nextube Temperature** | `sensor` | Always | SHT30 temperature in °C, updated every 60 s |
| **Nextube Humidity** | `sensor` | Always | SHT30 relative humidity %, updated every 60 s |
| **Nextube Air Quality** | `sensor` | Always | Outdoor AQI (US or European per the panel's scale setting) from Open-Meteo, updated every 60 s |
| **Nextube Mode** | `select` | Always | Read and set the active display mode (Clock, Weather, YouTube, …) |
| **Nextube Display** | `switch` | Always | Turn the backlight ON or OFF (same as the middle touch button) |
| **Nextube Brightness** | `number` | Always | LCD brightness 0–100 slider |
| **Nextube Ticker** | `text` | Always | Scrolling message ticker — type any text and press Enter to display it across all 6 tubes |
| **Nextube Ticker Speed** | `number` | Always | Marquee scroll speed slider, 1–20 px per tick (higher = faster). RAM-only; resets to 4 on reboot |
| **Nextube Ticker Sound** | `switch` | Always | Enable/disable the chime played on each incoming ticker message |
| **Nextube XTAL Drift** | `sensor` | Clock telemetry *(default off)* | Signed ms the ESP32 crystal would have drifted without NTP discipline; retained, updated each NTP sync |
| **Nextube RTC Max Error** | `sensor` | Clock telemetry *(default off)* | Worst single-minute clock error (ms) the PCF8563 discipline saw since the last sync; PCF slave mode only |
| **Nextube RSSI** | `sensor` | Device health *(default off)* | WiFi signal strength in dBm; updated every 60 s |
| **Nextube Free Heap** | `sensor` | Device health *(default off)* | Free heap bytes; updated every 60 s |
| **Nextube Uptime** | `sensor` | Device health *(default off)* | Device uptime in minutes; updated every 60 s |
| **Button press** (left / middle / right) | `device_automation` | Button presses *(default off)* | HA device-trigger fired on each capacitive touch press; usable in HA automations (e.g. "left button → toggle bedroom lights") |

### Setup

1. Make sure your Home Assistant instance has an MQTT broker running (the built-in **Mosquitto** add-on works). Plain `mqtt://` (no TLS) is used.
2. Open the Nextube web UI → **Services → Home Assistant MQTT**.
3. Check **Enable** (restart required).
4. Enter your broker's **Broker** hostname or IP address (e.g. `homeassistant.local` or `192.168.1.x`).
5. Set **Port** (default `1883`).
6. Fill in **Username** and **Password** if your broker requires authentication; leave blank for anonymous access.
7. Leave **Publish HA auto-discovery payloads** checked (recommended).
8. Optionally enable extra publishing groups: **Clock telemetry** (NTP sync drift sensors — off by default), **Device health** (RSSI / heap / uptime sensors, 60 s interval — off by default), **Button presses** (touch events as HA device-automation triggers — off by default). New entity groups appear after the next broker reconnect or reboot.
9. Click **Save**, then reboot the device (**System → Reboot**).

After reboot HA will show a new **Nextube** device under **Settings → Devices & Services → MQTT** within a few seconds of the device connecting.

### MQTT topics

All topics use the device hostname (default `nextube-remaster`, configurable in **Network Settings**) as the unique ID.

| Direction | Topic | Payload |
|---|---|---|
| Publish | `nextube/<hostname>/sensor/temperature/state` | `{"temperature": 21.4}` |
| Publish | `nextube/<hostname>/sensor/humidity/state` | `{"humidity": 55.1}` |
| Publish | `nextube/<hostname>/mode/state` | `Clock` (plain string) |
| **Subscribe** | `nextube/<hostname>/mode/set` | `Weather` (mode name) |
| Publish | `nextube/<hostname>/display/state` | `ON` or `OFF` |
| **Subscribe** | `nextube/<hostname>/display/set` | `ON` or `OFF` |
| Publish | `nextube/<hostname>/brightness/state` | `75` (integer 0–100) |
| **Subscribe** | `nextube/<hostname>/brightness/set` | `75` (integer 0–100) |
| Publish | `nextube/<hostname>/ticker/state` | Current ticker text, or `""` when cleared |
| **Subscribe** | `nextube/<hostname>/ticker/set` | UTF-8 string ≤ 255 chars; empty payload = cancel |
| Publish | `nextube/<hostname>/ticker_speed/state` | `4` (integer 1–20, px per tick) |
| **Subscribe** | `nextube/<hostname>/ticker_speed/set` | `4` (integer 1–20; clamped, echoed back) |
| Publish | `nextube/<hostname>/ticker_sound/state` | `ON` / `OFF` — chime on ticker text |
| **Subscribe** | `nextube/<hostname>/ticker_sound/set` | `ON` / `OFF` — persisted to config |
| Publish | `nextube/<hostname>/ntp/xtal_drift/state` | Signed ms the free-running ESP crystal drifted since the previous NTP sync (retained; updated per sync) |
| Publish | `nextube/<hostname>/ntp/rtc_err/state` | Worst single-minute clock error (ms) the PCF8563 discipline allowed since the previous sync (retained; PCF-slave mode only) |
| Publish | `nextube/<hostname>/health/state` | *(optional group)* `{"rssi":-53,"heap":51234,"uptime_min":185}` every 60 s |
| Publish | `nextube/<hostname>/button/state` | *(optional group)* `left` / `middle` / `right` on each touch press |
| Publish | `nextube/<hostname>/update/state` | `ON` or `OFF` (retained) — a newer firmware release is available |
| Publish | `nextube/<hostname>/update/latest_version/state` | `1.18.0` (retained) — latest release tag found |
| Publish | `homeassistant/sensor/<hostname>_temp/config` | HA discovery JSON (retained) |
| Publish | `homeassistant/sensor/<hostname>_hum/config` | HA discovery JSON (retained) |
| Publish | `homeassistant/select/<hostname>_mode/config` | HA discovery JSON (retained) |
| Publish | `homeassistant/switch/<hostname>_display/config` | HA discovery JSON (retained) |
| Publish | `homeassistant/number/<hostname>_brightness/config` | HA discovery JSON (retained) |
| Publish | `homeassistant/text/<hostname>_ticker/config` | HA discovery JSON (retained) |
| Publish | `homeassistant/number/<hostname>_ticker_speed/config` | HA discovery JSON (retained) |
| Publish | `homeassistant/switch/<hostname>_ticker_sound/config` | HA discovery JSON (retained) |
| Publish | `homeassistant/sensor/<hostname>_xtal_drift/config` | HA discovery JSON (retained) |
| Publish | `homeassistant/sensor/<hostname>_rtc_err/config` | HA discovery JSON (retained) |
| Publish | `homeassistant/binary_sensor/<hostname>_update/config` | HA discovery JSON (retained) |
| Publish | `homeassistant/sensor/<hostname>_update_ver/config` | HA discovery JSON (retained) |
| Publish | `homeassistant/sensor/<hostname>_{rssi,heap,uptime}/config` | HA discovery JSON (retained; health group only) |
| Publish | `homeassistant/device_automation/<hostname>_btn_{left,middle,right}/config` | HA device-trigger discovery (retained; buttons group only) |

### Notes

- **Boot-time gate** — MQTT is only started if both **Enable** is checked *and* a broker address is set. Disabling MQTT frees the task stack and stops all polling — useful if you don't use Home Assistant.
- **Optional publishing groups** — three checkboxes under **Services → MQTT** control extra telemetry: **Clock telemetry** (XTAL drift + RTC max error per NTP sync; default off), **Device health** (WiFi RSSI / free heap / uptime sensors every 60 s; default off), and **Button presses** (touch events as HA device triggers, usable in automations — e.g. "left button → toggle the bedroom lights"; default off). Publishing toggles take effect immediately; HA *entities* for a newly enabled group appear after the next broker reconnect or reboot.
- **Restart required** — changes to MQTT settings take effect after a reboot (same behaviour as weather and social counter toggles).
- **Reconnection** — the MQTT client reconnects automatically on broker restart or network interruption with a 5 s backoff. Discovery payloads are republished on every reconnect so entities reappear after a broker wipe.
- **TLS** — the current implementation uses plain `mqtt://`. If you need TLS, a reverse-proxy (e.g. nginx with stream passthrough) in front of Mosquitto is the simplest workaround for now.
- **Multiple devices** — each Nextube uses its hostname as the unique ID. Give each device a different hostname in **Network Settings** to avoid topic collisions.
- **Update check runs on-device** — independent of MQTT and of whether the web UI is ever open, the firmware itself checks GitHub for a newer release once on boot and every 24 hours after, entirely on its own. `Nextube Update Available` and `Nextube Latest Version` simply report that check's result over MQTT. This is on by default; it respects the same **Update Repo Override** (System → hidden debug panel) the web UI's own checker uses, so both always agree on which repo they're watching.

### Ticker

Publish any UTF-8 string to `nextube/<hostname>/ticker/set` to display a scrolling marquee across all 6 LCD tubes:

```
mosquitto_pub -h <broker> -t "nextube/nextube-remaster/ticker/set" -m "Good morning! Motion detected."
```

| Behaviour | Detail |
|---|---|
| **Scroll speed** | Default 4 px per 200 ms tick → 20 px/s on screen. Adjustable 1–20 via the **Nextube Ticker Speed** number entity (or `ticker_speed/set`). At the default, a typical 15-character message (~600 px wide, rendered double size) scrolls for about 75 seconds. RAM-only — resets to 4 on reboot. |
| **Cancel** | Publish an empty payload to `ticker/set` — the display returns to the previous mode immediately. |
| **New message while scrolling** | The current scroll stops and the new message starts from the right edge. |
| **Font** | `u8g2_font_logisoso28_tf` rendered with 2× pixel scaling (effective ~56 px) — full Latin character set including accented glyphs (é á ö ü ñ etc.). |
| **Background** | All tubes are blanked to solid black for the duration of the scroll, so the marquee shows on a clean background regardless of the previous mode. |
| **Colour** | Matches the currently loaded theme's digit colour. |
| **Mode rotation** | Paused for the duration of the ticker; resumes when the text finishes scrolling. |
| **Not persistent** | Ticker text is RAM-only; clears on device reboot. |
| **Notification sound** | Optional (off by default): when **Ticker Sound** is enabled, each non-empty `ticker/set` plays the configured sound file (default `/spiffs/audio/bell.wav`, changeable under **Hardware → Ticker Notification Sound**). Toggle via the HA **Nextube Ticker Sound** switch, `ticker_sound/set`, or the web UI; persists across reboots. Requires **audio output** enabled in device settings. |
| **Requires MQTT** | Ticker is only available when Home Assistant MQTT integration is enabled. |

In Home Assistant, the **Nextube Ticker** appears as a `text` entity on the Nextube device card — type your message and press Enter. The **Nextube XTAL Drift** and **Nextube RTC Max Error** telemetry sensors update after every NTP sync, giving graphable long-term history of the crystal's drift and the RTC discipline's hold accuracy.

## Follow Sun/Moon LED Mode

The accent LEDs can track the sun during the day and the moon at night instead of running a fixed animation. Select **Follow Sun/Moon** under **Display → LED Accent Lighting**. The LEDs are mostly off — only the tube(s) nearest the sun/moon's current position across the sky glow, fading smoothly as it passes between two adjacent tubes (e.g. the sun sitting exactly between tube 2 and tube 3 lights both at half intensity; as it drifts toward tube 3, tube 2 fades out while tube 3 brightens). Sun and moon each get their own configurable colour. The moon default (pale blue-white), while tThe sun default is a saturated warm yellow tuned for the LEDs specifically,.

Position is computed from the same real geocoded sunrise/sunset the WeatherLive theme uses (see [Time-of-day sky](#weatherlive-theme)), independently of which clock face is actually selected — Follow Sun/Moon LEDs work with any theme. Set a location under **Services → Weather** for accurate timing; without one, a fixed 6 AM–7 PM approximation is used instead. Moon position (and whether it's visible at all — it isn't shown near new moon) uses the same phase-based approximation as WeatherLive's on-screen moon, not a precise lunar-position calculation.

### Notes

| Behaviour | Detail |
|---|---|
| **Update rate** | Recomputed every 5 s — plenty responsive for a value that moves across the sky over hours, without the RMT burst overhead of a fast animation mode. |
| **Brightness** | Respects the normal LED Brightness and Night Brightness sliders like every other accent mode — no separate brightness setting. |
| **No location, no weather** | Falls back to a fixed 6 AM–7 PM sun window; the moon's window is still derived from that fallback and its phase. |
| **Takes effect immediately** | No reboot needed — like Static/Breath/Rainbow, switching to or from Follow Sun/Moon applies on the next tick after Save. |

---

## WLED Sync

The accent LEDs can mirror a WLED-controlled LED strip on your LAN in real time. When enabled, Nextube listens on the WLED UDP Notifier broadcast port and applies the primary colour + brightness to all 6 WS2812B accent LEDs whenever WLED changes state — no target IP or extra configuration on the WLED side.

The **Spectrum mode LCD bars** can also follow the WLED primary colour: enable **Follow WLED primary colour** next to the LCD Bar Colour picker in Spectrum settings. The bars track WLED live (changing the WLED colour recolours the bars within a second) and fall back to the configured fixed colour whenever no WLED data is available — sync disabled, no packet received yet, the strip turned off, or a palette effect that leaves WLED's primary colour black.

### Setup (Nextube side)

1. Open **Services → WLED Sync**.
2. Check **Enable** and optionally change the UDP port (default **21324**).
3. Click **Save**, then reboot the device.

### Setup (WLED side)

In the WLED app: **Config → Sync interfaces → UDP Sync → Send on direct change ✓**. That's all — WLED broadcasts to `255.255.255.255` (LAN subnet), so no Nextube IP is needed.

### Notes

| Behaviour | Detail |
|---|---|
| **Colour fidelity** | Solid effects: the exact solid colour is mirrored. Single-colour animations (Breath, Chase, Blink, etc.): the configured primary colour is shown statically — WLED UDP Notifier sends state, not per-frame pixel data. Palette-based animations (Rainbow, Fire, Ocean, Color Cycle, etc.): Nextube runs its own rainbow animation, because these effects don't publish a meaningful primary colour in the sync packet. |
| **WLED offline** | Nextube holds the last received colour indefinitely — no crash, no fallback to black. |
| **First boot** | Before any packet is received `wled_sync_get()` returns false → the accent LEDs run their normal config-driven effect (Static/Breath/Rainbow/Off). No delay or dark flash. |
| **Boot-time gate** | The UDP listener task is only created if **Enable** is checked at boot. Restart required after toggling. |
| **No TLS / HTTP** | Pure UDP receive — no `tls_sem` contention. Stack 3 KB, Core 0, priority 3. |

---

## Themes

### Built-in Themes

Ten themes ship pre-loaded in LittleFS with every firmware image. Select them from **Display → Theme** in the web UI; the dropdown is populated at runtime by scanning `/images/themes/` — any theme you upload appears there automatically.

| Theme | Style |
|---|---|
| **NixieOY** *(default)* | Warm amber Nixie-tube glyphs on a dark background — replicates classic neon vacuum tubes |
| **DarkSlate** | Cool slate-blue digits on near-black — modern minimalist |
| **FlipClock** | Split-flap flip animation — simulates the mechanical split-flap display |
| **Formula1** | High-contrast racing-style segmented digits |
| **GlitchGR** | Green-on-dark with glitch-art distortion |
| **LightFuture** | Light background, dark digits — inverted minimal look |
| **NotionRain** | Dark rain-effect styling |
| **RedDigits** | Bright red classic digit art |
| **RetroPaper** | Off-white paper tone — old VFD or e-ink feel |
| **WireMesh** | Wireframe outlined glyphs — technical circuit-board aesthetic |

All built-in themes include the complete required asset set: digits, AMPM indicators, weather icons, WeekDate images, and info-panel assets. See [Adding a Custom Theme](#adding-a-custom-theme) to install your own.

**Theme rotation** — enable **Theme Rotation** under **Display** to auto-cycle through all or a curated selection of themes on a timer. See [Theme Rotation](#theme-rotation) for details.

### WeatherLive theme

**WeatherLive** is a built-in **procedural** theme (no JPEG assets) — selecting it in the theme dropdown replaces the digit artwork with a fully animated, real-time sky rendered on the fly across all six tubes. It tracks your actual location and weather:

- **Time-of-day sky** — a dithered gradient that shifts through night → dawn → day → dusk, keyed to the **real geocoded sunrise/sunset** (±55 min twilight windows), so day length follows the season and location.
- **Sun & moon arc** — the sun arcs from sunrise to sunset; at night a moon traces the same path with the **true lunar phase** (crescent → gibbous → full) and position.
- **Twinkling stars** at night, fading in/out through twilight.
- **Weather-driven** drifting clouds, wind-blown rain/snow, and lightning flashes on thunderstorms (which also flash the LED ring when the weather-event LED override is on). Driven by the same weather data as the rest of the firmware (online source or your `POST /api/weather`).
- **Info panel(s)** on tube 5/6 honouring the **24H Custom** dual-tube layout: Day + Date (localised month abbreviation), current temperature over today's Hi/Lo range bar, and Sunrise/Sunset. When a **Custom Face** TTF font is selected the temperature, Hi/Lo, and other panel text are rendered with that font; the Hi/Lo range track, current-temperature marker, and humidity teardrop all honour the shadow setting from **Display → Custom Face**.

**Settings:** when WeatherLive is selected, a **WeatherLive scene** dropdown appears under the theme picker:
- **Realtime (animated)** — re-renders every tick (~20 Hz) for smooth motion.
- **Static (updates ~10 min)** — a still snapshot redrawn only on clock/data changes (near-zero CPU).

**Night color set:** because the sky darkens dramatically at night, a color set tuned for the day sky can be illegible after dark (and vice versa). Enable **Night color set** under **Display → Custom Face** to configure a second Font color / Digit color / Shadow set that the clock **crossfades to through twilight**, tracking the scene's real geocoded sunrise/sunset (the same signal that fades the stars in) — not the Night Brightness hour window. Colors blend smoothly over the ~55-minute twilight ramp; the shadow on/off flag flips once at mid-twilight. Only active while the WeatherLive sky is the background — static theme-image and Custom color/gradient backgrounds always use the day set. Tip: **WeatherLive Demo**'s accelerated 60-second day cycle previews the full crossfade in one minute.

**WeatherLive Demo** is a sibling theme that accelerates the day/night cycle (full day every 60 s) and auto-cycles every weather condition (10 s each) to preview the whole repertoire unattended.

Notes:
- WeatherLive renders a **subset** of the 24H Custom panels procedurally and **ignores** the Weather-icon and Pushed-image panels (those need JPEG assets / asset themes); in the web UI the Weather-icon panel option is hidden while WeatherLive is selected.
- In non-Clock modes (weather, follower counts, …) WeatherLive has no JPEG assets, so digits/symbols are drawn as procedural glyphs on a black background; the social-media platform logos still load from the built-in system assets.

### DotMatrix theme

**DotMatrix** is a second built-in **procedural** theme (no JPEG assets, like WeatherLive) — every glyph is drawn at runtime from a 7×14 dot grid, so the whole display reads like a classic scoreboard or departure-board panel instead of photo-realistic digit art.

**Settings:** when DotMatrix is selected, two colour pickers appear under the theme picker:
- **Dot colour (on)** — colour of lit dots.
- **Dot colour (off)** — colour of unlit dots. Every cell in a glyph is always painted one or the other, never left transparent, so the display always shows a full lit/unlit grid rather than glyphs floating on black.

**Coverage:**
- Clock digits, AM/PM, colon/dot/minus, and the °C/°F unit mark all render from the dot-matrix font.
- Weather-condition icons (sun, clouds, rain, snow, …), the humidity and wind panel icons, and the sunrise/sunset icons are dedicated hand-drawn dot-matrix pictograms rather than JPEG art.
- **24H Custom info panels** (tube 5/6): weekdate, indoor/outdoor temperature & humidity, wind, air quality, and sunrise/sunset all render in this theme's blocky style, each panel sharing one consistent dot pitch across its elements (larger icons are drawn at a coarser sub-grid so they don't look mismatched against smaller text) over a tiled unlit-dot background instead of a black one.
- Wind speed is labelled **kph** (not km/h), **mph**, or **m/s**.
- The Outdoor Temperature panel shows a dot-matrix progress bar between today's low and high instead of the smooth gradient track used by other themes.
- Hi/Lo and sunrise/sunset icons always use the configured **dot colour (on)** rather than the semantic red/blue or sun-crossing-horizon animation other themes use.
- This theme never applies drop shadows, regardless of the Custom Face shadow setting.

Unlike WeatherLive, DotMatrix does **not** hide the Weather Icon or Pushed Image panel options — Weather Icon uses the same dot-matrix condition pictograms as the other weather panels, and Pushed Image shows the raw pushed JPG/PNG as-is (unaffected by theme, same as any asset theme). In non-Clock modes (weather, follower counts, …) social-media platform logos still load from the built-in system assets; anything else with no dedicated dot-matrix glyph falls back to a plain unlit grid.

### Custom Face

**Custom Face** layers a TrueType font over a background of your choosing. Configure it under **Display → Custom Face**.

| Setting | Description |
|---|---|
| **Background** | **WeatherLive (animated sky)** — the fully procedural animated sky described above; **Custom color / gradient** — a solid fill or gradient you configure yourself (see below); or any installed **theme name** — that theme's static background artwork with no animation. |
| **Digit font** | Any `.ttf` uploaded to `/fonts/` on the device, or **Logisoso (built-in)**. Click **↺** to refresh the list after uploading a new font. |
| **Font colour** | Colour applied to clock digits rendered by the TTF engine. |
| **Glyph colour** | Colour for weather-panel glyphs and info-panel text rendered with the TTF engine. |
| **Shadow** | Toggle drop shadow on/off and pick its colour. Applies to clock digits, panel text, and the graphical weather elements (Hi/Lo range bar, current-temperature marker, and humidity teardrop). |
| **Night color set** | A second Font colour / Digit colour / Shadow set the display crossfades to through twilight, tracking the WeatherLive sky's real sunrise/sunset. Only active on the WeatherLive background — see the note in [WeatherLive theme](#weatherlive-theme). |

**Custom color / gradient background:** when selected, a **Fill style** dropdown and one or two colour pickers appear:

| Fill style | Result |
|---|---|
| **Solid color** | One flat colour fills the background. Only **Fill color** is shown. |
| **Linear gradient (vertical)** | Fades top → bottom between **Fill color** (start) and **Gradient end color**. |
| **Linear gradient (horizontal)** | Fades left → right between the two colours. |
| **Linear gradient (diagonal)** | Fades corner → corner between the two colours. |
| **Radial gradient** | Fades centre → edge between the two colours. |

The background is rendered once and shared identically across every tube, the same as the procedural WeatherLive sky and a static theme background — not recomputed per tube.

**How it interacts with themes:**

- **Any asset theme (NixieOY, FlipClock, etc.)** — the TTF font renders digits; all other panel assets (weather icons, social media logos, WeekDate images) continue loading from the theme.
- **WeatherLive** — the TTF font renders all panel text: temperature, Hi/Lo, sunrise/sunset, date, and humidity. The shadow setting also controls the graphical range track, temperature marker, and humidity teardrop. The procedural sky background is unaffected.

**Shadow without a custom font:** the shadow checkbox and colour operate independently of the font selection — on WeatherLive the shadow applies to the procedural graphical elements even when **Digit font → **Logisoso (built-in)** is selected.

**Time formats and 24H Custom panels** determine which info panels appear on tubes 5/6 alongside the clock digits:

| Time format | Tube layout | Info panels |
|---|---|---|
| **12H** | `[AM/PM] H H : M M` | None — all six tubes show clock elements |
| **24H** | `H H : M M [blank] [blank]` | None |
| **24H (No Sec)** | `H H : M M [info] [info]` | Tube 5 and 6 show rotating info panels |
| **24H (Custom / CX)** | `H H : M M [blank] [info]` *(single)* or `H H M M [info] [info]` *(dual)* | Fully configurable per-tube rotating panel sets |

**24H Custom info panels** (configured per tube under **Display → 24H Custom**):

| Panel | Content |
|---|---|
| Day + Date | Localised day-of-week abbreviation + date digits |
| Indoor Temp & Humidity | SHT30 sensor reading (°C/°F + %) |
| Outdoor Temp + Hi/Lo | Current outdoor temperature with today's high/low range |
| Sunrise & Sunset | Local solar times in HH:MM (NOAA algorithm, geocoded from weather city) |
| Weather Icon | Condition icon (sun, clouds, rain, snow, …) |
| Outdoor Humidity | Outdoor relative humidity (teardrop symbol + %) |
| Wind | Wind speed (wind symbol + value; km/h, mph, or m/s) |
| Air Quality | Outdoor AQI, big value colour-coded by health band — see [Air Quality panel](#air-quality-panel) |
| Pushed Image | Custom 80×160 JPG pushed via `POST /api/cx_image?tube=5\|6` — see [Pushed images on tube 5/6](#pushed-images-on-tube-56-24h-custom) |

**Dual-panel mode** (`cx_dual_panel`): drops the colon tube so the layout becomes `H H  M M [tube 5] [tube 6]`. Tubes 5 and 6 each have an independent enabled-panel set and cycle through it on the shared rotation interval.

See [Custom TTF Fonts](#custom-ttf-fonts) for font upload instructions and `pyftsubset` subsetting to reduce font file size.

### Adding a Custom Theme

The web UI theme dropdown is populated dynamically by scanning `/images/themes/` on LittleFS at runtime (via `/api/themes`). Any folder you upload there with the required file structure will appear in the dropdown automatically — no firmware update needed.

#### Option A — File browser (recommended)

Use the built-in LittleFS file browser under **System → LittleFS Files**:

1. Browse to `/images/themes/`
2. Click **📂 New Folder** → enter your theme name (e.g. `MyTheme`)
3. Navigate into `MyTheme/`, click **New Folder** → `Numbers`
4. Navigate into `Numbers/`, click **⬆ Upload File(s)** → select `0.jpg` … `9.jpg`
5. Go back to `MyTheme/` and repeat for `AMPM/` and `MutiInfo/Weather/` (and any other subfolders needed)
6. Open the **Display** tab — your theme appears in the dropdown immediately

#### Option B — Bulk folder upload

Prepare the full folder structure locally first, then go to **System → LittleFS Files**, browse to `/images/themes/`, and click **📁 Upload Folder**. Select your local theme directory. All files are uploaded in one pass with paths preserved.

### Required Folder Structure

All paths are relative to `/images/themes/{ThemeName}/`.

```
{ThemeName}/
├── Numbers/
│   ├── 0.jpg  1.jpg  2.jpg  3.jpg  4.jpg
│   └── 5.jpg  6.jpg  7.jpg  8.jpg  9.jpg
├── AMPM/
│   ├── blank.jpg        ← empty tube / suppressed digit placeholder
│   ├── dot.jpg          ← weather-waiting indicator (all 6 tubes)
│   ├── am.jpg           ← 12H clock AM indicator
│   ├── pm.jpg           ← 12H clock PM indicator
│   ├── colon.jpg        ← clock separator (tube 2)
│   ├── youtube.jpg      ← YouTube mode icon (tube 0)
│   ├── instagram.jpg    ← Instagram mode icon (tube 0)
│   ├── tiktok.jpg       ← TikTok mode icon (tube 0)
│   └── mastodon.jpg     ← Mastodon mode icon (tube 0)
│
│   * The four social media icons (youtube, instagram, tiktok, mastodon) fall
│     back to /images/system/{name}.jpg when absent from a theme — so older or
│     custom themes that predate these icons still display correctly.
│     All other missing AMPM assets render as black.
├── MutiInfo/
│   ├── Temperature/
│   │   ├── degreec.jpg  ← °C symbol
│   │   ├── degreef.jpg  ← °F symbol
│   │   └── minus.jpg    ← negative temperature sign
│   ├── Humidity/
│   │   └── humidity.jpg ← % symbol
│   └── Weather/
│       ├── sun.jpg           overcastClouds.jpg   fewClouds.jpg
│       ├── fog.jpg           rain.jpg             snow.jpg
│       ├── squalls.jpg       thunderstorm.jpg     sand.jpg
│       └── tornado.jpg       volcanicAsh.jpg
│   
└── FlipClock/            ← required only for FlipClock theme
    └── (same Numbers + AMPM sub-structure, used for split-flap animation)
```

### Image Dimensions

All tube images must be exactly **80 × 160 pixels** (portrait), saved as JPEG. The ST7735 displays are 80 × 160 px; images at any other size will display at the wrong resolution or be rejected by the size check in the display driver.

| Tube dimension | Value |
|---|---|
| Width | 80 px |
| Height | 160 px |
| Format | JPEG (any quality; 80–90 % is a good balance of size vs. artefacts) |
| Colour space | RGB (no CMYK) |

> [!TIP]
> Keep individual JPEGs under ~15 KB where possible. A full theme with all required images typically sits between 500 KB and 2 MB, well within the 7 MB LittleFS partition.

### Image Converter Helper

`helpers/image_converter/nextube_image_converter.py` is a standalone Python tool for preparing images for the device. Run it with:

```bash
python helpers/image_converter/nextube_image_converter.py
```

A browser UI opens at **http://localhost:5000**. Features:

**Regular image conversion:**
- Drag-and-drop individual images **or** click **📁 Browse Folder** to upload a whole directory at once
- Interactive crop editor with aspect-ratio-locked drag box and live 80×160 preview
- Auto center-crop and stretch modes
- JPEG or PNG output
- **Folder upload preserves the source directory structure and original filenames** in the output ZIP — ready to be decompressed and uploaded straight into the LittleFS file browser using 'upload folder'.

**`.zipper` theme import:**
- Drag-and-drop a `.zipper` file onto the drop zone (or click **📦 Import .zipper Theme**) to import a theme package created by the original Nextube desktop software
- A `.zipper` is a ZIP of PNG assets named `0.png`–`9.png`, `am.png`, `pm.png`, `blank.png`, and `colon.png`
- The filename is used as the theme name (e.g. `neon yellow.zipper` → theme `neon yellow`) — editable before converting
- Each asset is shown in a grid with a pre-computed suggested crop; click any thumbnail to fine-tune its crop individually in the editor
- Missing assets (e.g. a `.zipper` that omits `colon.png`) are listed with a warning but do not block conversion
- **Convert & Download Theme ZIP** packages all assets into the correct LittleFS folder structure:
  ```
  {ThemeName}/Numbers/0.jpg … 9.jpg
  {ThemeName}/AMPM/am.jpg  pm.jpg  blank.jpg  colon.jpg
  ```
  Unzip the downloaded file directly into `/images/themes/` via the LittleFS file browser and the theme appears in the dropdown immediately

Requires Python 3 and Pillow (`pip install Pillow` — auto-installed on first run).

## Custom TTF Fonts

The WeatherLive theme and all custom clock faces support user-supplied TrueType fonts for clock digits and panel text. Any `.ttf` file stored in `/fonts/` on the device is available for selection in the web UI under **Display → Custom Face → Digit font**.

### How it works

- The full `.ttf` file is loaded into **PSRAM** once (on start-up or when changed via the web UI). Only the characters actually displayed are rasterized — everything else costs nothing at render time.
- A 128-slot FIFO **glyph cache** in PSRAM holds pre-rasterized bitmaps so each character is decoded once per size, then reused.
- Font height is automatically **normalized** using `0` as the reference: the renderer scales the font so the digit `0` fills the target pixel height. Glyphs that are natively taller than `0` in the font file (e.g. `1` in Jim Nightshade) are automatically capped to the same height, so all digits appear visually uniform regardless of the font's internal bounding-box metrics.
- **Clock mode** renders each digit individually, centered in its own tube at full tube height.
- **Weather and WeatherLive panel text** (temperature, Hi/Lo, humidity value, wind speed, etc.) is rendered as a string that is first fitted to the tube width, then height-capped so no glyph overflows. The degree symbol (°) is appended inline as part of the temperature string and positioned naturally by the renderer rather than drawn as a separate fixed element.
- **Width overflow is handled in two passes to keep stroke weight uniform.** First, all digits `0`–`9` are scanned and if the widest one would exceed the tube width (80 px minus 5 px margin each side), the universal height scale is reduced so that every character shrinks proportionally — stroke weight stays consistent across the whole string. Second, a per-glyph fallback catches any individual character (e.g. `K`, `M`, `°`) that is still wider than the tube after the universal reduction; those are capped individually and do not affect the scale applied to other glyphs.

### Shadow

When a Custom Face font is active a **drop shadow** can be drawn around glyphs. **Digit shadow** and **Font shadow** are independent controls — each has its own On/Off toggle and colour under **Display → Custom Face** (and independent day/night variants if Night color set is enabled). Splitting them lets you, for example, run a dark digit colour with a matching dark digit shadow while keeping font/label text light with a lighter (or disabled) font shadow, without the two fighting each other. Both are a soft bloom (±2 px) behind each rendered character.

- **Digit shadow** — the clock face numerals only.
- **Font shadow** — info-panel and label text, plus the graphical weather-panel elements that are visually part of the same text:

  | Element | Shadow behaviour |
  |---|---|
  | Hi/Lo temperature range track (tube 4) | Soft underline shadow below the bar |
  | Current-temperature marker (tube 4) | Drop shadow offset below the circle |
  | Humidity teardrop (tube 5) | Halo around the teardrop outline |

Set either colour to pure black for a classic engraved look, or to a tinted hue to complement the theme palette. Turn a shadow off entirely by setting its On/Off toggle to Off — no need to fight with color values.

### Uploading a font

1. Use **System → LittleFS Files** to browse to `/fonts/` (create the folder if it does not exist).
2. Click **⬆ Upload File(s)** and select your `.ttf` file.
3. Go to **Display → Custom Face**, click the **↺** button next to the Digit font field to scan for available fonts, then choose your file from the dropdown.
4. Click **Save** — the new font takes effect on the next clock tick.

### Reducing font file size with `pyftsubset`

Full TTF files can be 200 KB – 2 MB. For the 11 supported display languages (EN/DE/FR/ES/IT/PT/NL/SV/NO/DA/FI) only a small Latin subset is actually needed. Install [fonttools](https://fonttools.readthedocs.io) and run:

```bash
pip install fonttools
```

```bash
pyftsubset MyFont.ttf \
  --unicodes="U+0020-007E,U+00A0-00FF,U+2013,U+2014,U+201C,U+201E,U+2026" \
  --output-file=MyFont-subset.ttf \
  --no-hinting \
  --layout-features=""
```

| Range | Content |
|---|---|
| `U+0020–007E` | Basic Latin — digits `0–9`, `A–Z`, `a–z`, `:` `.` `%` `-` and common punctuation |
| `U+00A0–00FF` | Latin-1 Supplement — all Western European and Nordic diacritics (ä ö ü ß å æ ø ñ ç é à °) plus `¡` `¿` |
| `U+2013` `U+2014` | En dash, em dash |
| `U+201C` `U+201E` | Typographic double quotes (used in French and German strings) |
| `U+2026` | Ellipsis `…` (used in Finnish UI strings) |

- `--no-hinting` — removes TrueType hint tables; not used by the on-device renderer and reduces file size.
- `--layout-features=""` — strips GSUB/GPOS OpenType feature tables (ligatures, kerning, etc.) which the renderer does not use.

Typical reduction: 200 KB → 30–60 KB; 1 MB → 60–150 KB.

**Batch process a folder (PowerShell):**

```powershell
Get-ChildItem *.ttf | ForEach-Object {
    pyftsubset $_.Name `
        --unicodes="U+0020-007E,U+00A0-00FF,U+2013,U+2014,U+201C,U+201E,U+2026" `
        --output-file="$($_.BaseName)-subset.ttf" `
        --no-hinting `
        --layout-features=""
}
```

**Batch process a folder (bash):**

```bash
for f in *.ttf; do
  pyftsubset "$f" \
    --unicodes="U+0020-007E,U+00A0-00FF,U+2013,U+2014,U+201C,U+201E,U+2026" \
    --output-file="${f%.ttf}-subset.ttf" \
    --no-hinting \
    --layout-features=""
done
```

> [!IMPORTANT]
> **Build dependency:** `stb_truetype.h` (a single-header C library from [github.com/nothings/stb](https://github.com/nothings/stb)) must be placed at `components/font_render/stb_truetype.h` before building. It is not bundled with the repo — copy it once after cloning and it is picked up automatically by the build system.

---

## REST API

All endpoints return JSON. The API is backward-compatible with the original firmware's endpoints and adds new ones.

**Authentication** — when auth is enabled, all mutation endpoints and any endpoint that returns secrets require a valid session. Obtain a bearer token via `/api/auth/login` and pass it as `Authorization: Bearer <token>` on every subsequent request. When auth is disabled, all tokens are accepted automatically — no header needed. `/api/status` and static file serving are always open.

```
# Auth — bootstrap (open, no token required)
POST /api/auth/set_password    → set password for first time; rejected once already set
POST /api/auth/login           → {"password":"…"} → {"token":"<64-char hex>"}

# Auth — session management (requires valid token)
GET  /api/auth/check           → 200 if token is valid, 401 if expired/missing
POST /api/auth/logout          → invalidates the current session
POST /api/auth/change_password → {"old_password":"…","new_password":"…"}
POST /api/auth/set_enabled     → {"enabled":true/false} — enable or disable auth requirement
                                  (requires valid token when disabling; open when enabling)

# WiFi setup AP (requires auth)
GET  /api/wifi/ap_pin        → {"pin":"12345678"}
POST /api/wifi/regen_pin     → generate and persist a new AP PIN → {"pin":"…"}

# Status (open)
GET  /api/ping               → {"status":"ok"}
GET  /api/status             → live status: time, wifi, weather, heap, firmware, admin_set, auth_enabled, ap_active
GET  /api/network_info       → WiFi diagnostics: disconnect_count, last_disconnect_reason, connected_duration_s, mac, bssid, channel, rssi, netmask, gateway, dns1, dns2, phy_11n

# Settings (requires auth)
GET  /api/settings           → full configuration JSON
POST /api/settings           → update config (JSON body; partial — only keys present are changed)
POST /api/weather            → push external weather data (External source); JSON: temp_c OR temp_f, humidity, condition, icon, weather_code, lat, lon (all optional)
POST /api/cx_image?tube=5|6  → push an 80×160 JPG (binary body) to a 24H Custom tube-5/6 "Pushed image" info panel (asset themes)
GET  /api/firmwareVersion    → {"version":"1.0.0"}
GET  /api/hardwareVersion    → {"version":"1.31"}
POST /api/reset              → reset settings to defaults + reboot (preserves admin password & AP PIN)
POST /api/factory_reset_full → full factory reset: clears settings + admin password + AP PIN, then reboots
POST /api/update_firmware    → OTA firmware upload (binary body, nextube-fw-ota.bin)
POST /api/update_fs          → OTA LittleFS upload (binary body, nextube-littlefs-*.bin)
POST /api/update_spiffs      → alias for /api/update_fs (backward-compatible)
GET  /api/themes             → {"themes":["ThemeA","ThemeB",...]} — scanned from LittleFS at runtime
GET  /api/file/ls?dir=/      → LittleFS directory listing
POST /api/file/rename        → rename a file or folder (JSON: {"from":"/path","to":"/newpath"})
POST /api/wifi/scan          → trigger WiFi scan
GET  /api/wifi/scan          → scan results
GET  /api/logs               → in-RAM device log (last 64 lines)
POST /api/logs/clear         → clear in-RAM log buffer
POST /api/update_notify      → {"active":true/false} — activate or clear the 4-row red update indicator on tube 6
```

**Debug / diagnostics** (all require auth when enabled; intended for troubleshooting, not automation):

```
GET  /api/debug/tasks        → per-task CPU accounting (FreeRTOS runtime stats): name, priority,
                               core, state, stack high-water mark, run_us, and pct.
                               `pct` is INSTANTANEOUS — the handler takes two snapshots ~250 ms
                               apart and diffs them, so it is correct at any uptime (an earlier
                               lifetime pct broke once the 32-bit µs counters wrapped at ~71 min).
                               pct is per-chip: both cores accrue, so the sum across all tasks ≈
                               200% (IDLE0+IDLE1 absorb the slack). The response also reports
                               `window_us` (the measured interval); `run_us` is the raw lifetime
                               counter if you want to diff it yourself. Note: the call blocks ~250 ms
                               by design. The go-to tool for diagnosing IDLE-starvation task-WDT warnings.
GET  /api/debug/adc          → single raw mic ADC reading: {"channel","gpio","raw","voltage_mv"}
POST /api/debug/dac          → inject a test signal on the audio DAC (GPIO25):
                               {"mode":"tone","freq_hz":1000,"amplitude":64} | {"mode":"dc","level":200}
                               | {"mode":"silence"} | {"mode":"hiz"} | {"mode":"normal"}
POST /api/debug/pwm          → override backlight PWM: {"freq_hz":10000,"brightness_pct":80}
                               or {"restore":true}
POST /api/debug/loglevel     → runtime per-tag log verbosity: {"tag":"ntp","level":4}
                               (0=none … 5=verbose) or {"tag":"…","enabled":false}; resets on reboot
POST /api/debug/burnin       → trigger the burn-in / colour-cycle routine (see Advanced Display)
POST /api/debug/snow         → trigger the static-snow routine (see Advanced Display)
GET  /api/debug/micframe     → atomic snapshot of one DMA capture: {"samples":[512 raw ADC values],
                               "dec":[128 decimated values]} — both arrays come from the same frame
GET  /api/debug/micbands     → per-band spectrum state for all 24 Goertzel bands:
                               raw energy, noise floor, post-floor power, and display value;
                               updated even on silence-gated frames
```

## Hardware

Reverse-engineered from PCB Rev **1.31** (2022/01/19):

| Component | Part | Pins |
|---|---|---|
| **MCU** | ESP32-WROVER-E (ESP32-D0WD-V3) | 16MB Flash, 8MB PSRAM (only 4MB assignable) |
| **Displays** | 6× ST7735 80×160 IPS | SPI: SCK=12, MOSI=13, DC=14, RST=27, BL=19(PWM) |
| | | CS: 33, 26, 21, 0, 5, 18 (left→right) |
| **LEDs** | 6× WS2812B RGB | Data=GPIO32 |
| **Touch** | 3× capacitive pads | LEFT=GPIO4(pad0), MIDDLE=GPIO2(pad2), RIGHT=GPIO15(pad3) |
| **RTC** | PCF8563 + CR1220 coin cell | I²C: SCL=22, SDA=23 (addr 0x51) |
| **Temp/Humidity** | SHT30 | I²C: SCL=22, SDA=23 (addr 0x44) |
| **Audio** | LTK8002D amplifier | DAC=GPIO25 |
| **Microphone** | Unmarked 4 mm SMT electret capsule (≈−42 dBV/Pa, see [Microphone Notes](#microphone-notes)) + LMV321IDBVR op-amp preamp | ADC=GPIO35 (ADC1_CH7, input-only) |

> **RTC battery:** The PCF8563 is backed by a **CR1220** coin cell on the underside of the PCB. Without a charged battery the RTC loses its time on every power-cycle. The firmware detects this condition (seed epoch < 2024-01-01) and blanks the clock tubes until the first NTP sync completes, rather than displaying the incorrect epoch-0 time.

> **Note on GPIO2 (MIDDLE touch):** GPIO2 is a boot-strapping pin with an internal pull-down. The ESP32 requires GPIO2 LOW to enter download mode — holding the middle touch pad during a serial flash attempt can prevent the chip entering download mode even when GPIO0 is correctly held low. This has no effect during normal operation.

### Audio / DAC Notes

#### Signal chain

```
ESP32 GPIO25 (DAC1, 8-bit)
    → [0.1 µF AC coupling cap]
    → [1 kΩ series resistor (Ri)]
    → LTK8002D Pin 3 (+IN)
    → LTK8002D internal BTL amplifier (3 W, ~3× gain)
    → Pins 5 / 8 (VO1 / VO2, bridge-tied load)
    → 4 Ω speaker
```

The LTK8002D is a **pure analog** Class-AB BTL amplifier — it has no I²S,
PDM, or any other digital audio interface. The ESP32's built-in 8-bit DAC
on GPIO25 is the only audio source.

#### DAC driver — `dac_continuous`, brought up **per-clip** (ESP-IDF v5)

The firmware uses `dac_continuous_new_channels()` from ESP-IDF v5, which
internally configures the I²S0 peripheral in **DAC mode** (`i2s_set_dac_mode`
equivalent). The I²S peripheral clocks 8-bit unsigned PCM samples from a DMA
ring buffer directly into the DAC register at **32 kHz**.

Crucially, the DAC is **not** left running between sounds. It is created and
enabled **per clip** by the playback task (`dac_restart()`), and torn down again
(`dac_teardown()`) the moment the clip — plus its fade-out — finishes. This
mirrors how the stock firmware behaved: silence means *nothing is clocked*.
Tearing down also lets the GPIO25 pad return to the isolated idle state between
clips (the decisive noise factor — see below), and avoids keeping the I²S0
engine, APLL, and a live DAC output buffer running for no benefit.

**Idle state (no clip playing), in *both* the enabled and disabled cases:**
the GPIO25 pad is **isolated** via `rtc_gpio_isolate()` — input and output
buffers off, no pulls, pad disconnected from the digital domain. This is the
exact state the stock firmware's `dac_output_disable()` (IDF 3.3.5) left the
pad in, and it is the critical detail for a silent idle: any *driven* idle
state (output LOW was tried, as was digital Hi-Z input) connects the amp's
AC-coupled input to the ESP32's digital ground/supply through the pin driver,
and every current transient on the die — the 1 kHz FreeRTOS tick, flash read
bursts, the per-second clock redraw — couples into the amp as a constant
static floor, activity hiss, and a 1 Hz tick. With the pad isolated the idle
is near-silent.
(The earlier `dac_oneshot` idle was dropped: the `dac_oneshot → dac_continuous`
transition is unreliable on the original ESP32 — the I²S0 controller does not
always release state after one-shot use, which then blocks
`dac_continuous_new_channels()` — so the firmware never mixes the two drivers.)

| Parameter | Value |
|---|---|
| Sample rate | 32 000 Hz (fixed) |
| Bit depth | 8-bit unsigned PCM (0–255) |
| Channels | Mono (DAC channel 0, GPIO25) |
| Playback operating point | **128** (= VDD/2 ≈ 1.65 V) — the centre the ring is fed around |
| Idle (between clips, either enabled or disabled) | GPIO25 pad **isolated** (`rtc_gpio_isolate` — stock firmware's idle state), no DMA, no clock |
| Per-clip fade in/out | **120 ms** cosine S-curve (`PLAY_FADE_MS`) |
| DMA buffers | 4 × 1024 bytes (allocated per clip, freed on teardown; ring is silence-primed before each clip — onset latency ≈ fade-in + 128 ms) |

#### Silence level — why 128, not 0

The coupling capacitor between the DAC output and the amplifier input charges
to the DC level of the DAC at idle. Because the cap blocks DC, the amplifier
sees `V_DAC − V_cap`. At steady state `V_cap = V_DAC_idle`, so the amplifier
input sits at 0 V differential — perfect silence — **regardless** of what the
idle DAC voltage actually is.

The pipeline uses **128** because:
- 128 ÷ 255 × 3.3 V ≈ VDD/2 — the centre of the DAC's linear range
- All WAV samples are decoded as signed 16-bit, volume-scaled, then offset by
  128 before writing to the DMA buffer
- If the idle level were anything other than 128, the coupling cap would charge
  to a different voltage, and the next sound would start from the wrong
  operating point, producing an audible pop as the cap re-centres

#### Pop prevention — per-clip fade in/out

Each time the DAC ring is brought up for a clip, the output would otherwise
step from 0 V (the DAC starts at code 0) to 128 (the playback centre). Through the coupling
cap that step looks like a DC transient — an audible thump. The firmware
prevents it at **both ends of every clip** with a **cosine S-curve fade** over
**120 ms** (`PLAY_FADE_MS`):

```c
fade[i] = (uint8_t)(64.0f * (1.0f - cosf(t * M_PI)));  // 0..128
```

- **Fade-in (0 → 128)** is written immediately after `dac_continuous_enable()`,
  before the clip's samples.
- **Fade-out (128 → 0)** is queued after the clip's last sample; the task then
  waits out the ring depth before `dac_teardown()` so the fade actually reaches
  the speaker (otherwise `del_channels()` would cut it off).

This keeps `dV/dt` low enough that the AC-coupled amp sees a gentle ramp rather
than a step at the start and end of each sound.

#### APLL cold-start delay

The first-ever call to `dac_continuous_new_channels()` after power-on triggers
ESP32 APLL lock and I²S peripheral initialisation — a one-time ~1.6 s stall.
Because the DAC is now brought up **per clip** (not at boot), this one-time lock
is paid on the **first sound played** in a boot session rather than during
startup. Subsequent clips re-create the channel quickly (APLL already locked).
When audio output is disabled, the continuous DAC / I²S0 peripheral is never
touched at all, so the APLL lock and the ~16 KB DMA ring allocation never occur.

#### LTK8002D hardware limitations

| Limitation | Detail |
|---|---|
| **No digital interface** | Analog input only — no I²S, no PDM, no volume register |
| **No software shutdown** | SD pin (pin 1) is pulled to VDD_5V via 100 kΩ — amp is permanently enabled; no ESP32 GPIO controls it |
| **Always-on noise floor** | Because the amp is always powered, it amplifies its own thermal noise and any supply noise with no audio playing |
| **Fixed gain** | ~3× (9.5 dB) set by internal resistors — not adjustable in software |

The firmware's **software volume control** (`audio_set_volume`) works by
scaling PCM sample values before writing to the DMA buffer — the amplifier
gain itself is fixed.

#### `Audio → Enable audio output` toggle

Audio output is **disabled by default**. The toggle takes effect on the **next reboot** — when the web UI saves a change to this setting it triggers a restart so the audio subsystem comes up cleanly in the chosen state.

**Boot with audio disabled (default):** `audio_init(false)` is called. The continuous DAC, I²S0 peripheral, APLL, and DMA ring are never started. The GPIO25 pad stays **isolated** (`rtc_gpio_isolate()`, applied once at the top of `app_main`) — disconnected from the digital domain, no DMA, no clock. The ~16 KB DMA heap and the ~1.6 s APLL cold-start delay are avoided entirely.

**Boot with audio enabled:** `audio_init(true)` only sets the enabled flag; the pad stays isolated. The DAC is **not** brought up at boot — it is created per clip by the playback task (`dac_restart()` → fade-in → clip → fade-out → `dac_teardown()`, which re-isolates the pad), so idle is identical to the disabled case. This is the key difference from earlier builds, which left the ring running at 128 between sounds.

**Note:** the LTK8002D itself remains powered (SD pin tied high) in both cases, so its thermal self-noise floor is still present at a very low level.

> **Why isolation beats a driven idle:** a driven-LOW pad conducts ground/supply transients into the amp input through its pull-down FET. `rtc_gpio_isolate()` removes that conduction path entirely, so idle is near-silent, matching stock.

#### Idle noise — RESOLVED

Root cause: the **GPIO25 idle drive state** was conducting the ESP32's
ground/supply transients into the amplifier. `rtc_gpio_isolate()` on GPIO25
(the stock firmware's idle pad state) removes the conduction path; idle is
now measured equivalent to stock. The mitigations below remain in place —
they reduce rail transients and per-second SPI/CPU load, which still matter
for the small capacitive residual and for general efficiency:

| Mitigation | Effect |
|---|---|
| GPIO25 pad **isolated** at idle (`rtc_gpio_isolate`, between clips, enabled *and* disabled) | No I²S/DMA/clock running, and the pad is disconnected from the digital domain — no conduction path for ground/supply transients into the amp (see toggle section) |
| DAC created/destroyed **per clip** (`dac_restart` / `dac_teardown`) | The DMA/I²S engine only runs while a sound is actually playing, then is fully torn down — matching the stock firmware's silent idle |
| `WIFI_PS_NONE` (radio always on) | MIN_MODEM was used while GPIO25 was clamped LOW, because the radio's continuous current was audible through that clamp's ground path. With the pad isolated the noise cost is gone — and MIN_MODEM's ~80% radio sleep was dropping downlink frames (MQTT `No PING_RESP` every keepalive, esp-tls `select() timeout` on handshakes), so the receiver now stays on |
| Bulk LCD pixel pushes use `spi_device_transmit()` (interrupt/DMA) instead of `spi_device_polling_transmit()` | The DMA path **yields the CPU** while each chunk clocks out, instead of busy-waiting with interrupts hot. This lowered the SPI-induced switching component of the noise floor during every clock-face/ticker redraw. Small command/parameter writes (≤8 bytes) stay on the polling path, where DMA setup would cost more than it saves |
| Colon-blink **partial push** (diff-box) | The once-per-second colon blink rewrites only the two changed colon-dot rectangles instead of repainting the whole tube — far less per-second SPI traffic (and CPU) on the shared rail |
| RMT transmissions paused during playback (`leds_set_audio_active`) | No WS2812 current spikes while a sound is playing |
| Static-mode change detection | No periodic RMT refresh when LED colour/brightness is unchanged |

**Optional hardware hardening** (no longer required for quiet idle): a
**100 µF + 100 nF** decoupling cap close to the ESP32 `VDD3P3_RTC` pin further
reduces supply transients reaching the DAC's analog domain during playback.

### Microphone Notes

#### Signal chain

```
4 mm SMT electret capsule (unmarked — see candidates below)
    → LMV321IDBVR single op-amp (SOT-23-5) — hardware preamp / bias stage
    → GPIO35 (ADC1_CH7, input-only)
    → ESP32 ADC1, 12-bit, 12 dB attenuation (0–3.3 V full scale)
```

The capsule on PCB Rev 1.31 (near the top edge of the board) carries **no markings**; identification is by package size/style only. Visual candidates: **CUI CMC-4015-25T**, **CUI CMC-4013-02S-423**, or **MIC-4013-6-G00** — dimensionally similar 4 mm SMT electrets. Measured ADC signal levels (raw-frame captures: ~±20 counts ≈ 16 mV for moderate room-level tones after the LMV321 stage) are more consistent with a standard **≈ −42 dBV/Pa** capsule than the −25 dBV/Pa the 4015-25T datasheet specifies, slightly favouring the 4013-class candidates. Functionally it does not matter: the firmware's analysis is fully relative (per-frame normalisation + adaptive noise floor), so capsule sensitivity only shifts where the floor settles. The **LMV321IDBVR** op-amp (marked `RC1F` on the PCB, located adjacent to the mic) provides the preamp stage between capsule and ADC. Both are analog-only components — no I²S or PDM interface. The correct ADC input is **GPIO35 / ADC1_CH7** (input-only pin). GPIO36 / ADC1_CH0 (`SENSOR_VP`) is **not** connected to the microphone on this hardware revision.

#### Sampling driver — `adc_continuous` (I²S0 DMA, hardware-clocked)

Capture is driven by the ESP32's **digital ADC controller via I²S0 DMA** (`adc_continuous`): the SAR is clocked in hardware at **32 kHz** (the digital controller's minimum is 20 kHz) and DMA delivers complete frames; the mic task averages every 4 samples down to the **8 kHz** analysis rate (the averaging doubles as an anti-alias filter and adds ~1 bit of effective resolution). Exact sample spacing, near-zero CPU per sample.

**Why hardware clocking is non-negotiable** (every software-timed approach was tried and failed):
- `esp_timer` + `adc_oneshot_read()` at 125 µs: each read costs **300–600 µs** (driver mutex, per-read reconfiguration), so the timer ran in permanent catch-up — the *real* sample rate was ~1.7 kHz, making every band above the true ~850 Hz Nyquist **aliased noise** (measured ~78 % of Core 0 for the privilege).
- Register-level SAR reads fixed the rate and cut CPU to ~16 %, but RTOS preemption produced **sampling jitter** (esp_timer catch-up clusters samples back-to-back, then they're treated as uniform) — a swept-tone test showed high-frequency energy smeared into the low bands and nothing detected above ~450 Hz.

**I²S0 sharing with audio:** `dac_continuous` (audio playback) uses the same peripheral. Audio claims it via `mic_set_audio_active(true)` before each clip and releases it after teardown — the spectrum freezes for the duration of a button click or alarm and resumes automatically (same pattern as the LED pause during playback).

| Parameter | Value |
|---|---|
| Hardware rate | 32 000 Hz (I²S0-clocked, DMA) ÷ 4 → 8 000 Hz effective |
| Frame size | 128 samples (16 ms per Goertzel frame, ~62 fps) |
| Bit depth | 12-bit, ADC_ATTEN_DB_12 |
| Window | Hann (suppresses off-band leakage — a loud bass note no longer splatters across all bands) |
| CPU cost | Near-zero acquisition (DMA); ~4–5 % of Core 0 for Goertzel analysis.  Capture is gated to Spectrum mode (all other modes pay zero) and httpd is pinned to Core 1 (task-WDT history).  **Note:** noise baselines captured on firmware ≤ v1.13.x were measured under the old aliased sampling — re-capture (or Reset to Auto) after updating. |

#### Frequency bands

**24 bands** are computed per frame using the **Goertzel algorithm** — far cheaper than an FFT for a fixed set of target frequencies. Each band is the **sum of all 62.5 Hz analysis bins inside its frequency range** (~60 bins across 250–3937 Hz, mapped to bands by geometric band edges), not a single bin at the band centre — a single bin only hears ±125 Hz, and the log-spaced centres above ~1 kHz sit 200–400 Hz apart, so tones between centres would fall into spectral cracks and vanish (a tone-sweep test measured literal zero at 3.2 kHz before this fix). Bands are logarithmically spaced across 280 Hz–3800 Hz (safely below the 4 kHz Nyquist limit), grouped **4 per tube** so each display shows a small 4-bar mini-spectrum. The table below lists the band centres. This layout avoids the 125–250 Hz region that attracts SPI switching harmonics and mains-frequency interference on this PCB:

| Tube | Bar 0 | Bar 1 | Bar 2 | Bar 3 | Range |
|---|---|---|---|---|---|
| 0 | 280 Hz | 315 Hz | 350 Hz | 395 Hz | Low bass |
| 1 | 440 Hz | 495 Hz | 555 Hz | 620 Hz | Upper bass |
| 2 | 695 Hz | 780 Hz | 870 Hz | 975 Hz | Lower mid |
| 3 | 1095 Hz | 1225 Hz | 1370 Hz | 1535 Hz | Midrange |
| 4 | 1720 Hz | 1925 Hz | 2160 Hz | 2420 Hz | Presence |
| 5 | 2710 Hz | 3030 Hz | 3395 Hz | 3800 Hz | Treble |

Ratio between adjacent bands ≈ 1.12× per step. The display reads left-to-right from bass (tube 0) to treble (tube 5).

#### Noise floor and peak-hold

Because the LMV321 preamp amplifies broadband electrical noise alongside audio, a **two-phase adaptive noise floor estimator** runs per band:

- **Phase 1 (first ~4 s, 250 frames):** fast convergence (α = 0.02, no signal guard) locks the per-band noise floor from zero.
- **Phase 2 (steady state):** slow drift tracking (α = 0.002) only when the current bin is below 4× the estimated floor — prevents audio signals from biasing the floor upward.

Each band's noise floor is subtracted before the peak-hold. Bars sit at zero in a quiet room without any manual tuning. A secondary **Noise Floor** threshold (`mic_silence_gate` in config, adjustable under **Display → Spectrum Mode → Noise Floor**) blanks all bands when the **sum of post-floor band power** falls below the set value — a *spectral* gate, self-calibrating because the adaptive floor has already absorbed electrical noise. Silence sums to <10; even quiet real audio exceeds 50. **Default 25; 0 disables.** *(Semantics changed: the old gate compared time-domain RMS² with a default of 250 — it was volume-calibrated rather than correctness-calibrated, blanking clearly-detected mid-frequency tones at listening level while letting loud sub-bass open the display onto nothing but speaker-distortion hash. Users upgrading should re-tune the slider, starting at 25.)*

Display dynamics: band power is smoothed with a short EMA (τ ≈ 45 ms) so tones near a band boundary hold a steady bar instead of flickering between neighbours, then peak-hold applies instant attack with exponential decay (`peak × 0.85` per frame in the mic task; a second cosmetic peak-dot layer in the display decays at 0.05/frame × 20 Hz ≈ 1 s hold). The Spectrum display task runs at **20 Hz** (50 ms tick) for snappy bar response; all other modes run at 5 Hz.

#### Spectrum LED Control

The LED ring behaviour in Spectrum mode is independently configurable under **Display → Spectrum Mode → LED Source**:

| Source | Behaviour |
|---|---|
| **Custom glow colour** (default) | Each LED is driven by the amplitude of its corresponding frequency band — loud = bright, silent = off. The colour is set by the **LED Glow Colour** picker and is the same for all LEDs. |
| **Follow accent mode** | The LEDs ignore the audio entirely and animate in whatever accent mode is configured (**Static**, **Breath**, **Rainbow**, or **Off**), using the per-tube colours from the LED settings. The LCD bars still respond to the microphone normally. |

The **LCD Bar Colour** picker is separate from the LED source and always applies — it controls the colour of the frequency bars drawn on the LCDs regardless of which LED source is selected.

### SHT30 Temperature / Humidity Sensor

The **SHT30** is fitted on the Nextube PCB and shares the I²C bus with the PCF8563 RTC.

| Parameter | Value |
|---|---|
| I²C address | 0x44 (ADDR pin low) |
| Bus | Shared with PCF8563 RTC — SCL=GPIO22, SDA=GPIO23 |
| Poll interval | Every 30 seconds (background FreeRTOS task) |
| Detection | Probed at boot via `i2c_master_probe()`; if the probe fails the driver disables itself silently |

Readings appear on the **Dashboard** under *Local Sensor* (temperature and humidity) and are included in the `/api/status` response as `sensor.temp_c` and `sensor.humidity`. The displayed temperature respects the **Units** setting (Celsius / Fahrenheit).

### Replacement LCD Panels

> [!IMPORTANT]
> **LCD replacement support requires firmware v1.8 or later.** Earlier versions lack the per-tube calibration settings (VCOM, gamma, column/row offsets, panel profile) needed to configure ST7735S replacement panels correctly.

The six original displays are **80×160 px ST7735 "Green Tab" IPS panels**. If one or more tubes fail they can be replaced with compatible ST7735S modules — the most common drop-in replacement confirmed to work with this firmware is:

 <img width="335" height="100" alt="image" src="https://github.com/user-attachments/assets/8837cff8-df31-41ae-81ee-644e3794f72e" />

Note the connector when purchasing

| Part number | Notes | Source |
|---|---|---|
| **LH096NT-IF09W** | ST7735S controller, 80×160 IPS, 0.96″, 4-pin FPC; confirmed working (Requires Invert Set)| [Alibaba listing](https://www.alibaba.com/product-detail/0-96-inch-Small-TFT-Display_1600887795945.html) |
| **LY096X1608TBBIG09C08** | ST7735S controller, 80×160 IPS, 0.96″, 4-pin FPC; confirmed working (No Invert Required) | [Alibaba listing](https://www.alibaba.com/product-detail/TFT-LCD-0-96-Inch-80X160_1600462526823.html) |

ST7735S panels are electrically identical to the original ST7735 but have a different factory register set: the default VCOM voltage and gamma curve produce washed, low-contrast colours on the Nextube PCB without calibration. The firmware's **Advanced Display** settings (see below) handle this entirely in software — no hardware modification is required.

> [!TIP]
> **LCD swap guide:** For a step-by-step video walkthrough of the physical panel replacement process, see the [community discussion thread](https://github.com/MrToast99/Nextube-Remaster/discussions/35).

#### Quick-start for LH096NT-IF09W replacement panels

1. Flash firmware, open the web UI
2. Go to **Display → Advanced Display → Panel Profile**
3. For each replaced tube set **Profile → Vivid** and **VCOM → 40** as a starting point
4. Click **Save** and evaluate — if colours are still washed raise VCOM toward 50–60; if they look over-saturated or too dark lower it toward 25–30
5. If colours remain washed even at high VCOM, raise **Gamma Correction** for that tube to **1.8–2.2**
6. If colours are inverted (white background appears black), tick **Colour Inversion** for that tube
7. If there is a 1-pixel static border on the right or bottom edge, set **Column Offset → +2** and **Row Offset → +1** for that tube
8. Click **Save** — changes take effect immediately without a reboot

> **Note on window offsets:** The LH096NT-IF09W ST7735S variant uses a frame-buffer that is 2 px wider than the visible area. Without the +2 column offset, the rightmost column of uninitialized frame-buffer appears as a thin static line. Row offset of +1 corrects the same issue at the bottom edge.

---

### Flash Layout (16MB)

| Offset | Size | Partition |
|---|---|---|
| 0x001000 | — | Bootloader |
| 0x008000 | — | Partition table |
| 0x009000 | 20K | NVS |
| 0x00E000 | 8K | OTA data |
| 0x010000 | 4.5M | app0 (OTA slot 0) |
| 0x490000 | 4.5M | app1 (OTA slot 1) |
| 0x910000 | 7M | LittleFS |

## Project Structure

```
nextube-fw/
├── .github/workflows/build.yml    # CI/CD
├── main/
│   ├── main.c                     # Application entry point + touch handler
│   ├── idf_component.yml          # Managed component dependencies (joltwallet/littlefs, etc.)
│   └── fw_version.h.in            # Version header template (processed by CMake)
├── components/
│   ├── board/include/board_pins.h # Hardware pin & display constants (incl. PIN_MIC_ADC_CHAN)
│   ├── config_mgr/                # JSON config persistence (NVS + LittleFS)
│   │   ├── CMakeLists.txt         # Injects FW_VERSION_STR / FS_VERSION_STR to all consumers
│   │   └── include/fw_version.h  # Auto-generated by CMake — do not edit manually
│   ├── display/                   # 6× ST7735 SPI display driver + mode renderer
│   ├── leds/                      # WS2812 RGB LED accent lighting task
│   ├── microphone/                # ADC mic sampling, Goertzel band analysis, peak-hold envelope
│   ├── touch/                     # Capacitive touch input (L/R = mode cycle, M = pause/resume or backlight)
│   ├── rtc/                       # PCF8563 RTC driver
│   ├── audio/                     # DAC audio playback (WAV)
│   ├── wifi_manager/              # AP+STA WiFi (WPA2 setup AP; on-demand via LEFT+RIGHT hotkey; NVS-backed per-device PIN)
│   ├── web_server/                # HTTP server + REST API + OTA handlers + log viewer
│   ├── ntp_time/                  # NTP synchronisation
│   ├── ha_mqtt/                   # Home Assistant MQTT auto-discovery + optional telemetry groups
│   ├── weather/                   # Weather client (wttr.in / Open-Meteo / OWM / Met.no)
│   ├── subscribers/               # Subscriber/follower counter (YouTube, Bilibili, Instagram, TikTok, Mastodon)
│   └── font_render/               # stb_truetype TrueType rasterizer — PSRAM glyph cache, norm_ratio height correction
│       ├── font_render.c          # Renderer implementation (stb_truetype.h must be placed here before building)
│       └── include/font_render.h  # Public API: fr_init, fr_load_face, fr_draw_glyph_centered, fr_draw_text, …
├── data/web/                      # Web UI source (bundled into LittleFS)
│   ├── index.html                 # Self-contained SPA (English inline; i18n engine built-in)
│   ├── lang/                      # Lazy-loaded translation files (de fr es it pt nl sv no da fi)
│   └── version.txt                # Auto-generated by CMake — do not edit manually
├── helpers/
│   ├── image_converter/
│   │   └── nextube_image_converter.py # Standalone Python tool: convert & crop images to 80×160 JPEG
│   └── social_relay/
│       ├── social_relay.py        # Local HTTP proxy for YouTube & TikTok counters (auto-installs Playwright)
│       └── requirements.txt       # Optional: manual pip install list for social_relay.py
├── version.json                   # Single source of truth for firmware + LittleFS version numbers
├── partitions.csv                 # Flash partition layout (LittleFS at 0x910000, 7 MB)
├── sdkconfig.defaults             # ESP-IDF SDK config overrides
└── CMakeLists.txt                 # Project build file
```

## License

MIT License. This is an independent community project with no affiliation to Rotrics.

## Community

**A big thank you to Andrew Lau** for sharing and promoting this firmware in the Nextube Facebook community — it's genuinely appreciated!

I'm not really active on Facebook, so if you have a bug report, feature request, or question, **GitHub is the best place to reach me**: open an [issue](https://github.com/MrToast99/Nextube-Remaster/issues) or start a [discussion](https://github.com/MrToast99/Nextube-Remaster/discussions) and I'll get back to you there.

## Acknowledgements

- [previoustube/previoustube](https://github.com/previoustube/previoustube) — pioneering reverse engineering of the Nextube hardware
- The original firmware strings analysis provided the complete API surface, task architecture, and peripheral configuration
