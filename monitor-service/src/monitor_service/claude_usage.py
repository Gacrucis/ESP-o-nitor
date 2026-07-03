import json
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from json import JSONDecodeError
from pathlib import Path
from typing import Any, cast

from monitor_service.config import clamp_percent, get_config_path, now_ms
from monitor_service.logutil import log_event
from monitor_service.types import ToolUsageReading, UsageWindowConfig

# Read-only endpoint that Claude Code uses for `/usage`. It does not consume quota.
CLAUDE_USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
OAUTH_BETA_HEADER = "oauth-2025-04-20"
USER_AGENT = "esp-o-nitor-service/0.1"

# Duration of the windows reported by the endpoint.
FIVE_HOUR_MINUTES = 300
SEVEN_DAY_MINUTES = 10080

# Candidate keys where Claude Code stores the OAuth block in .credentials.json.
OAUTH_CONTAINER_KEYS = ("claudeAiOauth", "oauth", "claude_ai_oauth")
ACCESS_TOKEN_KEYS = ("accessToken", "access_token")
EXPIRES_AT_KEYS = ("expiresAt", "expires_at")

# The usage endpoint has its own rate-limit: it is queried at most every
# ttl_seconds (default via .env CLAUDE_USAGE_TTL_SECONDS, editable in the web UI).
# After an attempt (successful or failed) the TTL is respected so as not to hammer the 429,
# and on failure the last % obtained is kept indefinitely.
_cache_lock = threading.Lock()
_cached_reading: ToolUsageReading | None = None  # last GOOD reading (to keep the %)
_last_reading: ToolUsageReading | None = None  # last returned result (good or failure)
_last_attempt_ms = 0
_bootstrapped = False


class UsageSourceError(RuntimeError):
    # Base error of a real quota source.
    pass


class CredentialMissingError(UsageSourceError):
    # The credentials file does not exist or does not contain an access token.
    pass


class CredentialExpiredError(UsageSourceError):
    # The access token expired; Claude Code must refresh it on its next use.
    pass


class RateLimitedError(UsageSourceError):
    # The usage endpoint responded 429; it is not retried, cache is served.
    pass


