# ESP-o-nitor

A physical usage monitor for Claude and Codex. A host service reads each tool's quota and
activity and feeds an ESP32 display that shows, at a glance, how much of your 5h and weekly
windows is left and whether each tool is working, waiting or idle.

The heavy lifting (reading quota, tracking activity, deciding pace) always happens in the host
service. What changes between builds is only the **display hardware**, so the firmware comes in
two variants that talk to the **same** service:

```
                                   /--HTTP frames--> [OLED ESP32] --I2C--> 2x SSD1306 128x64 (mono)
[monitor-service (PC/Docker)] -----|
   reads quota + activity          \--HTTP JSON----> [CYD ESP32]  --SPI--> 1x ILI9341 320x240 (color)
```

| Variant | Hardware | How it draws |
| --- | --- | --- |
| **OLED** | 2x SSD1306 128x64 mono (I2C), one screen per tool | The service renders the 1-bit framebuffers and the ESP just projects them (`/api/esp/frames`). |
| **CYD** | 1x ILI9341 320x240 color (ESP32-2432S028R "Cheap Yellow Display"), both tools on one screen | The ESP draws in color on-device from a ~1 KB JSON snapshot (`/api/esp/snapshot`). |

Pick one variant per device; the service supports both at once with no changes.

## Repository layout

| Folder | Contents |
| --- | --- |
| [`monitor-service/`](monitor-service/) | Host service (Python). Reads Claude/Codex quota, detects activity, renders the OLED frames and serves everything over HTTP. Includes a live configuration web UI. Shared by both firmware variants. |
| [`firmware/oled/`](firmware/oled/) | OLED firmware (PlatformIO, C++): projects the framebuffers the service renders, animates the activity corner and shows local screensavers. |
| [`firmware/cyd/`](firmware/cyd/) | CYD firmware (PlatformIO, C++): draws both tools in color natively from the JSON snapshot. |
| [`3d-models/`](3d-models/) | Printable enclosure for the OLED dual-screen build (STEP + STL with PNG previews): case and lid for the board plus the base and lids that hold the two screens. |

Each folder has its own README with architecture, requirements and commands.

## Getting started

1. **Host service** ([`monitor-service/`](monitor-service/)): run it with Docker
   (`docker compose up -d --build`) or locally. It serves the web UI at `http://localhost:8765`
   and the `/api/esp/*` endpoints the devices consume. This step is the same for both variants.
2. **Firmware**: build and flash the variant that matches your hardware with PlatformIO (USB or
   OTA), then point the device at the service URL (`http://PC_IP:8765`) from its own
   configuration web page.
   - OLED: [`firmware/oled/`](firmware/oled/)
   - CYD: [`firmware/cyd/`](firmware/cyd/)

## How the pieces fit

- The service builds a usage snapshot (remaining percentage, 5h/weekly windows, expected pace)
  and exposes it under `/api/esp/*`. The OLED variant consumes pre-rendered frames
  (`/api/esp/frames`); the CYD variant consumes the raw snapshot (`/api/esp/snapshot`) and draws
  it in color itself.
- Activity (working / waiting / idle) arrives via long-poll (`/api/esp/activity-wait`) so either
  device reacts almost instantly and drives its activity animation.
- Almost everything (theme, labels, animation, screensaver, brightness, manual usage) is set from
  the service web UI and applied on the device's next poll, without reflashing. You only reflash
  when the firmware itself changes (`firmware/<variant>/src/main.cpp`, its `platformio.ini` or its
  dependencies).

## Development

- **Host service** (`monitor-service/`): runs with hot reload via `watchmedo`, mounting
  `monitor-service/src` into the container. For changes to Python, embedded HTML, CSS or service
  logic there is no need to restart or run Docker; the live service reloads on its own. Validate
  with `python -m compileall monitor-service/src` and, when relevant, against the HTTP endpoints
  of the already running service. Only use Docker commands when the container configuration,
  dependencies, `Dockerfile` or `docker-compose.yml` change, or if the live service stops
  responding.
- **Firmware** (`firmware/oled/`, `firmware/cyd/`): only needs reflashing when that variant's
  `src/main.cpp`, `platformio.ini` or PlatformIO dependencies change. See each variant's README
  for the build and flash commands (USB and OTA).

## License

Released under the MIT License. See [LICENSE](LICENSE).
