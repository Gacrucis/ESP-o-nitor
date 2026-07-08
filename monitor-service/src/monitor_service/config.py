import json
import math
import os
import time
from json import JSONDecodeError
from pathlib import Path
from typing import Any, cast

from monitor_service.logutil import log_event
from monitor_service.types import ActivityAnimationConfig, ServiceConfig, ToolConfig, ToolId, UsageWindowConfig

# Valid animation styles for the activity corner (must exist in activity_animation.py).
ACTIVITY_ANIMATION_STYLES = ("spinner", "dots", "dots-right", "stars-right", "pulse", "bars", "ball", "wave", "worm")

# Bounds of the activity animation frame (px). The maximum matches the
# cap the firmware validates (ANIM_FRAME_MAX_* in main.cpp) and the OLED canvas.
ANIM_FRAME_DEFAULT_WIDTH = 48
ANIM_FRAME_DEFAULT_HEIGHT = 5
ANIM_FRAME_MIN_WIDTH = 8
ANIM_FRAME_MIN_HEIGHT = 1
ANIM_FRAME_MAX_WIDTH = 128
# Height cap consistent with the firmware buffer: the worst case (128x16 x 32 frames)
# is 8192 bytes, exactly what ANIM_MAX_BYTES reserves in main.cpp.
ANIM_FRAME_MAX_HEIGHT = 16

# Valid display themes (must exist in renderer.THEME_DEFINITIONS / _THEMES).
THEMES = ("delta",)
DEFAULT_THEME = "delta"

# Valid screensavers (anti burn-in when the service is down). The ESP renders them
# locally; they must match those in the firmware and the previews.
SCREENSAVERS = ("black", "snake", "pipes", "matrix", "dvd", "maze", "flower")
DEFAULT_SCREENSAVER = "dvd"


def now_ms() -> int:
    return int(time.time() * 1000)


def get_int_env(name: str, fallback: int) -> int:
    raw = os.environ.get(name)
    if raw is None or raw == "":
        return fallback

    try:
        return int(raw)
    except ValueError as error:
        raise RuntimeError(f"{name} must be an integer, got {raw!r}") from error


def build_usage_window_config(start_ms: int, reset_ms: int, remaining_percent: float) -> UsageWindowConfig:
    return {
        "remaining_percent": remaining_percent,
        "window_start_ms": start_ms,
        "window_reset_ms": reset_ms,
    }


def build_default_tool_config(label: str, start_ms: int, current_reset_ms: int, weekly_reset_ms: int) -> ToolConfig:
    return {
        "enabled": True,
        "label": label,
        "current": build_usage_window_config(start_ms, current_reset_ms, 100.0),
        "weekly": build_usage_window_config(start_ms, weekly_reset_ms, 100.0),
        "waiting_for_user": False,
        "status_text": "No recent data",
        "manual_override": True,
    }


def build_default_activity_animation_config() -> ActivityAnimationConfig:
    return {
        "style": "spinner",
        "interval_ms": get_int_env("ACTIVITY_INTERVAL_MS", 120),
        "invert_on_waiting": True,
        "invert_blink_ms": get_int_env("ACTIVITY_INVERT_BLINK_MS", 600),
        "frame_width": get_int_env("ACTIVITY_FRAME_WIDTH", ANIM_FRAME_DEFAULT_WIDTH),
        "frame_height": get_int_env("ACTIVITY_FRAME_HEIGHT", ANIM_FRAME_DEFAULT_HEIGHT),
        "codex_busy_window_seconds": get_int_env("CODEX_BUSY_WINDOW_SECONDS", 15),
        "stale_seconds": get_int_env("ACTIVITY_STALE_SECONDS", 1800),
        "include_codex_subagents": True,
        "include_claude_subagents": False,
    }


