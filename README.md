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
  - [Web UI Language](#web-ui-language)
  - [Setup AP (WiFi Provisioning)](#setup-ap-wifi-provisioning)
  - [Advanced Display (LCD Calibration)](#advanced-display-lcd-calibration)
- [Modes](#modes)
- [Weather](#weather)
- [NTP & Clock Accuracy](#ntp--clock-accuracy)
- [Social Media Counters](#social-media-counters)
  - [Local Relay (`social_relay.py`)](#local-relay-social_relaypy)
- [Home Assistant MQTT](#home-assistant-mqtt)
- [WLED Sync](#wled-sync)
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
| PCF8563 RTC-disciplined clock (PCF slave mode — ~1 ms between NTP syncs, default) | ✅ Working |
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
| External weather (push your own data via `POST /api/weather`) | ✅ Working |
| YouTube subscriber counter (direct + relay) | ✅ Working |
| Bilibili follower counter (direct) | ✅ Working |
| Instagram follower counter (direct unofficial API) | ✅ Working |
| TikTok follower counter (via local relay) | ✅ Working |
| Mastodon follower counter (direct API) | ✅ Working |
| Local social counter relay (`social_relay.py`) | ✅ Working |
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
| Home Assistant MQTT integration (sensor + mode + display + brightness) | ✅ Working |
| WLED Sync — receive UDP Notifier broadcasts; accent LEDs follow WLED colour | ✅ Working |
| Weather Panel 3 — animated sunrise/sunset (20 Hz rising/setting sun + mountains) | ✅ Working |
| Multilingual web UI — 11 languages (EN/DE/FR/ES/IT/PT/NL/SV/NO/DA/FI) with per-browser preference | ✅ Working |
| Tube display localisation — day-of-week abbreviation in 11 languages on clock/date panels | ✅ Working |

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

#### DAC driver — `dac_continuous`, brought up **per-clip** (ESP-IDF v5)

The firmware uses `dac_continuous_new_channels()` from ESP-IDF v5, which
internally configures the I²S0 peripheral in **DAC mode** (`i2s_set_dac_mode`
equivalent). The I²S peripheral clocks 8-bit unsigned PCM samples from a DMA
ring buffer directly into the DAC register at **32 kHz**.

Crucially, the DAC is **not** left running between sounds. It is created and
enabled **per clip** by the playback task (`dac_restart()`), and torn down again
(`dac_teardown()`) the moment the clip — plus its fade-out — finishes. This
mirrors how the stock firmware behaved: silence means *nothing is clocked*. A
continuously-running DMA/I²S engine puts periodic switching activity on the
shared 3.3 V rail (audible as static); by only bringing the engine up while a
sound is actually playing, the idle is genuinely quiet.

**Idle state (no clip playing), in *both* the enabled and disabled cases:**
GPIO25 is driven as a plain **GPIO output held LOW** — no I²S, no DMA, no clock.
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
| Idle (between clips, either enabled or disabled) | GPIO25 = **OUTPUT LOW** (0 V), no DMA, no clock |
| Per-clip fade in/out | **120 ms** cosine S-curve (`PLAY_FADE_MS`) |
| DMA buffers | 8 × 2048 bytes (allocated per clip, freed on teardown) |

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
step from 0 V (the LOW idle) to 128 (the playback centre). Through the coupling
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

**Boot with audio disabled (default):** `audio_init(false)` is called. The continuous DAC, I²S0 peripheral, APLL, and DMA ring are never started. GPIO25 is reset to a plain **GPIO output held LOW** (0 V) with **no DMA and no clock**. The AC coupling cap charges to that level; thereafter the amplifier sees ~0 V AC differential — silent. Because nothing is clocked, there is no periodic rail-switching activity to couple into the amp. The ~16 KB DMA heap and the ~1.6 s APLL cold-start delay are avoided entirely.

**Boot with audio enabled:** `audio_init(true)` only sets the enabled flag and leaves GPIO25 driven LOW. The DAC is **not** brought up at boot — it is created per clip by the playback task (`dac_restart()` → fade-in → clip → fade-out → `dac_teardown()`), so idle is identical to the disabled case (GPIO LOW, nothing clocked). This is the key difference from earlier builds, which left the ring running at 128 between sounds.

**Note:** the LTK8002D itself remains powered (SD pin tied high) in both cases, so its thermal self-noise floor is still present at a low level — see *Residual noise floor* below.

> **Why level 0, not 1, when disabled:** the DAC output equals `(level / 255) × VDD`, so a non-zero idle level *scales with the supply rail*. The per-second clock-face redraw briefly droops the 3.3 V rail; at a non-zero level that droop modulates the DAC output and couples a faint 1 Hz tick into the amp. At level 0 the output is `0 × VDD = 0 V` regardless of rail voltage, so rail droop produces no DAC-path coupling.

#### Idle noise — WS2812B LEDs (~400 Hz)

The WS2812B LEDs use an **internal ~400 Hz PWM** to modulate their brightness.
This creates current pulses on the shared 3.3 V rail at 400 Hz — solidly in
the audible band — that couple through the DAC output buffer into the amplifier
input.

**Software mitigations (already implemented):**

| Mitigation | Effect |
|---|---|
| GPIO25 driven **LOW** at idle (between clips, enabled *and* disabled) | No I²S/DMA/clock running — eliminates the continuous-DMA switching noise; a 0 V output is also immune to supply-rail droop (see toggle section) |
| DAC created/destroyed **per clip** (`dac_restart` / `dac_teardown`) | The DMA/I²S engine only runs while a sound is actually playing, then is fully torn down — matching the stock firmware's silent idle |
| `WIFI_PS_MIN_MODEM` (default modem-sleep) | Restores standard WiFi modem-sleep. An earlier build forced `WIFI_PS_NONE`, which kept the radio fully powered and added a **continuous** noise-floor component; reverting to `MIN_MODEM` was the single biggest reduction in the idle floor |
| Bulk LCD pixel pushes use `spi_device_transmit()` (interrupt/DMA) instead of `spi_device_polling_transmit()` | The DMA path **yields the CPU** while each chunk clocks out, instead of busy-waiting with interrupts hot. This lowered the SPI-induced switching component of the noise floor during every clock-face/ticker redraw. Small command/parameter writes (≤8 bytes) stay on the polling path, where DMA setup would cost more than it saves |
| Colon-blink **partial push** (diff-box) | The once-per-second colon blink rewrites only the two changed colon-dot rectangles instead of repainting the whole tube — far less per-second SPI traffic (and CPU) on the shared rail |
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

Driving GPIO25 LOW (0 V) at idle — with the DAC torn down so no DMA or clock is
running — gives the quietest idle the firmware can achieve: a 0 V output that
does not track supply droop and no switching activity on the rail. The remaining
hiss is the amp's own thermal noise (and supply-coupled noise via its finite
PSRR), which is independent of the DAC input. For complete silence a hardware
modification is required:

> **Hardware mod:** Cut the SD pull-up resistor and wire the SD pin to a free
> ESP32 GPIO. `gpio_set_level(PIN_AMP_SHDN, 0)` will draw the amp's shutdown
> current to < 0.5 µA — complete silence. Define `PIN_AMP_SHDN` in
> `board_pins.h` and assert it whenever audio is disabled.

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

> ⚠️ **LCD replacement support requires firmware v1.8 or later.** Earlier versions lack the per-tube calibration settings (VCOM, gamma, column/row offsets, panel profile) needed to configure ST7735S replacement panels correctly.

The six original displays are **80×160 px ST7735 "Green Tab" IPS panels**. If one or more tubes fail they can be replaced with compatible ST7735S modules — the most common drop-in replacement confirmed to work with this firmware is:

 <img width="335" height="100" alt="image" src="https://github.com/user-attachments/assets/8837cff8-df31-41ae-81ee-644e3794f72e" />

Note the connector when purchasing

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

**Settings are preserved automatically:** before wiping the partition the firmware saves your `config.json` to NVS (a separate flash partition that is never erased by a LittleFS update). After the new image is mounted the saved config is restored and the NVS copy is deleted. You do not need to re-enter your Wi-Fi credentials, theme, brightness, or any other settings after a Web UI Update.

**Custom files are not preserved:** any themes, album images, or audio files you have uploaded to LittleFS will be erased. Back them up using the LittleFS file browser (**System → LittleFS Files**) before updating.

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
- **Display** — theme (populated dynamically from LittleFS — add a folder to `/images/themes/` and it appears automatically), brightness, LED accent lighting effects & per-tube colours, enabled mode toggles, auto mode rotation, auto theme rotation (cycle all or selected themes on a timer), Spectrum LED source (custom amplitude-modulated glow colour **or** follow configured accent mode), Spectrum LCD bar colour, Spectrum noise floor threshold, **Advanced Display** (see below)
- **Network** — WiFi SSID/password (shown as plain text on first entry so you can verify before saving; masked once a password has been saved), hostname, timezone (UTC offset in hours), NTP server. Only reconnects when credentials actually change, preserving the live connection for all other saves.
- **Services** — weather API source (wttr.in / Open-Meteo / OpenWeatherMap / Met.no), city, units, panel rotation interval, per-panel enable/disable (including animated Panel 3 Sunrise & Sunset); **Social Media Counters** (YouTube / Bilibili / Instagram / TikTok / Mastodon — see [Social Media Counters](#social-media-counters)); **WLED Sync** (see [WLED Sync](#wled-sync)); **Home Assistant MQTT** (see [Home Assistant MQTT](#home-assistant-mqtt)); countdown duration, Pomodoro work and break durations
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
| **Clock** | 12H or 24H digital clock. **24H Custom** shows rotating info panel(s) on the right-hand tube(s) (configurable under Display → 24H Custom). Available panels: Day+date, Indoor temp & humidity (SHT30), Outdoor temperature, Sunrise & Sunset times (NOAA algorithm, geocoded from weather city), Weather icon. **Single-panel (default):** `H H : M M` with one rotating panel on tube 6. **Dual-panel:** the colon is dropped (`H H  M M`) and tubes 5 **and** 6 each show an **independently-configured** rotating panel — each tube has its own enabled-panel set and cycles through it on the shared rotation interval. |
| **Date** | Date display (DD/MM/YY). Can be enabled alongside Clock — both appear as separate stops in the touch cycle. |
| **Countdown** | Configurable countdown timer. Middle touch pauses/resumes. |
| **Pomodoro** | Work/break timer with configurable work and break durations. Middle touch pauses/resumes. Automatically flips between work and break phases. |
| **YouTube** | Live subscriber count. Direct fetch or via local relay (recommended — see [Social Media Counters](#social-media-counters)). |
| **Instagram** | Live follower count. Fetched directly from Instagram's unofficial public API — no account or relay required. |
| **TikTok** | Live follower count. Requires the local relay (`social_relay.py`) — TikTok's bot detection blocks direct ESP32 fetches. |
| **Mastodon** | Live follower count. Fetched directly from the configured Mastodon instance API — no relay required. |
| **Weather** | Up to three panels cycling on a configurable interval: **Panel 1** — temperature + °C/°F + condition icon; **Panel 2** — humidity + % + condition icon; **Panel 3** — animated sunrise/sunset (rising/setting sun + mountain silhouettes at 20 Hz, solar times in HH:MM). Any combination of panels can be enabled; at least one must remain on. Temperatures rounded to whole degrees; leading zeros suppressed; minus sign shifts with digit count. All 6 tubes show `······` (dots) until the first fetch completes. |
| **Album** | Slideshow of JPEGs from `/images/album/`. Each tube shows a **different** image offset by its position — with 6+ images all tubes are unique; with fewer they wrap gracefully. Images advance as a sliding window every `album_switch_ms` (default 2 s). |
| **Spectrum** | Microphone audio visualiser. 24 Goertzel bands (280–3800 Hz, log-spaced) drive **4 segmented mini-bars per tube** with a white peak-dot indicator. Tubes read left-to-right from bass to treble. Uses the onboard CMC-4015-25T capsule + LMV321IDBVR preamp on GPIO35 (ADC1_CH7). Adaptive per-band noise floor subtraction ensures bars sit at zero in silence. **LED source**, **LED ring colour**, **LCD bar colour**, and **Noise Floor** threshold are independently configurable in **Display → Spectrum Mode**. |

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
`CC` is the two-letter [ISO 3166-1 alpha-2](https://en.wikipedia.org/wiki/ISO_3166-1_alpha-2) country code.  
Open-Meteo and Met.no: the country code is sent as `&countrycode=CC` to the geocoding API, which fetches up to five candidates and picks the first whose `country_code` field matches.  
wttr.in and OpenWeatherMap pass the whole string to their own resolvers.

**Multi-word city names:**

| Provider | Recommended format |
|---|---|
| wttr.in | Use `+` for spaces: `New+York,US`, `Los+Angeles,US` |
| Open-Meteo, Met.no | Spaces work as-is: `New York,US` |
| OpenWeatherMap | Spaces work as-is: `New York,US` |

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
ntp: NTP sync: XTAL was +1048 ms — PCF kept <=4 ms between syncs
```

The XTAL offset shows how far the ESP32 crystal drifted since the last sync — this is what the clock *would* have been without discipline. The number after the dash is the worst single-minute error the PCF slave saw over the past hour.

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

| Entity | HA domain | What it does |
|---|---|---|
| **Nextube Temperature** | `sensor` | SHT30 temperature in °C, updated every 60 s |
| **Nextube Humidity** | `sensor` | SHT30 relative humidity %, updated every 60 s |
| **Nextube Mode** | `select` | Read and set the active display mode (Clock, Weather, YouTube, …) |
| **Nextube Display** | `switch` | Turn the backlight ON or OFF (same as the middle touch button) |
| **Nextube Brightness** | `number` | LCD brightness 0–100 slider |
| **Nextube Ticker** | `text` | Scrolling message ticker — type any text and press Enter to display it across all 6 tubes |
| **Nextube Ticker Speed** | `number` | Marquee scroll speed slider, 1–20 px per tick (higher = faster). RAM-only; resets to 4 on reboot |

### Setup

1. Make sure your Home Assistant instance has an MQTT broker running (the built-in **Mosquitto** add-on works). Plain `mqtt://` (no TLS) is used.
2. Open the Nextube web UI → **Services → Home Assistant MQTT**.
3. Check **Enable** (restart required).
4. Enter your broker's **Broker** hostname or IP address (e.g. `homeassistant.local` or `192.168.1.x`).
5. Set **Port** (default `1883`).
6. Fill in **Username** and **Password** if your broker requires authentication; leave blank for anonymous access.
7. Leave **Publish HA auto-discovery payloads** checked (recommended).
8. Click **Save**, then reboot the device (**System → Reboot**).

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
| Publish | `homeassistant/sensor/<hostname>_temp/config` | HA discovery JSON (retained) |
| Publish | `homeassistant/sensor/<hostname>_hum/config` | HA discovery JSON (retained) |
| Publish | `homeassistant/select/<hostname>_mode/config` | HA discovery JSON (retained) |
| Publish | `homeassistant/switch/<hostname>_display/config` | HA discovery JSON (retained) |
| Publish | `homeassistant/number/<hostname>_brightness/config` | HA discovery JSON (retained) |
| Publish | `homeassistant/text/<hostname>_ticker/config` | HA discovery JSON (retained) |
| Publish | `homeassistant/number/<hostname>_ticker_speed/config` | HA discovery JSON (retained) |

### Notes

- **Boot-time gate** — MQTT is only started if both **Enable** is checked *and* a broker address is set. Disabling MQTT frees the task stack and stops all polling — useful if you don't use Home Assistant.
- **Restart required** — changes to MQTT settings take effect after a reboot (same behaviour as weather and social counter toggles).
- **Reconnection** — the MQTT client reconnects automatically on broker restart or network interruption with a 5 s backoff. Discovery payloads are republished on every reconnect so entities reappear after a broker wipe.
- **TLS** — the current implementation uses plain `mqtt://`. If you need TLS, a reverse-proxy (e.g. nginx with stream passthrough) in front of Mosquitto is the simplest workaround for now.
- **Multiple devices** — each Nextube uses its hostname as the unique ID. Give each device a different hostname in **Network Settings** to avoid topic collisions.

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
| **Requires MQTT** | Ticker is only available when Home Assistant MQTT integration is enabled. |

In Home Assistant, the **Nextube Ticker** appears as a `text` entity on the Nextube device card — type your message and press Enter.

## WLED Sync

The accent LEDs can mirror a WLED-controlled LED strip on your LAN in real time. When enabled, Nextube listens on the WLED UDP Notifier broadcast port and applies the primary colour + brightness to all 6 WS2812B accent LEDs whenever WLED changes state — no target IP or extra configuration on the WLED side.

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
POST /api/settings           → update config (JSON body; partial — only keys present are changed)
POST /api/weather            → push external weather data (External source); JSON: temp_c OR temp_f, humidity, condition, icon, weather_code, lat, lon (all optional)
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
│   ├── wifi_manager/              # AP+STA WiFi (WPA2 setup AP; on-demand via LEFT+RIGHT hotkey; NVS-backed per-device PIN)
│   ├── web_server/                # HTTP server + REST API + OTA handlers + log viewer
│   ├── ntp_time/                  # NTP synchronisation
│   ├── weather/                   # Weather client (wttr.in / Open-Meteo / OWM / Met.no)
│   └── subscribers/               # Subscriber/follower counter (YouTube, Bilibili, Instagram, TikTok)
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

## Contributing

This is a community reverse-engineering effort. Key areas needing help:

1. ~~**Theme images** — Extract or recreate the Nixie/Digital/Flip digit artwork for the displays~~
2. ~~**SHT30 sensor** — Add temperature/humidity sensor support (I²C addr 0x44)~~
3. **Date face** — Configuration UI for custom digit-mapped date face (mode switching already works)

## License

MIT License. This is an independent community project with no affiliation to Rotrics.

## Community

**A big thank you to Andrew Lau** for sharing and promoting this firmware in the Nextube Facebook community — it's genuinely appreciated!

I'm not really active on Facebook, so if you have a bug report, feature request, or question, **GitHub is the best place to reach me**: open an [issue](https://github.com/MrToast99/Nextube-Remaster/issues) or start a [discussion](https://github.com/MrToast99/Nextube-Remaster/discussions) and I'll get back to you there.

## Acknowledgements

- [previoustube/previoustube](https://github.com/previoustube/previoustube) — pioneering reverse engineering of the Nextube hardware
- The original firmware strings analysis provided the complete API surface, task architecture, and peripheral configuration