class ClaudeUsageClient:
    # HTTP connector to the Claude subscription usage endpoint.
    def __init__(self, credentials_path: Path, request_timeout_seconds: float, max_attempts: int) -> None:
        self._credentials_path = credentials_path
        self._request_timeout_seconds = request_timeout_seconds
        self._max_attempts = max_attempts

    def _read_access_token(self) -> tuple[str, int]:
        # Returns (token, expires_at_ms). expires_at_ms is 0 if unknown.
        if not self._credentials_path.exists():
            raise CredentialMissingError(f"No credentials at {self._credentials_path}")

        try:
            raw = json.loads(self._credentials_path.read_text(encoding="utf-8"))
        except JSONDecodeError as error:
            raise UsageSourceError(f"Credentials with invalid JSON in {self._credentials_path}: {error}") from error
        except OSError as error:
            raise UsageSourceError(f"Could not read {self._credentials_path}: {error}") from error

        if not isinstance(raw, dict):
            raise UsageSourceError(f"{self._credentials_path} must contain a JSON object")

        container = self._extract_oauth_container(cast(dict[str, Any], raw))
        token = self._first_value(container, ACCESS_TOKEN_KEYS)
        if not isinstance(token, str) or token == "":
            raise CredentialMissingError("accessToken not found in the Claude credentials")

        expires_raw = self._first_value(container, EXPIRES_AT_KEYS)
        expires_at_ms = int(expires_raw) if isinstance(expires_raw, (int, float)) else 0
        return (token, expires_at_ms)

    def _extract_oauth_container(self, raw: dict[str, Any]) -> dict[str, Any]:
        for key in OAUTH_CONTAINER_KEYS:
            value = raw.get(key)
            if isinstance(value, dict):
                return cast(dict[str, Any], value)
        return raw

    def _first_value(self, container: dict[str, Any], keys: tuple[str, ...]) -> Any:
        for key in keys:
            if key in container:
                return container[key]
        return None

    def fetch_usage(self) -> dict[str, Any]:
        token, expires_at_ms = self._read_access_token()
        if expires_at_ms > 0 and now_ms() >= expires_at_ms:
            raise CredentialExpiredError("Claude token expired; open Claude Code to refresh it")

        request = urllib.request.Request(
            CLAUDE_USAGE_URL,
            headers={
                "Authorization": f"Bearer {token}",
                "anthropic-beta": OAUTH_BETA_HEADER,
                "Content-Type": "application/json",
                "User-Agent": USER_AGENT,
            },
        )
        return self._request_with_retries(request)

    def _request_with_retries(self, request: urllib.request.Request) -> dict[str, Any]:
        last_error: Exception = UsageSourceError("No attempts executed")
        for attempt in range(1, self._max_attempts + 1):
            try:
                with urllib.request.urlopen(request, timeout=self._request_timeout_seconds) as response:
                    body = response.read()
                parsed = json.loads(body.decode("utf-8"))
                if not isinstance(parsed, dict):
                    raise UsageSourceError("The usage endpoint did not return a JSON object")
                return cast(dict[str, Any], parsed)
            except urllib.error.HTTPError as error:
                # 401/403 are not retried: the credential is the problem.
                if error.code in (401, 403):
                    raise CredentialExpiredError(f"Claude rejected the credential (HTTP {error.code})") from error
                # 429 is not retried: retrying worsens the rate-limit; cache is served.
                if error.code == 429:
                    raise RateLimitedError("Claude usage endpoint rate-limited (HTTP 429)") from error
                last_error = error
                log_event("claude_usage_http_retry", attempt=attempt, status=error.code)
            except (urllib.error.URLError, TimeoutError, JSONDecodeError, OSError) as error:
                last_error = error
                log_event("claude_usage_request_retry", attempt=attempt, detail=str(error))

            if attempt < self._max_attempts:
                time.sleep(float(attempt))

        raise UsageSourceError(f"Claude usage query failed after {self._max_attempts} attempts: {last_error}") from last_error


def _parse_iso_ms(value: Any, fallback_ms: int) -> int:
    if not isinstance(value, str) or value == "":
        return fallback_ms

    try:
        parsed = datetime.fromisoformat(value)
    except ValueError:
        return fallback_ms

    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)

    return int(parsed.timestamp() * 1000)


def _window_from_claude(section: Any, window_minutes: int, current_ms: int) -> UsageWindowConfig:
    fallback_reset_ms = current_ms + (window_minutes * 60_000)

    if not isinstance(section, dict):
        return {
            "remaining_percent": 100.0,
            "window_start_ms": current_ms,
            "window_reset_ms": fallback_reset_ms,
        }

    utilization = section.get("utilization")
    used_percent = float(utilization) if isinstance(utilization, (int, float)) else 0.0
    remaining_percent = clamp_percent(100.0 - used_percent)

    window_reset_ms = _parse_iso_ms(section.get("resets_at"), fallback_reset_ms)
    window_start_ms = window_reset_ms - (window_minutes * 60_000)

    return {
        "remaining_percent": remaining_percent,
        "window_start_ms": window_start_ms,
        "window_reset_ms": window_reset_ms,
    }


def map_claude_usage(payload: dict[str, Any], current_ms: int) -> ToolUsageReading:
    current_window = _window_from_claude(payload.get("five_hour"), FIVE_HOUR_MINUTES, current_ms)
    weekly_window = _window_from_claude(payload.get("seven_day"), SEVEN_DAY_MINUTES, current_ms)

    return {
        "ok": True,
        "source": "claude-oauth-usage",
        "detail": "Real Claude subscription quota",
        "observed_at_ms": current_ms,
        "current": current_window,
        "weekly": weekly_window,
    }