def build_default_config() -> ServiceConfig:
    current_ms = now_ms()
    current_reset_ms = current_ms + (5 * 60 * 60 * 1000)
    weekly_reset_ms = current_ms + (7 * 24 * 60 * 60 * 1000)

    return {
        "tolerance_percent": 5.0,
        "theme": DEFAULT_THEME,
        "frame_cache_ttl_seconds": get_int_env("FRAME_CACHE_TTL_SECONDS", 5),
        # The .env CLAUDE_USAGE_TTL_SECONDS provides the default; it is then edited in the web UI.
        "claude_usage_ttl_seconds": get_int_env("CLAUDE_USAGE_TTL_SECONDS", 300),
        "codex_usage_ttl_seconds": get_int_env("CODEX_USAGE_TTL_SECONDS", 30),
        "activity_animation": build_default_activity_animation_config(),
        "screensaver": DEFAULT_SCREENSAVER,
        "dim_after_seconds": get_int_env("DIM_AFTER_SECONDS", 600),
        "dim_brightness_percent": get_int_env("DIM_BRIGHTNESS_PERCENT", 30),
        "claude": build_default_tool_config("Claude", current_ms, current_reset_ms, weekly_reset_ms),
        "codex": build_default_tool_config("Codex", current_ms, current_reset_ms, weekly_reset_ms),
    }


def get_config_path() -> Path:
    raw_path = os.environ.get("CONFIG_PATH")
    if raw_path is None or raw_path == "":
        raise RuntimeError("CONFIG_PATH is not configured")

    return Path(raw_path)


def get_readonly_home(env_name: str) -> Path:
    raw_path = os.environ.get(env_name)
    if raw_path is None or raw_path == "":
        raise RuntimeError(f"{env_name} is not configured")

    return Path(raw_path)


def clamp_percent(value: float) -> float:
    # NaN/Infinity (e.g. a hand-edited or corrupt config; json.loads accepts the NaN token) must
    # not leak into the render math: int(round(nan)) raises ValueError and would crash the frame.
    if not math.isfinite(value):
        return 0.0
    if value < 0.0:
        return 0.0
    if value > 100.0:
        return 100.0
    return value


def clamp_int(value: int, minimum: int, maximum: int) -> int:
    if value < minimum:
        return minimum
    if value > maximum:
        return maximum
    return value


def to_float(value: Any, fallback: float) -> float:
    # Total parse used by normalize_*: a non-numeric or null value in a persisted/POSTed config
    # degrades to the fallback instead of raising ValueError/TypeError out of load_config (which
    # runs on every ESP poll and web call) and bricking the service.
    try:
        result = float(value)
    except (TypeError, ValueError):
        return fallback
    return result if math.isfinite(result) else fallback


def to_int(value: Any, fallback: int) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return fallback


def as_dict(value: Any) -> dict[str, Any]:
    # A persisted/POSTed config where a nested section (claude, activity_animation, ...) is not an
    # object would make the downstream `.get(...)` raise AttributeError. Non-dicts collapse to an
    # empty dict so normalize_* falls back field by field instead of crashing.
    return cast(dict[str, Any], value) if isinstance(value, dict) else {}


def normalize_tool_config(raw_config: dict[str, Any], fallback_config: ToolConfig) -> ToolConfig:
    legacy_window = {
        "remaining_percent": raw_config.get("remaining_percent", fallback_config["current"]["remaining_percent"]),
        "window_start_ms": raw_config.get("window_start_ms", fallback_config["current"]["window_start_ms"]),
        "window_reset_ms": raw_config.get("window_reset_ms", fallback_config["current"]["window_reset_ms"]),
    }
    raw_current = as_dict(raw_config.get("current", legacy_window))
    raw_weekly = as_dict(raw_config.get("weekly", fallback_config["weekly"]))

    # Migrate the legacy Spanish default so the "no recent data" sentinel keeps matching
    # persisted configs saved before the switch to English.
    raw_status = str(raw_config.get("status_text", fallback_config["status_text"]))
    status_text = "No recent data" if raw_status == "Sin datos recientes" else raw_status

    return {
        "enabled": bool(raw_config.get("enabled", fallback_config["enabled"])),
        "label": str(raw_config.get("label", fallback_config["label"])),
        "current": normalize_usage_window_config(raw_current, fallback_config["current"]),
        "weekly": normalize_usage_window_config(raw_weekly, fallback_config["weekly"]),
        "waiting_for_user": bool(raw_config.get("waiting_for_user", fallback_config["waiting_for_user"])),
        "status_text": status_text,
        "manual_override": bool(raw_config.get("manual_override", fallback_config["manual_override"])),
    }


