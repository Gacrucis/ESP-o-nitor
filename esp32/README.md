# ESP-o-nitor (firmware)

Firmware for an ESP32 with two SSD1306 OLED displays (128x64) that act as a physical usage
monitor for Claude and Codex. The ESP32 computes nothing: it only projects the framebuffers
produced by the [monitor-service](../monitor-service/), animates the activity corner and shows
local screensavers when the service does not respond.

## Architecture

```
[monitor-service (PC/Docker)]  --HTTP-->  [ESP32]  --I2C-->  2x OLED SSD1306
        renders 128x64 1-bit             projects            Claude / Codex
```

- The service draws each screen (128x64, 1 bit) and serves it at `/api/esp/frames`.
- The ESP32 polls that endpoint and copies the bitmap straight to the OLED (it never interprets
  the content).
- Activity (working / waiting / idle) arrives via long-poll and headers, and drives the animation
  in the top-right corner; while waiting for a response the screen blinks inverted.
- The animation package is downloaded once and kept in RAM; it is re-downloaded only if its ETag
  changes.
- If the service is down, the ESP shows a local screensaver (anti burn-in).

## Hardware

- Board: ESP32 dev (`board = esp32dev`).
- 2x OLED SSD1306 128x64 over I2C, on separate buses (one `TwoWire` per display).
- Default I2C pins (configurable from the ESP web UI):
  - Claude: SDA 21, SCL 22, address `0x3C`.
  - Codex: SDA 16, SCL 17, address `0x3C`.

## Features

- Projection of 128x64 framebuffers from the service.
- Activity animation in the corner (size and style configurable from the service).
- Blinking screen inversion when a tool is waiting for the user.
- Local anti burn-in screensavers: black, snake, pipes, matrix, dvd, maze, flower.
- Brightness dimming on inactivity (decided by the service via the `X-Brightness` header).
- Built-in web server on port 80 to configure WiFi, service URL, pins and displays.
- Emergency AP mode when it cannot connect to WiFi (portal at `http://192.168.4.1`).
- Network OTA (ArduinoOTA, port 3232) and web OTA.
- Configuration persistence in NVS (`Preferences`).
- mDNS: the device is reachable as `esp32.local`.

## Structure

- `src/main.cpp` - the entire firmware (setup, loop, web server, OTA, rendering, screensavers).
- `platformio.ini` - build environments and dependencies.

## Requirements

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension).
- Dependencies (resolved automatically by PlatformIO): Adafruit SSD1306, Adafruit GFX,
  ArduinoJson.

## Credentials (WiFi and OTA)

The firmware ships no credentials in the source. To set local default values:

1. Copy `include/secrets.example.h` to `include/secrets.h` and put your SSID, WiFi password and
   OTA password there. That file is ignored by git and never versioned.
2. For OTA flashing, copy `platformio_override.example.ini` to `platformio_override.ini` and put
   the same OTA password in `--auth`. PlatformIO applies it automatically on top of
   `platformio.ini`; it is ignored by git too.

Without `secrets.h`, the firmware builds with empty credentials: the ESP boots into the emergency
AP and you configure WiFi/service from its web UI. The OTA password must match between
`secrets.h` (running firmware) and `platformio_override.ini` (OTA upload).

## Build and flash

### Toolchain (PlatformIO in the project `.venv`)

All commands in this section run from the `esp32/` folder (where `platformio.ini` and `.venv/`
live).

PlatformIO 6.1.19 lives in the project `.venv`. The `.venv/bin/pio` launcher kept a shebang
inherited from another project (it points to a non-existent Python), so the module is invoked
with the venv's Python, which does work:

```bash
.venv/bin/python3 -m platformio <command>
```

To get `pio` back directly, reinstall the launcher once:

```bash
.venv/bin/python3 -m pip install --force-reinstall platformio
```

(If you already have PlatformIO on the system PATH, just use `pio <command>`.)

**macOS**: the bundled `.venv/` is a local Linux artifact and is not part of the repository, so on
Mac you install PlatformIO separately (once) and use the system `pio`:

```bash
brew install platformio          # or: pip3 install platformio
pio run -e esp32dev              # replaces `.venv/bin/python3 -m platformio ...`
```

In the rest of this guide, replace `.venv/bin/python3 -m platformio` with `pio`.

### Over USB (first flash or if OTA fails)

Connect the ESP32 over USB. PlatformIO auto-detects the serial port; force it with `--upload-port`
if there are several devices. To list the available ports:

```bash
.venv/bin/python3 -m platformio device list
```

```bash
.venv/bin/python3 -m platformio run -e esp32dev -t upload
# explicit port, optional:
.venv/bin/python3 -m platformio run -e esp32dev -t upload --upload-port <port>
```

**Linux**

- Typical ports: `/dev/ttyUSB0` (CP210x/CH340 chip) or `/dev/ttyACM0` (native USB).
- If the port is permission denied, add your user to the `dialout` group
  (`sudo usermod -aG dialout $USER`) and restart the session.

**macOS**

- Typical ports: `/dev/cu.usbserial-*` or `/dev/cu.SLAB_USBtoUART` (CP210x),
  `/dev/cu.wchusbserial*` (CH340). Always use the `cu.*` node, not `tty.*`.
- Modern macOS (11+) already ships the CP210x and CH340 drivers. If the port does not appear,
  install the chip vendor's driver: Silicon Labs CP210x VCP or WCH CH34x.
- There is no `dialout` group or permission tweak: the port works without `sudo`.

```bash
# example on macOS with an explicit port:
.venv/bin/python3 -m platformio run -e esp32dev -t upload --upload-port /dev/cu.usbserial-0001
```

### Over OTA (with the ESP already on the network, reachable as `esp32.local`)

```bash
.venv/bin/python3 -m platformio run -e esp32dev_ota -t upload
# if mDNS does not resolve, point at the ESP's IP:
.venv/bin/python3 -m platformio run -e esp32dev_ota -t upload --upload-port 192.168.1.xxx
```

The `esp32dev_ota` environment uses `espota` against `esp32.local` with the password defined in
`platformio_override.ini` (`--auth=...`, see [Credentials](#credentials-wifi-and-ota)). It must
match the OTA password of the running firmware; if you change one, change the other.

### Build only (no upload)

```bash
.venv/bin/python3 -m platformio run -e esp32dev
```

The default WiFi and OTA credentials come from `include/secrets.h` (see
[Credentials](#credentials-wifi-and-ota)); at runtime they are changed from the ESP web UI.

## First boot and configuration

1. After flashing, if the ESP cannot connect to the configured WiFi, it brings up the emergency
   AP `ESP32-Emergencia`. Connect to it and open `http://192.168.4.1`.
2. Configure the SSID, password and the service URL (for example `http://PC_IP:8765`).
3. Once on the network, the ESP is reachable at `http://esp32.local` to reconfigure pins,
   displays, contrast and to check status.

## Relationship with the service

This firmware depends on the service in [monitor-service/](../monitor-service/), which exposes the
`/api/esp/*` endpoints the ESP consumes. Changes to animation style/size, theme, screensavers,
brightness and usage data are made in the service web UI and applied on the ESP's next poll,
without reflashing. You only need to reflash when `src/main.cpp`, `platformio.ini` or the
PlatformIO dependencies change.