def _empty_window(current_ms: int, window_minutes: int) -> UsageWindowConfig:
    return {
        "remaining_percent": 0.0,
        "window_start_ms": current_ms,
        "window_reset_ms": current_ms + (window_minutes * 60_000),
    }


def _cache_file_path() -> Path:
    # Persistence next to config.json, in the data volume.
    return get_config_path().parent / "claude_usage_cache.json"


def _load_persisted_reading() -> tuple[ToolUsageReading | None, int]:
    path = _cache_file_path()
    if not path.exists():
        return (None, 0)

    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, JSONDecodeError):
        return (None, 0)

    if not isinstance(raw, dict):
        return (None, 0)

    reading = raw.get("reading")
    saved_at_ms = raw.get("saved_at_ms", 0)
    if isinstance(reading, dict) and isinstance(saved_at_ms, (int, float)):
        # Backfill for caches written before observed_at_ms existed: the save
        # date is used as the best estimate of when the quota was obtained.
        reading.setdefault("observed_at_ms", int(saved_at_ms))
        return (cast(ToolUsageReading, reading), int(saved_at_ms))

    return (None, 0)


def _persist_reading(reading: ToolUsageReading, saved_at_ms: int) -> None:
    path = _cache_file_path()
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"reading": reading, "saved_at_ms": saved_at_ms}, ensure_ascii=False), encoding="utf-8")
    except OSError as error:
        log_event("claude_usage_persist_failed", detail=str(error))


def _stale_copy(reading: ToolUsageReading, detail: str) -> ToolUsageReading:
    return {
        "ok": reading["ok"],
        "source": f"{reading['source']}-stale",
        "detail": detail,
        "observed_at_ms": reading.get("observed_at_ms", 0),
        "current": reading["current"],
        "weekly": reading["weekly"],
    }


def read_claude_usage_reading(claude_home: Path, current_ms: int, ttl_seconds: int) -> ToolUsageReading:
    # Degradation boundary with cache. The endpoint is queried at most once
    # every ttl_seconds (throttle by last ATTEMPT, whether or not there is data), so as not to
    # hammer the rate-limit (429). On failure the last % obtained is kept.
    global _cached_reading, _last_reading, _last_attempt_ms, _bootstrapped

    ttl_ms = ttl_seconds * 1000

    with _cache_lock:
        # Only once: recovers the last % persisted on disk after a restart.
        if not _bootstrapped:
            persisted_reading, persisted_at_ms = _load_persisted_reading()
            if persisted_reading is not None:
                _cached_reading = persisted_reading
                _last_reading = persisted_reading
                _last_attempt_ms = persisted_at_ms
            _bootstrapped = True

        # Throttle: if it was attempted recently, returns the last result WITHOUT calling.
        if _last_reading is not None and (current_ms - _last_attempt_ms) < ttl_ms:
            return _last_reading

        _last_attempt_ms = current_ms
        client = ClaudeUsageClient(
            credentials_path=claude_home / ".credentials.json",
            request_timeout_seconds=15.0,
            max_attempts=2,
        )

        try:
            payload = client.fetch_usage()
        except UsageSourceError as error:
            if _cached_reading is not None:
                # Keeps the last % obtained (does not reset to manual 100%).
                log_event("claude_usage_stale", detail=str(error))
                result: ToolUsageReading = _stale_copy(_cached_reading, f"Not refreshed ({error})")
            else:
                log_event("claude_usage_unavailable", detail=str(error))
                result = {
                    "ok": False,
                    "source": "claude-oauth-unavailable",
                    "detail": str(error),
                    "observed_at_ms": 0,
                    "current": _empty_window(current_ms, FIVE_HOUR_MINUTES),
                    "weekly": _empty_window(current_ms, SEVEN_DAY_MINUTES),
                }
            # The failure is cached too: it is not retried until the TTL is met.
            _last_reading = result
            return result

        reading = map_claude_usage(payload, current_ms)
        _cached_reading = reading
        _last_reading = reading
        _persist_reading(reading, current_ms)
        return reading