def normalize_usage_window_config(raw_config: dict[str, Any], fallback_config: UsageWindowConfig) -> UsageWindowConfig:
    return {
        "remaining_percent": clamp_percent(to_float(raw_config.get("remaining_percent", fallback_config["remaining_percent"]), fallback_config["remaining_percent"])),
        "window_start_ms": to_int(raw_config.get("window_start_ms", fallback_config["window_start_ms"]), fallback_config["window_start_ms"]),
        "window_reset_ms": to_int(raw_config.get("window_reset_ms", fallback_config["window_reset_ms"]), fallback_config["window_reset_ms"]),
    }


def normalize_activity_animation_config(raw_config: dict[str, Any], fallback_config: ActivityAnimationConfig) -> ActivityAnimationConfig:
    raw_style = str(raw_config.get("style", fallback_config["style"]))
    style = raw_style if raw_style in ACTIVITY_ANIMATION_STYLES else fallback_config["style"]

    frame_width = clamp_int(
        to_int(raw_config.get("frame_width", fallback_config["frame_width"]), fallback_config["frame_width"]), ANIM_FRAME_MIN_WIDTH, ANIM_FRAME_MAX_WIDTH
    )
    frame_height = clamp_int(
        to_int(raw_config.get("frame_height", fallback_config["frame_height"]), fallback_config["frame_height"]), ANIM_FRAME_MIN_HEIGHT, ANIM_FRAME_MAX_HEIGHT
    )

    return {
        "style": style,
        "interval_ms": max(20, to_int(raw_config.get("interval_ms", fallback_config["interval_ms"]), fallback_config["interval_ms"])),
        "invert_on_waiting": bool(raw_config.get("invert_on_waiting", fallback_config["invert_on_waiting"])),
        "invert_blink_ms": max(100, to_int(raw_config.get("invert_blink_ms", fallback_config["invert_blink_ms"]), fallback_config["invert_blink_ms"])),
        "frame_width": frame_width,
        "frame_height": frame_height,
        "codex_busy_window_seconds": max(1, to_int(raw_config.get("codex_busy_window_seconds", fallback_config["codex_busy_window_seconds"]), fallback_config["codex_busy_window_seconds"])),
        "stale_seconds": max(60, to_int(raw_config.get("stale_seconds", fallback_config["stale_seconds"]), fallback_config["stale_seconds"])),
        "include_codex_subagents": bool(raw_config.get("include_codex_subagents", fallback_config["include_codex_subagents"])),
        "include_claude_subagents": bool(raw_config.get("include_claude_subagents", fallback_config["include_claude_subagents"])),
    }


def normalize_theme(raw_value: Any, fallback: str) -> str:
    candidate = str(raw_value)
    return candidate if candidate in THEMES else fallback


def normalize_screensaver(raw_value: Any, fallback: str) -> str:
    candidate = str(raw_value)
    return candidate if candidate in SCREENSAVERS else fallback


