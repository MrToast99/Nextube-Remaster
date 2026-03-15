# Nextube Open-Source Firmware

[![Build](https://github.com/MrToast99/Nextube-Remaster/actions/workflows/build.yml/badge.svg)](https://github.com/MrToast99/Nextube-Remaster/actions/workflows/build.yml)

**Unofficial** open-source replacement firmware for the [Rotrics Nextube](https://www.rotrics.com/) split-flap–style digital clock, reverse-engineered from a full flash dump of the original ESP32 firmware.

## What is this?

The Nextube is a desktop clock with six small IPS LCD displays that simulate a split-flap/Nixie-tube aesthetic. The original firmware relies on Rotrics' cloud servers and a mobile app, both of which are increasingly unreliable. This project replaces it with fully self-contained firmware featuring a built-in web management UI — no apps, no cloud, no accounts.

## Features

| Feature | Status |
|---|---|
| 6× ST7735 LCD display driver | ✅ Working |
| WS2812 RGB LED effects (static/breath/rainbow) | ✅ Working |
| Capacitive touch pads (3 buttons) | ✅ Working |
| PCF8563 RTC (battery-backed) | ✅ Working |
| WiFi AP+STA with captive portal | ✅ Working |
| NTP time sync | ✅ Working |
| Built-in web management UI (SPA) | ✅ Working |
| REST API (backward-compatible + extensions) | ✅ Working |
| mDNS (`http://nextube.local`) | ✅ Working |
| OTA firmware updates via web UI | ✅ Working |
| OpenWeatherMap integration | ✅ Working |
| YouTube subscriber counter | ✅ Working |
| Bilibili follower counter | ✅ Working |
| DAC audio playback (LTK8002D amp, WAV files) | ✅ Working |
| Clock themes (Nixie/Digital/Flip art) | ✅ Working (requires theme images in SPIFFS) |
| Countdown / Pomodoro timer modes | ✅ Working |
| Album/slideshow mode | ✅ Working (place JPEGs in `/images/album/`) |
| Scoreboard mode | 🔧 Stub (displays zeros; no score input API yet) |

## Hardware

Reverse-engineered from PCB Rev **1.31** (2022/01/19):

| Component | Part | Pins |
|---|---|---|
| **MCU** | ESP32-WROVER-E | 16MB Flash, 8MB PSRAM |
| **Displays** | 6× ST7735 80×160 IPS | SPI: SCK=12, MOSI=13, DC=14, RST=27, BL=19(PWM) |
| | | CS: 33, 26, 21, 0, 5, 18 (left→right) |
| **LEDs** | 6× WS2812B RGB | Data=GPIO32 |
| **Touch** | 3× capacitive pads | GPIO2 (L), GPIO4 (M), GPIO15 (R) |
| **RTC** | PCF8563 | I²C: SCL=22, SDA=23 (addr 0x51) |
| **Audio** | LTK8002D amplifier | DAC=GPIO25 |

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

### CI Build

Every push to `main` triggers a GitHub Actions build. Tagged releases (`v*`) automatically create a GitHub Release with downloadable firmware binaries.

## Flashing

### Over-the-Air (OTA)

1. Connect to the `Nextube-Setup` WiFi network
2. Open `http://192.168.4.1` in a browser
3. Go to **System → Firmware Update**
4. Upload the `nextube-fw.bin` file

### USB Serial

```bash
# Full flash (erases everything, fresh install)
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
  write_flash 0x0 nextube-fw-full.bin

# Or individual partitions
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
  --flash_mode dio --flash_freq 40m --flash_size 16MB \
  write_flash \
  0x1000   bootloader.bin \
  0x8000   partition-table.bin \
  0x10000  nextube-fw.bin \
  0x910000 spiffs.bin
```

> **Note:** On Windows replace `/dev/ttyUSB0` with your COM port (e.g. `COM3`).

## Web Management UI

Once running, the device creates a `Nextube-Setup` WiFi AP. After connecting to your home network, access the management interface at:

- **http://nextube.local** (mDNS)
- **http://\<device-ip\>**

The web UI provides:
- **Dashboard** — live status, quick mode switching
- **Display** — theme, brightness, backlight effects, LED colours
- **Network** — WiFi config, timezone, NTP
- **Services** — weather API, YouTube/Bilibili subscriber tracking
- **Audio** — volume, sound file selection
- **System** — OTA updates, SPIFFS file browser, factory reset

## REST API

All endpoints return JSON. The API is backward-compatible with the original firmware's endpoints and adds new ones:

```
GET  /api/ping              → {"status":"ok"}
GET  /api/settings          → full configuration JSON
POST /api/settings          → update config (JSON body)
GET  /api/status            → live device status
GET  /api/firmwareVersion   → {"version":"1.0.0-oss"}
GET  /api/hardwareVersion   → {"version":"1.31"}
POST /api/reset             → factory reset + reboot
POST /api/update_firmware   → OTA upload (binary body)
GET  /api/file/ls?dir=/     → SPIFFS directory listing
POST /api/wifi/scan         → trigger WiFi scan
GET  /api/wifi/scan         → scan results
```

## Project Structure

```
nextube-fw/
├── .github/workflows/build.yml    # CI/CD
├── main/
│   └── main.c                     # Application entry point + touch handler
├── components/
│   ├── board/include/board_pins.h # Hardware pin & display constants
│   ├── config_mgr/                # JSON config persistence
│   ├── display/                   # 6× ST7735 SPI display driver + mode renderer
│   ├── leds/                      # WS2812 RGB LED effects task
│   ├── touch/                     # Capacitive touch input (L/R = mode, M = backlight)
│   ├── rtc/                       # PCF8563 RTC driver
│   ├── audio/                     # DAC audio playback (WAV)
│   ├── wifi_manager/              # AP+STA WiFi management
│   ├── web_server/                # HTTP server + REST API
│   ├── ntp_time/                  # NTP synchronisation
│   ├── weather/                   # OpenWeatherMap client
│   └── youtube_bili/              # YouTube/Bilibili API client
├── data/web/                      # Web UI (SPIFFS)
│   └── index.html                 # Self-contained SPA
├── partitions.csv                 # Flash partition layout
├── sdkconfig.defaults             # ESP-IDF SDK config
└── CMakeLists.txt                 # Project build file
```

## Contributing

This is a community reverse-engineering effort. Key areas needing help:

1. **Theme images** — Extract or recreate the Nixie/Digital/Flip digit artwork for the displays
2. **Display modes** — Complete the countdown, pomodoro, scoreboard, and album modes
3. **SHT30 sensor** — Add temperature/humidity sensor support (I²C addr 0x44)

## License

MIT License. This is an independent community project with no affiliation to Rotrics.

## Acknowledgements

- [previoustube/previoustube](https://github.com/previoustube/previoustube) — pioneering reverse engineering of the Nextube hardware
- The original firmware strings analysis provided the complete API surface, task architecture, and peripheral configuration
