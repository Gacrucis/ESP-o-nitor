# monitor-service

Local service that feeds the [ESP32 monitor](../esp32/) of Claude and Codex usage. It reads each
tool's quota, renders the two OLED screens (128x64, 1 bit) as images, generates the activity
animation and the screensaver, and serves it all over HTTP. It includes a live configuration web
UI.

The ESP32 computes nothing: this service does the rendering and the device only projects.

## How it works

- Builds a usage snapshot (remaining percentage, 5h/weekly windows, expected pace).
  - Claude: usage OAuth endpoint.
  - Codex: local `rate_limits` snapshots from the rollouts.
- Renders each screen with Pillow according to the selected theme and serves it as a framebuffer
  at `/api/esp/frames` (with cache and ETag so it is not rebuilt on every ESP poll).
- Detects activity (working / waiting / idle) and delivers it via long-poll so the ESP reacts
  almost instantly.
- Packs the corner animation (configurable style + size) into a binary that the ESP downloads
  once (re-downloaded only if the ETag changes).
- Measures ESP telemetry (last poll, cadence, IP) to show in the web UI whether it is connected
  and when a freshly saved change will take effect.

## Requirements

- Python >= 3.12 and Pillow + watchdog (see `pyproject.toml`), or Docker.
- Read-only access to `~/.claude` and `~/.codex` (where quota and activity are read from).

## Run with Docker (recommended)

```bash
docker compose up -d --build
```

The web UI is at `http://localhost:8765`. The `docker-compose.yml`:

- Mounts `./src` for hot reload (when you edit the code on the host, `watchmedo` restarts the
  service).
- Mounts `~/.claude` and `~/.codex` as read-only.
- Persists the configuration in the `monitor-service-data` volume (`/app/data/config.json`).

Logs and status:

```bash
docker compose ps
docker compose logs --tail 80
```

## Run without Docker

```bash
pip install pillow==11.0.0 watchdog==6.0.0
export CONFIG_PATH=./data/config.json
export CLAUDE_HOME="$HOME/.claude"
export CODEX_HOME="$HOME/.codex"
export SERVICE_HOST=0.0.0.0
export SERVICE_PORT=8765
PYTHONPATH=src python -m monitor_service.main
```

## Environment variables

| Variable | Description |
| --- | --- |
| `CONFIG_PATH` | Path of the persistent `config.json` (required). |
| `CLAUDE_HOME` / `CODEX_HOME` | Claude and Codex directories (read-only). |
| `SERVICE_HOST` / `SERVICE_PORT` | HTTP server host and port. |
| `FRAME_CACHE_TTL_SECONDS` | Minimum interval between ESP frame regenerations. |
| `CLAUDE_USAGE_TTL_SECONDS` | Minimum interval between real Claude usage queries. |
| `CODEX_USAGE_TTL_SECONDS` | Minimum interval between local Codex reads. |

Most settings (theme, labels, animation, screensaver, dimming, manual usage) are edited from the
web UI and saved in `config.json`; the variables only set the initial default values.

## Configuration web UI

`http://localhost:8765` offers:

- Home: Claude/Codex cards, ESP32 device status (live) and a preview of both screens.
- Usage: manual percentages and windows (5h / weekly).
- Settings: labels, tolerance, cache TTLs, anti burn-in dimming, activity animation (style, speed
  and box size), theme and screensavers.

On save, the web UI estimates when it will take effect based on the ESP's real poll cadence and
confirms once the device applies it.

## Main endpoints

Web / configuration:

- `GET /` - web interface.
- `GET /api/config` / `POST /api/config` - read / save configuration.
- `POST /api/usage/manual` - set manual usage.
- `GET /api/status` - diagnostics of paths and sources.
- `GET /api/esp/status` - ESP telemetry (online, last poll, cadence, IP).
- `GET /api/themes` - available themes.

Consumed by the ESP32:

- `GET /api/esp/frames` - framebuffers of both screens (octet-stream, ETag).
- `GET /api/esp/activity-wait` - activity long-poll.
- `GET /api/esp/activity-animation` - binary package of the corner animation.

Previews (web):

- `GET /api/esp/snapshot` - JSON snapshot.
- `GET /api/esp/preview/{claude|codex}.png` - screen preview.
- `GET /api/esp/activity-preview.gif` - animation preview.
- `GET /api/esp/screensaver-preview.gif` - screensaver preview.

## Structure

- `src/monitor_service/main.py` - HTTP server, embedded web UI and endpoints.
- `renderer.py` - screen rendering (delta theme).
- `activity_animation.py` - generation and packaging of the corner animation.
- `screensavers.py` - screensaver previews.
- `usage.py` / `claude_usage.py` / `collectors.py` - quota and activity reading.
- `config.py` / `types.py` - configuration loading, normalization and types.

## Theme and styles

- Screen theme: `delta` (terminal-style columns with remaining %, pace delta and reset, plus a
  solid bar marking the value expected from the linear regression).
- Animation styles: `spinner`, `dots`, `dots-right`, `stars-right`, `pulse`, `bars`, `ball`,
  `wave`, `worm`. The box size is configurable and the animations adapt to it.
- Screensavers (rendered locally by the ESP): `black`, `snake`, `pipes`, `matrix`, `dvd`, `maze`,
  `flower`.

## Development

- With Docker, hot reload is already active; editing `src/` restarts the service on its own.
- Quick validation: `python -m compileall src`.