def normalize_config(raw_config: dict[str, Any]) -> ServiceConfig:
    fallback_config = build_default_config()

    return {
        "tolerance_percent": clamp_percent(to_float(raw_config.get("tolerance_percent", fallback_config["tolerance_percent"]), fallback_config["tolerance_percent"])),
        "theme": normalize_theme(raw_config.get("theme", fallback_config["theme"]), fallback_config["theme"]),
        "frame_cache_ttl_seconds": max(1, to_int(raw_config.get("frame_cache_ttl_seconds", fallback_config["frame_cache_ttl_seconds"]), fallback_config["frame_cache_ttl_seconds"])),
        "claude_usage_ttl_seconds": max(10, to_int(raw_config.get("claude_usage_ttl_seconds", fallback_config["claude_usage_ttl_seconds"]), fallback_config["claude_usage_ttl_seconds"])),
        "codex_usage_ttl_seconds": max(1, to_int(raw_config.get("codex_usage_ttl_seconds", fallback_config["codex_usage_ttl_seconds"]), fallback_config["codex_usage_ttl_seconds"])),
        "activity_animation": normalize_activity_animation_config(
            as_dict(raw_config.get("activity_animation", {})), fallback_config["activity_animation"]
        ),
        "screensaver": normalize_screensaver(raw_config.get("screensaver", fallback_config["screensaver"]), fallback_config["screensaver"]),
        "dim_after_seconds": max(0, to_int(raw_config.get("dim_after_seconds", fallback_config["dim_after_seconds"]), fallback_config["dim_after_seconds"])),
        "dim_brightness_percent": min(100, max(0, to_int(raw_config.get("dim_brightness_percent", fallback_config["dim_brightness_percent"]), fallback_config["dim_brightness_percent"]))),
        "claude": normalize_tool_config(as_dict(raw_config.get("claude", {})), fallback_config["claude"]),
        "codex": normalize_tool_config(as_dict(raw_config.get("codex", {})), fallback_config["codex"]),
    }


def _recover_corrupt_config(config_path: Path, detail: str) -> ServiceConfig:
    # A corrupt/truncated config.json must not brick the whole service: load_config runs on every
    # ESP poll and web call. The unreadable file is moved aside (for inspection) and defaults are
    # rebuilt so the device keeps working instead of returning 500 on every request.
    backup_path = config_path.with_suffix(".corrupt")
    try:
        os.replace(config_path, backup_path)
    except OSError as error:
        log_event("config_corrupt_backup_failed", detail=str(error))
    log_event("config_corrupt_recovered", detail=detail, backup=str(backup_path))
    config = build_default_config()
    save_config(config)
    return config


def load_config() -> ServiceConfig:
    config_path = get_config_path()
    if not config_path.exists():
        config = build_default_config()
        save_config(config)
        return config

    try:
        raw_text = config_path.read_text(encoding="utf-8")
    except OSError as error:
        # A read failure (permissions, transient IO) is NOT corruption: fail loud instead of
        # clobbering a possibly-good file with defaults.
        raise RuntimeError(f"Could not read the configuration at {config_path}: {error}") from error

    try:
        raw_config = json.loads(raw_text)
    except (JSONDecodeError, ValueError) as error:
        return _recover_corrupt_config(config_path, f"invalid JSON: {error}")

    if not isinstance(raw_config, dict):
        return _recover_corrupt_config(config_path, "config root is not a JSON object")

    return normalize_config(cast(dict[str, Any], raw_config))


def save_config(config: ServiceConfig) -> None:
    # Atomic write: serialize to a temp file, fsync, then os.replace (atomic rename on the same
    # filesystem). A crash or full disk mid-write can no longer leave config.json truncated, which
    # previously bricked load_config (JSONDecodeError on every request) until manual repair.
    config_path = get_config_path()
    config_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = config_path.with_suffix(config_path.suffix + ".tmp")

    try:
        with tmp_path.open("w", encoding="utf-8") as handle:
            json.dump(config, handle, ensure_ascii=False, indent=2)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp_path, config_path)
    except OSError as error:
        try:
            tmp_path.unlink(missing_ok=True)
        except OSError:
            pass
        raise RuntimeError(f"Could not save the configuration at {config_path}: {error}") from error


def update_tool_config(config: ServiceConfig, tool_id: ToolId, tool_config: ToolConfig) -> ServiceConfig:
    updated_config: ServiceConfig = {
        "tolerance_percent": config["tolerance_percent"],
        "theme": config["theme"],
        "frame_cache_ttl_seconds": config["frame_cache_ttl_seconds"],
        "claude_usage_ttl_seconds": config["claude_usage_ttl_seconds"],
        "codex_usage_ttl_seconds": config["codex_usage_ttl_seconds"],
        "activity_animation": config["activity_animation"],
        "screensaver": config["screensaver"],
        "dim_after_seconds": config["dim_after_seconds"],
        "dim_brightness_percent": config["dim_brightness_percent"],
        "claude": config["claude"],
        "codex": config["codex"],
    }
    updated_config[tool_id] = tool_config

    return updated_config
