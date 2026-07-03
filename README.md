# ESP-o-nitor

A physical usage monitor for Claude and Codex: an ESP32 with two SSD1306 OLED displays
(128x64) that shows each tool's quota and activity. All the computation and rendering happens
on a service running on the PC; the ESP32 only projects the framebuffers it receives.

```
[monitor-service (PC/Docker)]  --HTTP-->  [ESP32]  --I2C-->  2x OLED SSD1306
        renders 128x64 1-bit             projects            Claude / Codex
```

## Repository layout

| Folder | Contents |
| --- | --- |
| [`esp32/`](esp32/) | ESP32 firmware (PlatformIO, C++). Polls the service, projects the screens, animates activity and shows local screensavers. |
| [`monitor-service/`](monitor-service/) | Host service (Python). Reads Claude/Codex quota, renders the OLED screens and serves them over HTTP; includes a live configuration web UI. |
| [`3d-models/`](3d-models/) | Printable enclosure (STEP): case and lid for the ESP32 board plus the base and lid that hold the two screens. |

The root holds shared material: this README and common configuration.

## Getting started

1. **Host service** ([`monitor-service/`](monitor-service/)): run it with Docker
   (`docker compose up -d --build`) or locally. It serves the web UI at `http://localhost:8765`
   and the `/api/esp/*` endpoints the ESP consumes.
2. **Firmware** ([`esp32/`](esp32/)): build and flash with PlatformIO (USB or OTA), then point
   the ESP at the service URL from its configuration web page.

Each folder has its own README with the details on architecture, requirements and commands.

## How the pieces fit

- The service draws each screen (128x64, 1 bit) and serves it at `/api/esp/frames`.
- The ESP32 polls that endpoint and copies the bitmap straight to the OLED (it never interprets
  the content).
- Activity (working / waiting / idle) arrives via long-poll so the device reacts almost
  instantly; the corner animation is downloaded once (re-downloaded only if its ETag changes).
- Almost everything (theme, labels, animation, screensaver, brightness, manual usage) is set
  from the service web UI and applied on the ESP's next poll, without reflashing. You only need
  to reflash when the firmware itself changes (`esp32/src/main.cpp`, `esp32/platformio.ini` or
  its dependencies).

## Development

- **Host service** (`monitor-service/`): runs with hot reload via `watchmedo`, mounting
  `monitor-service/src` into the container. For changes to Python, embedded HTML, CSS or service
  logic there is no need to restart or run Docker; the live service reloads on its own. Validate
  with `python -m compileall monitor-service/src` and, when relevant, against the HTTP endpoints
  of the already running service. Only use Docker commands when the container configuration,
  dependencies, `Dockerfile` or `docker-compose.yml` change, or if the live service stops
  responding.
- **Firmware** (`esp32/`): only needs reflashing when `esp32/src/main.cpp`,
  `esp32/platformio.ini` or the PlatformIO dependencies change. See [`esp32/README.md`](esp32/)
  for the build and flash commands (USB and OTA).

## License

Released under the MIT License. See [LICENSE](LICENSE).
