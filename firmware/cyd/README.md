# ESP-o-nitor - CYD firmware

The **CYD variant** of [ESP-o-nitor](../../): firmware for a **CYD** (Cheap Yellow Display,
ESP32-2432S028R) with a 320x240 color ILI9341 screen that acts as a physical usage monitor for
Claude and Codex.

Unlike the [OLED variant](../oled/), which projects the framebuffers the service renders, the CYD
**draws in color on-device** from the JSON snapshot exposed by the
[monitor-service](../../monitor-service/). Both variants use the same service, unchanged.

## Architecture

```
[monitor-service (PC/Docker)] --HTTP JSON--> [CYD/ESP32] --SPI--> ILI9341 320x240
      computes quota/activity    /api/esp/*     draws       Claude (top) + Codex (bottom)
```

- Polls `GET /api/esp/snapshot` every `pollIntervalMs` and draws both panels.
- Uses `GET /api/esp/activity-wait` (non-blocking long-poll) to react almost instantly to the
  busy / waiting / idle states and animate each tool's activity indicator.
- Only redraws when the data changes (signature comparison); the activity spinner refreshes just
  its own box so the rest of the screen does not flicker.
- If the service does not respond it shows "No service"; if there is no WiFi it brings up a
  configuration AP.

Sending ~1 KB of JSON per poll instead of full framebuffers keeps the wire cheap, exploits the
color panel, and needs no changes on the service side.

## What it shows

For each tool (Claude / Codex), on its half of the screen:

- Icon + name and an activity indicator (BUSY with spinner / WAIT blinking / IDLE).
- The **5H** (current) and **WK** (weekly) windows, each with:
  - Remaining percentage and the delta against what the linear regression expects (+margin ahead
    / -behind).
  - A usage bar colored by pace (green below, blue on pace, amber above, red when very little is
    left) with a vertical mark for the expected value.
  - Time until reset.

## Hardware (ESP32-2432S028R)

- ESP32-WROOM-32 with a 320x240 SPI ILI9341 panel and a resistive XPT2046 touch (unused).
- Panel pinout (fixed by `build_flags` in `platformio.ini`):
  SCLK 14, MOSI 13, MISO 12, CS 15, DC 2, RST -1, backlight (BL) GPIO 21.
- Backlight driven by PWM (LEDC) to dim on inactivity.
- The RGB LED (GPIO 4/16/17, active low) is turned off at boot.

## Requirements

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension).
- Dependencies (resolved by PlatformIO): TFT_eSPI, ArduinoJson.

## Credentials (WiFi and OTA)

1. Copy `include/secrets.example.h` to `include/secrets.h` and set the SSID, WiFi password, OTA
   password and (optionally) the default service URL. `secrets.h` is ignored by git.
2. For OTA flashing, copy `platformio_override.example.ini` to `platformio_override.ini` with the
   same OTA password in `--auth`.

Without `secrets.h` it builds with empty values: the CYD boots into the `CYD-Emergencia` AP
(password `configcyd0`) and you configure WiFi/service from `http://192.168.4.1`.

## Build and flash

Run from this `firmware/cyd/` folder.

**Linux.** The CYD reuses the OLED variant's PlatformIO `.venv`, so no separate install is needed:

```bash
# Build
../oled/.venv/bin/python3 -m platformio run -e cyd
# Flash over USB (the CYD ships a CH340, usually /dev/ttyUSB0)
../oled/.venv/bin/python3 -m platformio run -e cyd -t upload --upload-port /dev/ttyUSB0
# Flash over OTA (with the CYD on the network as cyd.local)
../oled/.venv/bin/python3 -m platformio run -e cyd_ota -t upload
```

**macOS.** The bundled `.venv/` is a local Linux artifact and is not in the repository, so install
PlatformIO once and use the system `pio` (replace `../oled/.venv/bin/python3 -m platformio` with
`pio` in every command above):

```bash
brew install platformio          # or: pip3 install platformio
# Build
pio run -e cyd
# Flash over USB (the CYD ships a CH340; on macOS it shows up as /dev/cu.wchusbserial*)
pio run -e cyd -t upload --upload-port /dev/cu.wchusbserial-0001
# Flash over OTA
pio run -e cyd_ota -t upload
```

- On macOS use the `cu.*` node, never `tty.*`. If the port does not appear, install the WCH CH34x
  driver (the CYD uses a CH340 USB-serial chip). There is no `dialout` group or `sudo`: the port
  works as-is.
- List the available ports with `pio device list` (or `.venv/bin/python3 -m platformio device list`
  on Linux).

If USB upload fails, lower `upload_speed` in `platformio.ini` (921600 -> 460800 -> 115200).

## Configuration

Booting without valid WiFi brings up the `CYD-Emergencia` AP. Connect and open
`http://192.168.4.1` to set the SSID, password and service URL (`http://PC_IP:8765`). Once on the
network the CYD is reachable at `http://cyd.local` to reconfigure the service, rotation,
brightness, dimming and poll interval, and for web OTA.

## Structure

- `src/main.cpp` - the entire firmware (setup, loop, WiFi/AP, web, OTA, polling and rendering).
- `platformio.ini` - build environments and the TFT_eSPI configuration for the CYD.

## Relationship with the service

This firmware depends on the service in [monitor-service/](../../monitor-service/), which exposes
the `/api/esp/*` endpoints. It consumes `GET /api/esp/snapshot` (data) and
`GET /api/esp/activity-wait` (activity long-poll). It also hits `GET /api/esp/frames` with an
`If-None-Match`, but only to read the `X-Sessions-*` session-count headers off the 304 response;
it never downloads the framebuffer. Theme colors, labels, brightness and usage data
are set in the service web UI and applied on the next poll, without reflashing. You only reflash
when `src/main.cpp`, `platformio.ini` or the PlatformIO dependencies change.
