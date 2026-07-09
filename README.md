# Victron Reactor HUD — Elecrow CrowPanel 1.28" Round Display

An **ESP32-S3** round display that shows a **Victron MultiPlus-II GX** system's live
telemetry over MQTT as an animated, anti-aliased sci-fi HUD. No HomeKit, no cloud,
no VRM account — everything runs on the local network.

Hardware: [Elecrow CrowPanel 1.28inch HMI ESP32 Rotary Display](https://www.elecrow.com/wiki/CrowPanel_1.28inch-HMI_ESP32_Rotary_Display.html)
(ESP32-S3, 8 MB PSRAM / 16 MB flash, GC9A01 240×240 round IPS, CST816D touch,
rotary crown + push button, 5× WS2812 RGB LEDs).

## Preview

[`preview/hud_preview.html`](preview/hud_preview.html) is a browser simulator of the
panel — open it locally to explore all five pages and the crown / knob / touch controls
without flashing hardware.

## Pages & controls

Four telemetry pages plus a settings page, all navigable from the panel:

| Page | Shows |
|------|-------|
| **Reactor** | SOC gauge ring + big SOC %, clock, battery voltage, charge status |
| **Power** | GRID / BATT / LOAD power with direction arrows |
| **Battery** | Battery voltage, SOC %, charge/discharge power, state |
| **Clock** | NTP time + date, thin SOC ring |
| **Settings** | Backlight brightness, LED mode |

| Input | Action |
|-------|--------|
| **Crown — rotate** | Switch page (and change the value when editing a setting) |
| **Button — press** | On Settings: enter edit → next item → exit |
| **Touch** | Tap / swipe-up = next page, swipe-down = previous |
| **RGB LEDs** | Off by default; Settings can set static Status or SOC-bar |

Rendering uses a 2× off-screen buffer downscaled with `pushRotateZoomWithAA` for
anti-aliased arcs, circles, and vector text.

## Setup

1. **Create your secrets file** and fill in your values:
   ```sh
   cp arduino_secrets.example.h arduino_secrets.h   # git-ignored
   ```
   Set Wi-Fi, Victron GX IP, VRM Portal ID, the `vebus` instance, and your POSIX timezone.
2. **Enable MQTT** on the Victron GX: *Settings → Services → MQTT on LAN (Plaintext)*.
3. **Verify the vebus instance** (device-specific):
   ```sh
   mosquitto_sub -h <GX_IP> -t 'N/<VRM_ID>/vebus/#' -v
   ```

## Build & flash

Espressif ESP32 core, board **ESP32S3 Dev Module**. One-time toolchain setup:

```sh
brew install arduino-cli
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index && arduino-cli core install esp32:esp32
arduino-cli lib install "LovyanGFX" "PubSubClient" "ArduinoJson" "Adafruit NeoPixel"
python3 -m pip install --user pyserial
```

Then:

```sh
./flash.sh            # compile + flash + monitor
./flash.sh compile    # compile only
```

The board has no USB-UART chip — it flashes and prints over the ESP32-S3's native
USB-Serial-JTAG. See `flash.sh` for the exact FQBN options.

## MQTT topics

```
N/<VRM_ID>/system/0/Dc/Battery/Soc | Voltage | Power
N/<VRM_ID>/vebus/<INST>/Ac/Out/P          (inverter AC output = LOAD)
N/<VRM_ID>/vebus/<INST>/Ac/ActiveIn/P     (grid AC input = GRID)
R/<VRM_ID>/keepalive                       (published every ~30 s)
```
