# Nextube-Remaster Open-Source Firmware

[![Build](https://github.com/MrToast99/Nextube-Remaster/actions/workflows/build.yml/badge.svg)](https://github.com/MrToast99/Nextube-Remaster/actions/workflows/build.yml)

**Unofficial** open-source replacement firmware for the [Rotrics Nextube](https://www.rotrics.com/) split-flap–style digital clock, reverse-engineered from a full flash dump of the original ESP32 firmware.

[!["Buy Me A Coffee"](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/mrtoast99)

## What is this?

The Nextube is a desktop clock with six small IPS LCD displays that simulate a split-flap/Nixie-tube aesthetic. The original firmware relies on Rotrics' cloud servers and a mobile app, both of which are increasingly unreliable. This project replaces it with fully self-contained firmware featuring a built-in web management UI — no apps, no cloud, no accounts.

## Features

| Feature | Status |
|---|---|
| 6× ST7735 LCD display driver | ✅ Working |
| WS2812 RGB LED accent lighting (static/breath/rainbow) | ✅ Working |
| Capacitive touch pads (3 buttons) | ✅ Working |
| PCF8563 RTC (battery-backed) | ✅ Working |
| WiFi AP+STA with captive portal | ✅ Working |
| NTP time sync | ✅ Working |
| Built-in web management UI (SPA) | ✅ Working |
| REST API (backward-compatible + extensions) | ✅ Working |
| mDNS (`http://nextube-remaster.local`) | ✅ Working |
| OTA firmware updates via web UI | ✅ Working |
| OTA web UI / SPIFFS updates via web UI | ✅ Working |
| Firmware + SPIFFS version mismatch detection | ✅ Working |
| Weather display (temp, humidity, condition icon) | ✅ Working |
| wttr.in weather (free, no key) | ✅ Working |
| Open-Meteo weather (free, no key) | ✅ Working |
| OpenWeatherMap weather (free-tier API key) | ✅ Working |
| Met.no weather (free, no key, elevation-aware) | ✅ Working |
| YouTube subscriber counter | ✅ Working |
| Bilibili follower counter | ✅ Working |
| DAC audio playback (LTK8002D amp, WAV files) | ✅ Working |
| Clock themes (Nixie/Digital/Flip art) | ✅ Working |
| Countdown / Pomodoro timer modes | ✅ Working |
| Album/slideshow mode | ✅ Working (place JPEGs in `/images/album/`) |
| Custom Clock mode (date display) | ✅ Working |
| Per-mode enable/disable toggles | ✅ Working |
| Auto mode rotation with configurable interval | ✅ Working |
| SPIFFS file browser with upload/delete | ✅ Working |
| Scoreboard mode | 🔧 Stub (displays zeros; no score input API yet) |

## Hardware

Reverse-engineered from PCB Rev **1.31** (2022/01/19):

| Component | Part | Pins |
|---|---|---|
| **MCU** | ESP32-WROVER-E (ESP32-D0WD-V3) | 16MB Flash, 8MB PSRAM |
| **Displays** | 6× ST7735 80×160 IPS | SPI: SCK=12, MOSI=13, DC=14, RST=27, BL=19(PWM) |
| | | CS: 33, 26, 21, 0, 5, 18 (left→right) |
| **LEDs** | 6× WS2812B RGB | Data=GPIO32 |
| **Touch** | 3× capacitive pads | LEFT=GPIO4(pad0), MIDDLE=GPIO2(pad2), RIGHT=GPIO15(pad3) |
| **RTC** | PCF8563 | I²C: SCL=22, SDA=23 (addr 0x51) |
| **Audio** | LTK8002D amplifier | DAC=GPIO25 |

> **Note on GPIO2 (MIDDLE touch):** GPIO2 is a strapping pin with an internal pull-down. It functions correctly as touch pad channel 2 in normal operation but must not be held LOW during boot.

### Original Firmware Analysis

The stock firmware was built with **ESP-IDF v4.4** + **Arduino framework** via PlatformIO by developer `HERRY0812`. It uses:
- **AutoConnect** library for WiFi provisioning
- **FreeRTOS** tasks: `TaskDisplay`, `TaskWifiServer`, `TaskNtp`, `TaskWeather`, `TaskYoutubeAndBili`, `TaskIIC`, `TaskLed`, `TaskAudio`, `TaskConfigs`
- **LittleFS/SPIFFS** for theme images and config.json
- **cJSON** for configuration
- Theme images stored under `/images/themes/`
- Weather icons under `/MutiInfo/Weather/`

### Flash Layout (16MB)

| Offset | Size | Partition |
|---|---|---|
| 0x001000 | — | Bootloader |
| 0x008000 | — | Partition table |
| 0x009000 | 20K | NVS |
| 0x00E000 | 8K | OTA data |
| 0x010000 | 4.5M | app0 (OTA slot 0) |
| 0x490000 | 4.5M | app1 (OTA slot 1) |
| 0x910000 | 6.9M | SPIFFS |

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

The firmware version is defined in `version.json` at the project root:

```json
{ "version": "1.0.0", "name": "Nextube-Remaster", "description": "..." }
```

CMake reads this at configure time using a robust semver regex (`[0-9]+[.][0-9]+[.][0-9]+`). The version is injected into every component as a compiler `-D` flag via `target_compile_definitions(PUBLIC)` in `components/config_mgr/CMakeLists.txt` — this propagates to all consumers automatically through the CMake dependency graph. A `fw_version.h` header with `#ifndef` guard is also generated as a fallback. CMake also writes the version into `data/web/version.txt`, which is bundled into `spiffs.bin` for runtime mismatch detection.

To release a new version, update `version.json` and tag the commit.

### CI Build

Every push to `main` triggers a GitHub Actions build. Tagged releases (`v*`) automatically create a GitHub Release with downloadable firmware binaries.

## Flashing

### Option A — Browser-based (no tools required)

**[ESPConnect](https://thelastoutpostworkshop.github.io/ESPConnect/)** is the easiest way to flash — no Python, no drivers, no CLI. It runs entirely in the browser using the Web Serial API (Chrome / Edge only).

1. Open **https://thelastoutpostworkshop.github.io/ESPConnect/** in Chrome or Edge
2. Connect the Nextube via USB
3. Click **Connect** and select the device's COM port
4. Set baud rate to **460800**
5. Flash `nextube-fw-full.bin` at offset `0x0`

> **Note:** Web Serial requires Chrome or Edge. Firefox is not supported.

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
  0x910000 spiffs.bin
```

### Over-the-Air (OTA)

The web UI provides two separate OTA upload paths under **System**:

| Update type | File | When to use |
|---|---|---|
| **Firmware Update** | `nextube-fw-ota.bin` | New firmware, bug fixes |
| **Web UI Update** | `spiffs.bin` | New web interface, weather sources, theme changes |

> **Do not** upload `nextube-fw-full.bin` via OTA — it is the merged USB-flash image, not a valid OTA app image.

#### Version mismatch detection

After a firmware-only OTA, the web UI shows a warning banner if the SPIFFS web UI version doesn't match the new firmware version. Follow the prompt to upload the matching `spiffs.bin` via **System → Web UI Update**.

**SPIFFS update warning:** flashing `spiffs.bin` overwrites all SPIFFS flash, including any custom themes or images you have uploaded. Back up custom files using the SPIFFS file browser (System tab) before updating.

## Web Management UI

On first boot (or whenever home WiFi is not configured/unreachable) the device broadcasts a `Nextube-Setup` open WiFi AP. Connect to it and navigate to **http://192.168.4.1** to configure your network.

Once the device connects to your home network the setup AP automatically shuts down after **60 seconds** — enough time to finish loading the web UI before it disappears. If the home network is later lost the AP comes back immediately so you can always reach the device for re-configuration.

After setup, access the management interface via:

- **http://nextube-remaster.local** (mDNS — works on most platforms without knowing the IP)
- **http://\<device-ip\>** (shown on the dashboard and in your router's DHCP table)

The web UI provides:
- **Dashboard** — live status (time, mode, weather, subscribers, heap), quick mode switching
- **Display** — theme, brightness, LED accent lighting effects & per-tube colours, enabled mode toggles, auto mode rotation
- **Network** — WiFi config, timezone, NTP server
- **Services** — weather API source (wttr.in / Open-Meteo / OpenWeatherMap / Met.no), city, units, YouTube/Bilibili tracking
- **Audio** — volume, sound file selection
- **System** — firmware OTA, web UI / SPIFFS OTA, SPIFFS file browser (browse/upload/delete), device log viewer, factory reset, about

## Modes

| Mode | Description |
|---|---|
| **Clock** | 12H or 24H digital clock |
| **Custom Clock** | Date display (DD/MM/YY) |
| **Countdown** | Configurable countdown timer |
| **Pomodoro** | Work/break timer (configurable intervals) |
| **YouTube** | Live subscriber/follower count |
| **Weather** | Temperature, humidity, condition icon. Leading zeros suppressed; negative temps hide humidity. Tubes show `------` until first fetch completes. |
| **Album** | Slideshow of JPEGs from `/images/album/` |
| **Scoreboard** | Stub — displays zeros |

### Mode Rotation

Enable **Auto Rotation** in Display settings to automatically cycle through all enabled modes on a configurable interval (15 s → 1 hour). When disabled, modes only change via the Quick Actions buttons or the physical left/right touch pads. Any manual mode change resets the rotation timer.

### Touch Buttons

| Button | Action |
|---|---|
| LEFT | Previous enabled mode |
| MIDDLE | Toggle LCD displays on/off (backlight) |
| RIGHT | Next enabled mode |

## Weather

Weather mode cycles through all enabled weather APIs until one succeeds. Supported sources:

| Source | API Key | Notes |
|---|---|---|
| **wttr.in** | None | Default; city can be `Name,CC` format |
| **Open-Meteo** | None | Geocoding via Open-Meteo; strips country code automatically |
| **OpenWeatherMap** | Free-tier key | Configure at openweathermap.org |
| **Met.no** | None | Elevation-aware (fetched from geocoding API for accurate results) |

Weather display layout (6 tubes):

```
Positive:  [tens/blank] [units] [°C/°F] [hum tens/blank] [hum units] [icon]
Negative single:  [-] [units] [°C/°F] [blank] [blank] [icon]
Negative double:  [-] [tens] [units] [°C/°F] [blank] [icon]
```

Required SPIFFS image files:

```
/images/themes/{theme}/MutiInfo/Temperature/  degreec.jpg  degreef.jpg  minus.jpg
/images/themes/{theme}/MutiInfo/Weather/      sun.jpg  fewClouds.jpg  overcastClouds.jpg  fog.jpg
                                              rain.jpg  snow.jpg  squalls.jpg  thunderstorm.jpg
                                              sand.jpg  tornado.jpg  volcanicAsh.jpg
/images/themes/{theme}/AMPM/                  blank.jpg   ← used for all suppressed/blank tube slots
```

## REST API

All endpoints return JSON. The API is backward-compatible with the original firmware's endpoints and adds new ones:

```
GET  /api/ping              → {"status":"ok"}
GET  /api/settings          → full configuration JSON
POST /api/settings          → update config (JSON body)
GET  /api/status            → live status: time, wifi, weather, heap, firmware, spiffs_version
GET  /api/firmwareVersion   → {"version":"1.0.0"}
GET  /api/hardwareVersion   → {"version":"1.31"}
POST /api/reset             → factory reset + reboot
POST /api/update_firmware   → OTA firmware upload (binary body, nextube-fw-ota.bin)
POST /api/update_spiffs     → OTA SPIFFS upload (binary body, spiffs.bin)
GET  /api/file/ls?dir=/     → SPIFFS directory listing
POST /api/wifi/scan         → trigger WiFi scan
GET  /api/wifi/scan         → scan results
GET  /api/logs              → in-RAM device log (last 64 lines)
POST /api/logs/clear        → clear in-RAM log buffer
```

## Project Structure

```
nextube-fw/
├── .github/workflows/build.yml    # CI/CD
├── main/
│   ├── main.c                     # Application entry point + touch handler
│   └── fw_version.h.in            # Version header template (processed by CMake)
├── components/
│   ├── board/include/board_pins.h # Hardware pin & display constants
│   ├── config_mgr/                # JSON config persistence (NVS + SPIFFS)
│   │   ├── CMakeLists.txt         # Injects FW_VERSION_STR to all consumers
│   │   └── include/fw_version.h  # Auto-generated by CMake — do not edit manually
│   ├── display/                   # 6× ST7735 SPI display driver + mode renderer
│   ├── leds/                      # WS2812 RGB LED accent lighting task
│   ├── touch/                     # Capacitive touch input (L/R = mode cycle, M = brightness)
│   ├── rtc/                       # PCF8563 RTC driver
│   ├── audio/                     # DAC audio playback (WAV)
│   ├── wifi_manager/              # AP+STA WiFi (AP auto-disables 60 s after STA connects)
│   ├── web_server/                # HTTP server + REST API + OTA handlers + log viewer
│   ├── ntp_time/                  # NTP synchronisation
│   ├── weather/                   # Weather client (wttr.in / Open-Meteo / OWM / Met.no)
│   └── youtube_bili/              # YouTube/Bilibili API client
├── data/web/                      # Web UI source (bundled into SPIFFS)
│   ├── index.html                 # Self-contained SPA
│   └── version.txt                # Auto-generated by CMake — do not edit manually
├── version.json                   # Single source of truth for firmware version number
├── partitions.csv                 # Flash partition layout
├── sdkconfig.defaults             # ESP-IDF SDK config overrides
└── CMakeLists.txt                 # Project build file
```

## Contributing

This is a community reverse-engineering effort. Key areas needing help:

1. ~~**Theme images** — Extract or recreate the Nixie/Digital/Flip digit artwork for the displays~~
2. **Scoreboard mode** — Complete the score input API and display logic
3. **SHT30 sensor** — Add temperature/humidity sensor support (I²C addr 0x44)
4. **Custom Clock face** — Configuration UI for custom digit-mapped clock face (mode switching already works)

## License

MIT License. This is an independent community project with no affiliation to Rotrics.

## Acknowledgements

- [previoustube/previoustube](https://github.com/previoustube/previoustube) — pioneering reverse engineering of the Nextube hardware
- The original firmware strings analysis provided the complete API surface, task architecture, and peripheral configuration
