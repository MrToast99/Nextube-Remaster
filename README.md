# Nextube-Remaster Open-Source Firmware

[![Build](https://github.com/MrToast99/Nextube-Remaster/actions/workflows/build.yml/badge.svg)](https://github.com/MrToast99/Nextube-Remaster/actions/workflows/build.yml) ![GitHub Downloads (all assets, latest release)](https://img.shields.io/github/downloads/MrToast99/Nextube-Remaster/latest/total)
 ![GitHub Downloads (all assets, all releases)](https://img.shields.io/github/downloads/MrToast99/Nextube-Remaster/total?label=downloads%40total)



**Unofficial** open-source replacement firmware for the [Rotrics Nextube](https://www.rotrics.com/) split-flap–style digital clock, reverse-engineered from a full flash dump of the original ESP32 firmware.

[!["Buy Me A Coffee"](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/mrtoast99)

---

## Contents

- [What is this?](#what-is-this)
- [Features](#features)
- [Hardware](#hardware)
  - [Audio / DAC Notes](#audio--dac-notes)
  - [Microphone Notes](#microphone-notes)
  - [SHT30 Temperature / Humidity Sensor](#sht30-temperature--humidity-sensor)
  - [Replacement LCD Panels](#replacement-lcd-panels)
  - [Flash Layout](#flash-layout-16mb)
- [Building](#building)
- [Flashing](#flashing)
  - [Option A — Browser / ESPConnect](#option-a--browser-based-no-tools-required)
  - [Option B — esptool full flash](#option-b--first-time--full-flash-esptool-cli)
  - [Option C — Individual partitions](#option-c--individual-partitions-esptool-cli)
  - [Over-the-Air (OTA)](#over-the-air-ota)
    - [Automatic Update Checks](#automatic-update-checks)
    - [Returning to factory firmware](#returning-to-the-original-factory-firmware)
- [Web Management UI](#web-management-ui)
  - [Admin Authentication](#admin-authentication-optional)
  - [Setup AP (WiFi Provisioning)](#setup-ap-wifi-provisioning)
  - [Advanced Display (LCD Calibration)](#advanced-display-lcd-calibration)
- [Modes](#modes)
- [Weather](#weather)
- [Themes](#themes)
  - [Adding a Custom Theme](#adding-a-custom-theme)
  - [Image Converter Helper](#image-converter-helper)
- [REST API](#rest-api)
- [Project Structure](#project-structure)
- [Contributing](#contributing)
- [License](#license)
- [Community](#community)

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
| Spectrum mode (microphone audio visualiser — 24 Goertzel bands, 4 per tube → LED + LCD) | ✅ Working |
| Per-mode enable/disable toggles | ✅ Working |
| Auto mode rotation with configurable interval | ✅ Working |
| Auto theme rotation with configurable interval and per-theme selection | ✅ Working |
| LittleFS file browser with upload/delete/mkdir/rename | ✅ Working |
| Automatic firmware update check (compares against latest GitHub release) | ✅ Working |
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
| **RTC** | PCF8563 + CR1220 coin cell | I²C: SCL=22, SDA=23 (addr 0x51) |
| **Temp/Humidity** | SHT30 | I²C: SCL=22, SDA=23 (addr 0x44) |
| **Audio** | LTK8002D amplifier | DAC=GPIO25 |
| **Microphone** | CMC-4015-25T electret capsule + LMV321IDBVR op-amp preamp | ADC=GPIO35 (ADC1_CH7, input-only) |

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
3. `gpio_reset_pin(GPIO25)` + `GPIO_MODE_OUTPUT` + `gpio_set_level(GPIO25, 0)` drives the pin **LOW (0 V)**

Driving 0 V (rather than floating Hi-Z) clamps the amplifier's AC-coupled input at a stable, low-impedance reference (~50 Ω GPIO source resistance). The AC coupling cap charges to +VDD/2 differential within a few RC time constants and the amplifier input thereafter sees 0 V AC — near silence. A floating Hi-Z node acts as an antenna: WS2812 and SPI rail-switching transients couple into the high-impedance input and are amplified as audible hiss. **Note:** the LTK8002D itself remains powered (SD pin tied high), so its thermal self-noise floor is still present, but at a much lower level than with a floating input.

Re-enabling runs the full `dac_restart()` sequence including the boot fade.

#### Idle noise — WS2812B LEDs (~400 Hz)

The WS2812B LEDs use an **internal ~400 Hz PWM** to modulate their brightness.
This creates current pulses on the shared 3.3 V rail at 400 Hz — solidly in
the audible band — that couple through the DAC output buffer into the amplifier
input.

**Software mitigations (already implemented):**

| Mitigation | Effect |
|---|---|
| GPIO25 driven 0 V when audio disabled (`Audio → Enable audio output` unchecked) | DAC buffer powered off; 0 V clamps the amp's AC-coupled input — rail-switching noise cannot couple into the high-impedance node |
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

Even with audio output disabled, LEDs off, and LCD brightness at 0, a very
faint baseline hiss may still be audible. Root cause: the LTK8002D SD pin is
tied to VDD_5V with no GPIO control — the amp remains fully powered and
amplifies its own thermal noise (~3× gain into a 4 Ω speaker).

Driving GPIO25 LOW (0 V) when audio is disabled (see above) greatly reduces
this hiss by providing a stable, low-impedance reference to the amp input.
For complete silence a hardware modification is required:

> **Hardware mod:** Cut the SD pull-up resistor and wire the SD pin to a free
> ESP32 GPIO. `gpio_set_level(PIN_AMP_SHDN, 0)` will draw the amp's shutdown
> current to < 0.5 µA — complete silence. Define `PIN_AMP_SHDN` in
> `board_pins.h` and call it from `audio_set_enabled()` to assert shutdown
> alongside the DAC teardown.

### Microphone Notes

#### Signal chain

```
CMC-4015-25T electret capsule (top-port SMT, −25 dBV/Pa)
    → LMV321IDBVR single op-amp (SOT-23-5) — hardware preamp / bias stage
    → GPIO35 (ADC1_CH7, input-only)
    → ESP32 ADC1, 12-bit, 12 dB attenuation (0–3.3 V full scale)
```

The **CMC-4015-25T** is a CUI Devices SMT electret capsule confirmed on PCB Rev 1.31 near the top edge of the board. The **LMV321IDBVR** op-amp (marked `RC1F` on the PCB, located adjacent to the mic) provides the preamp stage between capsule and ADC. Both are analog-only components — no I²S or PDM interface. The correct ADC input is **GPIO35 / ADC1_CH7** (input-only pin). GPIO36 / ADC1_CH0 (`SENSOR_VP`) is **not** connected to the microphone on this hardware revision.

#### Sampling driver — `esp_timer` + `adc_oneshot`

`adc_continuous` is unavailable on the original ESP32 (LX6): on this target the driver uses I²S0 in DMA mode — the same peripheral occupied by `dac_continuous` for audio output. They cannot coexist, producing an `i2s controller 0 has been occupied by dac_dma` abort at boot.

A simple `esp_timer_get_time()` busy-wait at 125 µs intervals holds Core 1 at 100%, starving `IDLE1` and triggering the task watchdog within seconds.

The firmware uses an **`esp_timer` periodic timer** (125 µs period, `ESP_TIMER_TASK` dispatch) that fires the sample callback in the `esp_timer` service task (priority 22). The callback calls `adc_oneshot_read()` — safe in task context — and stores each sample into a **ping-pong `int16_t` buffer**. After `FRAME_SIZE = 128` samples (16 ms) the write buffer is flipped and a binary semaphore wakes the analysis task. Core 1 is idle between frames; no watchdog involvement.

| Parameter | Value |
|---|---|
| Sample rate | 8 000 Hz |
| Frame size | 128 samples (16 ms per Goertzel frame) |
| Bit depth | 12-bit, ADC_ATTEN_DB_12 |
| Timer overhead | ~3–8 µs per 125 µs tick ≈ 4–6 % of core time |

#### Frequency bands

**24 bands** are computed per frame using the **Goertzel algorithm** — far cheaper than an FFT for a fixed set of target frequencies. Bands are logarithmically spaced across 280 Hz–3800 Hz (safely below the 4 kHz Nyquist limit), grouped **4 per tube** so each display shows a small 4-bar mini-spectrum. This avoids the 125–250 Hz region that attracts SPI switching harmonics and mains-frequency interference on this PCB:

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

Each band's noise floor is subtracted before the peak-hold. Bars sit at zero in a quiet room without any manual tuning. A secondary **Noise Floor** threshold (`mic_silence_gate` in config, adjustable under **Display → Spectrum Mode → Noise Floor**) blanks all bands if the frame RMS² falls below a set level — useful for immediate suppression of board noise rather than waiting for the adaptive envelope to converge. The default of 250 (≈16 counts RMS) sits above ADC noise but below real audio; raise it to squelch persistent interference, lower it to catch very quiet sounds.

Peak-hold: instant attack, exponential decay (`peak × 0.85` per frame in the mic task; a second cosmetic peak-dot layer in the display decays at 0.05/frame × 20 Hz ≈ 1 s hold). The Spectrum display task runs at **20 Hz** (50 ms tick) for snappy bar response; all other modes run at 5 Hz.

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

The six original displays are **80×160 px ST7735 "Green Tab" IPS panels**. If one or more tubes fail they can be replaced with compatible ST7735S modules — the most common drop-in replacement confirmed to work with this firmware is:

| Part number | Notes | Source |
|---|---|---|
| **LH096NT-IF09W** | ST7735S controller, 80×160 IPS, 0.96″, 4-pin FPC; confirmed working | [Alibaba listing](https://www.alibaba.com/product-detail/0-96-inch-Small-TFT-Display_1600887795945.html) |

ST7735S panels are electrically identical to the original ST7735 but have a different factory register set: the default VCOM voltage and gamma curve produce washed, low-contrast colours on the Nextube PCB without calibration. The firmware's **Advanced Display** settings (see below) handle this entirely in software — no hardware modification is required.

> 📹 **LCD swap guide:** For a step-by-step video walkthrough of the physical panel replacement process, see the [community discussion thread](https://github.com/MrToast99/Nextube-Remaster/discussions/35).

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
6. Once flashing is complete, click **Disconnect** in ESPConnect to release the serial port
7. **Unplug the USB cable, wait 3 seconds, then plug it back in** (or use the power switch if you have one)

> ⚠️ **Steps 6 and 7 are required.** The Nextube cannot start its WiFi setup network while ESPConnect is still connected to the serial port. After power-cycling you will see the display light up and the `Nextube-Setup` WiFi network will appear within about 10 seconds. If the network does not appear, unplug and replug the USB cable once more.

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

#### Automatic Update Checks

The web UI automatically checks for new GitHub releases and shows a **dismissable toast notification** in the bottom-right corner if a newer version is available. The check runs once when the page loads and repeats every 24 hours while the page is open. No data leaves your network beyond the GitHub API query (`api.github.com/repos/MrToast99/Nextube-Remaster/releases/latest`).

The notification tells you which partitions changed (firmware-only, LittleFS-only, or both) so you know which files to download. Dismiss it by clicking **✕** — it won't reappear until the next page load or 24-hour interval.

**Clock-face indicator (tube 6):** when the update toast appears, the firmware can also paint a **4-row solid red bar at the physical bottom of tube 6** (the rightmost tube) on every render frame so you know an update is waiting without having the web UI open. This is opt-in — enable it under **Display → Enable clock face update notification**. The bar appears as soon as an update is detected and clears automatically when the toast is dismissed or the option is unchecked. The indicator state is RAM-only and resets on reboot (the update check re-runs on next page load).

#### Migrating from an older SPIFFS build (v1.0.10 or older, all releases past v1.1.0 are already LittleFS)

Changing the partition subtype from `spiffs` to `littlefs` requires re-flashing the partition table — a firmware-only OTA is **not** sufficient. Perform a full USB re-flash using `nextube-fw-full.bin` (Option A or B above) once, then all subsequent OTA updates work normally.

#### Returning to the original factory firmware

The release includes `Stock Recovery Firmware/full_bak-used_flash_0x0.bin` — a complete flash dump of the original Rotrics firmware taken before any modifications. **This file is provided solely as a safety net for restoring the device to its factory state.** Flash it the same way as `nextube-fw-full.bin` (Option A or B above) at offset `0x0`. Doing so will erase all Nextube-Remaster settings, themes, and configuration and put the device back exactly as it shipped from Rotrics.

## Web Management UI

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

### Setup AP (WiFi Provisioning)

The device uses a **WPA2-secured** `Nextube-Setup` network for initial WiFi provisioning. The password is an **8-digit PIN** unique to each device, generated on first boot and stored in NVS.

**Finding the PIN:**
- **LCD tubes** — while the setup AP is active and no client is connected, the PIN scrolls across the tubes as a repeating marquee: 3 blank tubes followed by all 8 digits, cycling continuously. Read the digits as they scroll past — the 3-blank gap gives your eye a clear reset point between repetitions.
- **Web UI** — once logged in, go to **System → WiFi Setup AP → Show** to display the PIN. You can also regenerate it there.
- **Serial monitor** — on first boot the PIN is logged: `Generated new AP PIN (first boot): 47391082`

**Connecting to the setup AP:**
1. The tubes display the 8-digit PIN (or find it in the serial log).
2. On your phone or laptop, connect to **Nextube-Setup** and enter the PIN when prompted.
3. Navigate to **http://192.168.4.1**.
4. Set your admin password (first boot only), then enter your home WiFi credentials under **Network**.

**AP lifecycle:**
- **No credentials saved** — AP stays up indefinitely for first-time setup.
- **Credentials saved, STA connects** — AP closes **90 seconds** after the device gets an IP, giving the browser time to finish loading the UI.
- **Credentials saved, STA fails to connect** — AP opens automatically after a **90-second** fallback timeout so you can always recover access. The device keeps retrying STA in the background.
- **STA drops after connecting** — AP comes back so you can reach the device at `192.168.4.1` to fix credentials.

After setup, access the management interface via:

- **http://nextube-remaster.local** (mDNS — works on most platforms without knowing the IP)
- **http://\<device-ip\>** (shown on the dashboard and in your router's DHCP table)

The web UI provides:
- **Dashboard** — live status (time, mode, weather, local sensor temp/humidity if SHT30 fitted, subscribers, heap), quick mode switching
- **Display** — theme (populated dynamically from LittleFS — add a folder to `/images/themes/` and it appears automatically), brightness, LED accent lighting effects & per-tube colours, enabled mode toggles, auto mode rotation, auto theme rotation (cycle all or selected themes on a timer), Spectrum LED source (custom amplitude-modulated glow colour **or** follow configured accent mode), Spectrum LCD bar colour, Spectrum noise floor threshold, **Advanced Display** (see below)
- **Network** — WiFi SSID/password (only reconnects when credentials actually change, preserving the live connection for all other saves), hostname, timezone (UTC offset in hours), NTP server
- **Services** — weather API source (wttr.in / Open-Meteo / OpenWeatherMap / Met.no), city, units, panel rotation interval, per-panel enable/disable; YouTube/Bilibili tracking; countdown duration, Pomodoro work and break durations
- **Audio** — volume, sound file selection
- **System** — firmware OTA, web UI / LittleFS OTA, LittleFS file browser (browse/upload/delete/new folder/**rename file or folder**), device log viewer, firmware update check (automatic on page load and every 24 h; compares against latest GitHub release with dismissable toast notification), **Lock Webui** (enable/disable password protection, change password, sign out), WiFi Setup AP PIN management (show/regenerate), factory reset (settings-only or full), about (shows firmware + web UI versions independently)

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
| **Clock** | 12H or 24H digital clock |
| **Date** | Date display (DD/MM/YY). Can be enabled alongside Clock — both appear as separate stops in the touch cycle. |
| **Countdown** | Configurable countdown timer. Middle touch pauses/resumes. |
| **Pomodoro** | Work/break timer with configurable work and break durations. Middle touch pauses/resumes. Automatically flips between work and break phases. |
| **YouTube** | Live subscriber/follower count |
| **Weather** | Two panels cycling on a configurable interval: **Panel 1** — temperature + °C/°F + condition icon; **Panel 2** — humidity + % + condition icon. Either panel can be disabled (but not both). Temperatures rounded to whole degrees; leading zeros suppressed; minus sign position shifts with digit count. All 6 tubes show `······` (dots) until the first fetch completes. |
| **Album** | Slideshow of JPEGs from `/images/album/`. Each tube shows a **different** image offset by its position — with 6+ images all tubes are unique; with fewer they wrap gracefully. Images advance as a sliding window every `album_switch_ms` (default 2 s). |
| **Spectrum** | Microphone audio visualiser. 24 Goertzel bands (280–3800 Hz, log-spaced) drive **4 segmented mini-bars per tube** with a white peak-dot indicator. Tubes read left-to-right from bass to treble. Uses the onboard CMC-4015-25T capsule + LMV321IDBVR preamp on GPIO35 (ADC1_CH7). Adaptive per-band noise floor subtraction ensures bars sit at zero in silence. **LED source**, **LED ring colour**, **LCD bar colour**, and **Noise Floor** threshold are independently configurable in **Display → Spectrum Mode**. |
| **Scoreboard** | Stub — displays zeros |

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

**Regular image conversion:**
- Drag-and-drop individual images **or** click **📁 Browse Folder** to upload a whole directory at once
- Interactive crop editor with aspect-ratio-locked drag box and live 80×160 preview
- Auto center-crop and stretch modes
- JPEG or PNG output
- **Folder upload preserves the source directory structure and original filenames** in the output ZIP — ready to drag straight into the LittleFS file browser

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

# Settings (requires auth)
GET  /api/settings           → full configuration JSON
POST /api/settings           → update config (JSON body)
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
│   ├── wifi_manager/              # AP+STA WiFi (WPA2 setup AP; 90 s fallback if STA fails; NVS-backed per-device PIN)
│   ├── web_server/                # HTTP server + REST API + OTA handlers + log viewer
│   ├── ntp_time/                  # NTP synchronisation
│   ├── weather/                   # Weather client (wttr.in / Open-Meteo / OWM / Met.no)
│   └── youtube_bili/              # YouTube/Bilibili API client
├── data/web/                      # Web UI source (bundled into LittleFS)
│   ├── index.html                 # Self-contained SPA
│   └── version.txt                # Auto-generated by CMake — do not edit manually
├── helpers/
│   └── nextube_image_converter.py # Standalone Python tool: convert & crop images to 80×160 JPEG
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

## Community

**A big thank you to Andrew Lau** for sharing and promoting this firmware in the Nextube Facebook community — it's genuinely appreciated!

I'm not really active on Facebook, so if you have a bug report, feature request, or question, **GitHub is the best place to reach me**: open an [issue](https://github.com/MrToast99/Nextube-Remaster/issues) or start a [discussion](https://github.com/MrToast99/Nextube-Remaster/discussions) and I'll get back to you there.

## Acknowledgements

- [previoustube/previoustube](https://github.com/previoustube/previoustube) — pioneering reverse engineering of the Nextube hardware
- The original firmware strings analysis provided the complete API surface, task architecture, and peripheral configuration
