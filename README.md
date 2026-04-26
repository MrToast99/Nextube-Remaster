# Nextube-Remaster Open-Source Firmware

[![Build](https://github.com/MrToast99/Nextube-Remaster/actions/workflows/build.yml/badge.svg)](https://github.com/MrToast99/Nextube-Remaster/actions/workflows/build.yml) ![GitHub Downloads (all assets, latest release)](https://img.shields.io/github/downloads/MrToast99/Nextube-Remaster/latest/total)
 ![GitHub Downloads (all assets, all releases)](https://img.shields.io/github/downloads/MrToast99/Nextube-Remaster/total?label=downloads%40total)



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
| OTA web UI / LittleFS updates via web UI | ✅ Working |
| Firmware + LittleFS version mismatch detection | ✅ Working |
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
| Album/slideshow mode (sliding window — each tube shows a different image) | ✅ Working |
| Date mode (date display, DD/MM/YY) | ✅ Working |
| Spectrum mode (microphone audio visualiser — 6 Goertzel bands → LED + display) | 🚧 In Progress |
| Per-mode enable/disable toggles | ✅ Working |
| Auto mode rotation with configurable interval | ✅ Working |
| LittleFS file browser with upload/delete/mkdir | ✅ Working |
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
| **Microphone** | CMEJ-0413-42-SMT-TR electret | ADC=GPIO36 (ADC1_CH0 / SENSOR_VP) |

> **Note on GPIO2 (MIDDLE touch):** GPIO2 is a strapping pin with an internal pull-down. It functions correctly as touch pad channel 2 in normal operation but must not be held LOW during boot.

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

#### DAC driver — `dac_continuous` (ESP-IDF v5)

The firmware uses `dac_continuous_new_channels()` from ESP-IDF v5, which
internally configures the I²S0 peripheral in **DAC mode** (`i2s_set_dac_mode`
equivalent). The I²S peripheral clocks 8-bit unsigned PCM samples from a DMA
ring buffer directly into the DAC register at **32 kHz**. The DAC runs
continuously at all times — there is no Hi-Z idle state between sounds.

| Parameter | Value |
|---|---|
| Sample rate | 32 000 Hz (fixed) |
| Bit depth | 8-bit unsigned PCM (0–255) |
| Channels | Mono (DAC channel 0, GPIO25) |
| Silence level | **128** (= VDD/2 ≈ 1.65 V) |
| DMA buffers | 8 × 2048 bytes |

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

#### Startup pop prevention — boot fade

When the DAC DMA ring first starts, the output transitions from 0 V (hardware
reset state) to 128 (silence). Through the coupling cap this looks like a DC
step — a large low-frequency thump. The firmware prevents this with a
**cosine S-curve fade** from 0 → 128 over **500 ms** written into the DMA
buffer immediately after `dac_continuous_enable()`:

```c
boot_fade[i] = (uint8_t)(64.0f * (1.0f - cosf(t * M_PI)));  // 0..128
```

This keeps `dV/dt` low enough that the AC-coupled amp sees only the gentle
ramp rather than a step, eliminating the startup thump.

#### APLL cold-start delay

The first-ever call to `dac_continuous_new_channels()` after power-on triggers
ESP32 APLL lock and I²S peripheral initialisation — a one-time ~1.6 s stall.
To hide this cost from the user, `audio_init()` performs a **warm-up start**
at boot (creates and immediately tears down the DAC handle) so the APLL is
already locked before the first sound is needed.

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

When the **Enable audio output** checkbox in the web UI is unchecked:

1. Any in-progress playback is stopped
2. `dac_continuous_disable()` + `dac_continuous_del_channels()` tears down the DMA ring
3. `gpio_reset_pin(GPIO25)` + `GPIO_MODE_INPUT` puts the DAC output pin into Hi-Z

This removes the DAC's active output buffer from the signal chain, breaking
the coupling path to the amplifier. **Note:** the LTK8002D itself remains
powered (SD pin tied high), so its self-noise floor is still present even with
audio output disabled — just no longer driven by the DAC.

Re-enabling runs the full `dac_restart()` sequence including the boot fade.

#### Idle noise — WS2812B LEDs (~400 Hz)

The WS2812B LEDs use an **internal ~400 Hz PWM** to modulate their brightness.
This creates current pulses on the shared 3.3 V rail at 400 Hz — solidly in
the audible band — that couple through the DAC output buffer into the amplifier
input.

**Software mitigations (already implemented):**

| Mitigation | Effect |
|---|---|
| DAC Hi-Z when audio disabled (`Audio → Enable audio output` unchecked) | DAC buffer fully powered off; coupling path removed |
| RMT transmissions paused during playback (`leds_set_audio_active`) | No WS2812 current spikes while a sound is playing |
| Static-mode change detection | No periodic RMT refresh when LED colour/brightness is unchanged |

**Why increasing PWM frequency does not help:**
- The WS2812 internal ~400 Hz PWM is inside the chip and cannot be changed in software.
- The LCD backlight PWM (`display.c`) is already at **50 kHz** — above the audible range.
- The WS2812 RMT bit clock (`leds.c`) runs at **10 MHz** — this is a fixed protocol requirement, not a noise tone.
- The interference is broadband current transients, not a single tone; moving the repetition rate higher does not reduce coupled energy.

**Permanent hardware fix:**
Place a **100 µF + 100 nF** decoupling cap as close as possible to the ESP32
`VDD3P3_RTC` pin. This absorbs the 400 Hz current spikes at the source before
they can reach the DAC buffer.

#### Residual noise floor with everything off

Even with audio output disabled (DAC Hi-Z), LEDs off, and LCD brightness at 0,
a faint baseline hiss is audible. Root cause: the LTK8002D SD pin is tied to
VDD_5V with no GPIO control. The amp remains fully powered and amplifies its
own thermal noise (~3× gain into a 4 Ω speaker). No software mitigation is
possible without a hardware modification:

> **Hardware mod:** Cut the SD pull-up resistor and wire the SD pin to a free
> ESP32 GPIO. `gpio_set_level(PIN_AMP_SHDN, 0)` will draw the amp's shutdown
> current to < 0.5 µA — complete silence. Define `PIN_AMP_SHDN` in
> `board_pins.h` and call it from `audio_set_enabled()` alongside the DAC
> Hi-Z sequence.

### Microphone Notes

Spectrum mode samples the built-in CMEJ-0413-42-SMT-TR electret condenser
microphone via GPIO36 (ADC1_CH0, `SENSOR_VP`) using the ESP-IDF `adc_oneshot`
API at **8 kHz** with 12-bit resolution and 12 dB attenuation (0–3.3 V full
scale).

Six frequency bands (125 / 250 / 500 / 1000 / 2000 / 4000 Hz) are computed
per 128-sample frame using the **Goertzel algorithm** — far cheaper than FFT
for a fixed set of target frequencies. Each band uses a peak-hold envelope
with instant attack and exponential decay (`peak × 0.85` per frame) for
smooth VU-style response.

`adc_oneshot` is used specifically to avoid I²S bus contention — the audio
driver occupies I²S0 in DAC mode, so the old I²S built-in-ADC approach is
unavailable. `esp_timer_get_time()` busy-waits provide the precise 125 µs
inter-sample spacing the FreeRTOS 1 ms tick cannot achieve.

### Original Firmware Analysis

The stock firmware was built with **ESP-IDF v4.4** + **Arduino framework** via PlatformIO by developer `HERRY0812`. It uses:
- **AutoConnect** library for WiFi provisioning
- **FreeRTOS** tasks: `TaskDisplay`, `TaskWifiServer`, `TaskNtp`, `TaskWeather`, `TaskYoutubeAndBili`, `TaskIIC`, `TaskLed`, `TaskAudio`, `TaskConfigs`
- **SPIFFS** for theme images and config.json
- **cJSON** for configuration
- Theme images stored under `/images/themes/`
- Weather icons under `/MutiInfo/Weather/`

> **This project has migrated from SPIFFS to LittleFS.** The original firmware used SPIFFS (partition subtype `0x82`). Nextube-Remaster uses LittleFS (`joltwallet/littlefs`, subtype `0x83`) to gain real directory support, power-loss resilience (copy-on-write), and removal of the 64-character filename limit. The VFS mount point is kept as `/spiffs` so all existing path strings in the firmware, config, and web UI are unchanged. A one-time full USB re-flash is required when upgrading from any SPIFFS-based build because the partition table subtype must change.

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

> **Note:** Web Serial requires Chrome or Edge. Firefox is not supported. Use USB-A to USB-C, C-C cables don't seem to work.

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

The web UI provides two separate OTA upload paths under **System**:

| Update type | File | When to use |
|---|---|---|
| **Firmware Update** | `nextube-fw-v{ver}-ota.bin` | New firmware, bug fixes |
| **Web UI Update** | `nextube-littlefs-v{ver}.bin` | New web interface, weather sources, theme changes |

> **Do not** upload `nextube-fw-full.bin` via OTA — it is the merged USB-flash image, not a valid OTA app image.

#### Version mismatch detection

After a firmware-only OTA, the web UI shows a warning banner if the LittleFS web UI version doesn't match the new firmware's expected version. Follow the prompt to upload the matching `littlefs.bin` via **System → Web UI Update**.

**LittleFS update warning:** flashing `littlefs.bin` overwrites all LittleFS flash, including any custom themes or images you have uploaded. Back up custom files using the LittleFS file browser (**System → LittleFS Files**) before updating.

#### Migrating from an older SPIFFS build

Changing the partition subtype from `spiffs` to `littlefs` requires re-flashing the partition table — a firmware-only OTA is **not** sufficient. Perform a full USB re-flash using `nextube-fw-full.bin` (Option A or B above) once, then all subsequent OTA updates work normally.

## Web Management UI

On first boot (or whenever home WiFi is not configured/unreachable) the device broadcasts a `Nextube-Setup` open WiFi AP. Connect to it and navigate to **http://192.168.4.1** to configure your network.

**AP lifecycle:**
- **No credentials saved** — AP stays open indefinitely for first-time setup.
- **Credentials saved, STA connects** — AP shuts down **60 seconds** after the device gets an IP (gives the browser time to finish loading the UI).
- **Credentials saved, STA never connects** — AP closes automatically after **3 minutes** so the device doesn't broadcast `Nextube-Setup` indefinitely in a deployed environment. The device keeps retrying STA silently in the background.
- **STA drops after connecting** — AP comes back immediately so you can always reach the device at `192.168.4.1` to fix credentials.
- **New credentials saved via UI** — AP reappears and a fresh 3-minute window starts while the device tries the new credentials.

After setup, access the management interface via:

- **http://nextube-remaster.local** (mDNS — works on most platforms without knowing the IP)
- **http://\<device-ip\>** (shown on the dashboard and in your router's DHCP table)

The web UI provides:
- **Dashboard** — live status (time, mode, weather, local sensor temp/humidity if SHT30 fitted, subscribers, heap), quick mode switching
- **Display** — theme (populated dynamically from LittleFS — add a folder to `/images/themes/` and it appears automatically), brightness, LED accent lighting effects & per-tube colours, enabled mode toggles, auto mode rotation, Spectrum colour
- **Network** — WiFi SSID/password (only reconnects when credentials actually change, preserving the live connection for all other saves), hostname, timezone (UTC offset in hours), NTP server
- **Services** — weather API source (wttr.in / Open-Meteo / OpenWeatherMap / Met.no), city, units, panel rotation interval, per-panel enable/disable; YouTube/Bilibili tracking; countdown duration, Pomodoro work and break durations
- **Audio** — volume, sound file selection
- **System** — firmware OTA, web UI / LittleFS OTA, LittleFS file browser (browse/upload/delete/new folder), device log viewer, firmware update check (compares against latest GitHub release), factory reset, about (shows firmware + web UI versions independently)

## Modes

| Mode | Description |
|---|---|
| **Clock** | 12H or 24H digital clock |
| **Date** | Date display (DD/MM/YY). Can be enabled alongside Clock — both appear as separate stops in the touch cycle. |
| **Countdown** | Configurable countdown timer. Middle touch pauses/resumes. |
| **Pomodoro** | Work/break timer with configurable work and break durations. Middle touch pauses/resumes. Automatically flips between work and break phases. |
| **YouTube** | Live subscriber/follower count |
| **Weather** | Two panels cycling on a configurable interval: **Panel 1** — temperature + °C/°F + condition icon; **Panel 2** — humidity + % + condition icon. Either panel can be disabled (but not both). Temperatures rounded to whole degrees; leading zeros suppressed; minus sign position shifts with digit count. All 6 tubes show `······` (dots) until the first fetch completes. |
| **Album** | Slideshow of JPEGs from `/images/album/`. Each tube shows a **different** image offset by its position — with 6+ images all tubes are unique; with fewer they wrap gracefully. Images advance as a sliding window every `album_switch_ms` (default 2 s). |
| **Spectrum** 🚧 | Microphone audio visualiser. Six Goertzel frequency bands (125 / 250 / 500 / 1000 / 2000 / 4000 Hz) drive both the LED accent lighting and the tube displays (bar-graph 0–9). Requires the onboard CMEJ-0413-42-SMT-TR microphone (GPIO36). Colour is configurable in **Display → Spectrum Colour**. |
| **Scoreboard** | Stub — displays zeros |

### Mode Rotation

Enable **Auto Rotation** in Display settings to automatically cycle through all enabled modes on a configurable interval (15 s → 1 hour). When disabled, modes only change via the Quick Actions buttons or the physical left/right touch pads. Any manual mode change resets the rotation timer.

### Touch Buttons

| Button | Action |
|---|---|
| LEFT | Previous enabled mode |
| MIDDLE | **Countdown / Pomodoro:** pause / resume the timer. **All other modes:** toggle LCD displays on/off (backlight) |
| RIGHT | Next enabled mode |

## Weather

Weather mode cycles through all enabled weather APIs until one succeeds. Supported sources:

| Source | API Key | Notes |
|---|---|---|
| **wttr.in** | None | City can be `Name,CC` format |
| **Open-Meteo** | None | Geocoding via Open-Meteo; strips country code automatically |
| **OpenWeatherMap** | Free-tier key | Configure at openweathermap.org |
| **Met.no** | None | **Default.** Elevation-aware (fetched from geocoding API for accurate results) |

Weather fetching: On WiFi connect the first fetch happens immediately with automatic 5-second retries until data arrives. After the first successful fetch, weather is refreshed every 10 minutes.

Weather mode auto-cycles between two panels on a configurable interval (default 5 s). Either panel can be disabled in **Services → Weather → Display Panels**, but not both simultaneously.

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

## Themes

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
│   ├── countdown.jpg    ← countdown mode label (tube 0)
│   ├── pomodoro.jpg     ← pomodoro mode label (tube 0)
│   ├── pomodorosb.jpg   ← pomodoro work-session indicator (tube 5)
│   ├── pomodorolb.jpg   ← pomodoro break indicator (tube 5)
│   └── youtube.jpg      ← YouTube mode label (tube 0)
├── MutiInfo/
│   ├── Temperature/
│   │   ├── degreec.jpg  ← °C symbol
│   │   ├── degreef.jpg  ← °F symbol
│   │   └── minus.jpg    ← negative temperature sign
│   ├── Humidity/
│   │   └── humidity.jpg ← % symbol
│   ├── Weather/
│   │   ├── sun.jpg           overcastClouds.jpg   fewClouds.jpg
│   │   ├── fog.jpg           rain.jpg             snow.jpg
│   │   ├── squalls.jpg       thunderstorm.jpg     sand.jpg
│   │   └── tornado.jpg       volcanicAsh.jpg
│   └── WeekDate/
│       ├── week/
│       │   └── sunday.jpg  monday.jpg  tuesday.jpg  wednesday.jpg
│       │       thursday.jpg  friday.jpg  saturday.jpg
│       └── date/
│           └── 0.jpg  1.jpg  2.jpg  3.jpg  4.jpg
│               5.jpg  6.jpg  7.jpg  8.jpg  9.jpg
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

> **Tip:** Keep individual JPEGs under ~15 KB where possible. A full theme with all required images typically sits between 500 KB and 2 MB, well within the 7 MB LittleFS partition.

### Image Converter Helper

`helpers/nextube_image_converter.py` is a standalone Python tool for preparing images for the device. Run it with:

```bash
python helpers/nextube_image_converter.py
```

A browser UI opens at **http://localhost:5000**. Features:
- Drag-and-drop individual images **or** click **📁 Browse Folder** to upload a whole directory at once
- Interactive crop editor with aspect-ratio-locked drag box and live 80×160 preview
- Auto center-crop and stretch modes
- JPEG or PNG output
- **Folder upload preserves the source directory structure and original filenames** in the output ZIP — ready to drag straight into the LittleFS file browser

Requires Python 3 and Pillow (`pip install Pillow` — auto-installed on first run).

## REST API

All endpoints return JSON. The API is backward-compatible with the original firmware's endpoints and adds new ones:

```
GET  /api/ping              → {"status":"ok"}
GET  /api/settings          → full configuration JSON
POST /api/settings          → update config (JSON body)
GET  /api/status            → live status: time, wifi, weather, heap, firmware, fs_version
GET  /api/firmwareVersion   → {"version":"1.0.0"}
GET  /api/hardwareVersion   → {"version":"1.31"}
POST /api/reset             → factory reset + reboot
POST /api/update_firmware   → OTA firmware upload (binary body, nextube-fw-ota.bin)
POST /api/update_fs         → OTA LittleFS upload (binary body, nextube-littlefs-*.bin)
POST /api/update_spiffs     → alias for /api/update_fs (backward-compatible)
GET  /api/themes            → {"themes":["ThemeA","ThemeB",...]} — scanned from LittleFS at runtime
GET  /api/file/ls?dir=/     → LittleFS directory listing
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
│   ├── wifi_manager/              # AP+STA WiFi (3 min boot timeout if STA fails; 60 s graceful shutdown after connect)
│   ├── web_server/                # HTTP server + REST API + OTA handlers + log viewer
│   ├── ntp_time/                  # NTP synchronisation
│   ├── weather/                   # Weather client (wttr.in / Open-Meteo / OWM / Met.no)
│   └── youtube_bili/              # YouTube/Bilibili API client
├── data/web/                      # Web UI source (bundled into LittleFS)
│   ├── index.html                 # Self-contained SPA
│   └── version.txt                # Auto-generated by CMake — do not edit manually
├── helpers/
│   └── nextube_image_converter.py # Standalone Python tool: convert & crop images to 80×160 JPEG/PNG
├── version.json                   # Single source of truth for firmware + LittleFS version numbers
├── partitions.csv                 # Flash partition layout (LittleFS at 0x910000, 7 MB)
├── sdkconfig.defaults             # ESP-IDF SDK config overrides
└── CMakeLists.txt                 # Project build file
```

## Contributing

This is a community reverse-engineering effort. Key areas needing help:

1. ~~**Theme images** — Extract or recreate the Nixie/Digital/Flip digit artwork for the displays~~
2. ~~**SHT30 sensor** — Add temperature/humidity sensor support (I²C addr 0x44)~~
3. **Scoreboard mode** — Complete the score input API and display logic
4. **Date face** — Configuration UI for custom digit-mapped date face (mode switching already works)

## License

MIT License. This is an independent community project with no affiliation to Rotrics.

## Acknowledgements

- [previoustube/previoustube](https://github.com/previoustube/previoustube) — pioneering reverse engineering of the Nextube hardware
- The original firmware strings analysis provided the complete API surface, task architecture, and peripheral configuration
