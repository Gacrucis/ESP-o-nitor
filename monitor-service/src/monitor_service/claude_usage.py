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

# Endpoint de solo lectura que usa Claude Code para `/usage`. No consume cuota.
CLAUDE_USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
OAUTH_BETA_HEADER = "oauth-2025-04-20"
USER_AGENT = "esp-o-nitor-service/0.1"

# Duración de las ventanas reportadas por el endpoint.
FIVE_HOUR_MINUTES = 300
SEVEN_DAY_MINUTES = 10080

# Claves candidatas donde Claude Code guarda el bloque OAuth en .credentials.json.
OAUTH_CONTAINER_KEYS = ("claudeAiOauth", "oauth", "claude_ai_oauth")
ACCESS_TOKEN_KEYS = ("accessToken", "access_token")
EXPIRES_AT_KEYS = ("expiresAt", "expires_at")

# El endpoint de uso tiene su propio rate-limit: se consulta como máximo cada
# ttl_seconds (default vía .env CLAUDE_USAGE_TTL_SECONDS, editable en la web).
# Tras un intento (exitoso o fallido) se respeta el TTL para no martillar el 429,
# y ante fallo se conserva indefinidamente el último % obtenido.
_cache_lock = threading.Lock()
_cached_reading: ToolUsageReading | None = None  # última lectura BUENA (para conservar el %)
_last_reading: ToolUsageReading | None = None  # último resultado devuelto (bueno o fallo)
_last_attempt_ms = 0
_bootstrapped = False


class UsageSourceError(RuntimeError):
    # Error base de una fuente de cuota real.
    pass


class CredentialMissingError(UsageSourceError):
    # No existe el archivo de credenciales o no contiene un access token.
    pass


class CredentialExpiredError(UsageSourceError):
    # El access token venció; Claude Code debe refrescarlo en su próximo uso.
    pass


class RateLimitedError(UsageSourceError):
    # El endpoint de uso respondió 429; no se reintenta, se sirve caché.
    pass


class ClaudeUsageClient:
    # Conector HTTP hacia el endpoint de uso de la suscripción de Claude.
    def __init__(self, credentials_path: Path, request_timeout_seconds: float, max_attempts: int) -> None:
        self._credentials_path = credentials_path
        self._request_timeout_seconds = request_timeout_seconds
        self._max_attempts = max_attempts

    def _read_access_token(self) -> tuple[str, int]:
        # Devuelve (token, expires_at_ms). expires_at_ms es 0 si no se conoce.
        if not self._credentials_path.exists():
            raise CredentialMissingError(f"No existe credenciales en {self._credentials_path}")

        try:
            raw = json.loads(self._credentials_path.read_text(encoding="utf-8"))
        except JSONDecodeError as error:
            raise UsageSourceError(f"Credenciales con JSON inválido en {self._credentials_path}: {error}") from error
        except OSError as error:
            raise UsageSourceError(f"No se pudo leer {self._credentials_path}: {error}") from error

        if not isinstance(raw, dict):
            raise UsageSourceError(f"{self._credentials_path} debe contener un objeto JSON")

        container = self._extract_oauth_container(cast(dict[str, Any], raw))
        token = self._first_value(container, ACCESS_TOKEN_KEYS)
        if not isinstance(token, str) or token == "":
            raise CredentialMissingError("No se encontró accessToken en las credenciales de Claude")

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
            raise CredentialExpiredError("Token de Claude expirado; abre Claude Code para refrescarlo")

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
        last_error: Exception = UsageSourceError("Sin intentos ejecutados")
        for attempt in range(1, self._max_attempts + 1):
            try:
                with urllib.request.urlopen(request, timeout=self._request_timeout_seconds) as response:
                    body = response.read()
                parsed = json.loads(body.decode("utf-8"))
                if not isinstance(parsed, dict):
                    raise UsageSourceError("El endpoint de uso no devolvió un objeto JSON")
                return cast(dict[str, Any], parsed)
            except urllib.error.HTTPError as error:
                # 401/403 no se reintentan: la credencial es el problema.
                if error.code in (401, 403):
                    raise CredentialExpiredError(f"Claude rechazó la credencial (HTTP {error.code})") from error
                # 429 no se reintenta: reintentar empeora el rate-limit; se sirve caché.
                if error.code == 429:
                    raise RateLimitedError("Endpoint de uso de Claude con rate-limit (HTTP 429)") from error
                last_error = error
                log_event("claude_usage_http_retry", attempt=attempt, status=error.code)
            except (urllib.error.URLError, TimeoutError, JSONDecodeError, OSError) as error:
                last_error = error
                log_event("claude_usage_request_retry", attempt=attempt, detail=str(error))

            if attempt < self._max_attempts:
                time.sleep(float(attempt))

        raise UsageSourceError(f"Falló la consulta de uso de Claude tras {self._max_attempts} intentos: {last_error}") from last_error


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
        "detail": "Cuota real de la suscripción de Claude",
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
    # Persistencia junto al config.json, en el volumen de datos.
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
        # Backfill para cachés escritos antes de existir observed_at_ms: se usa la
        # fecha de guardado como mejor estimación de cuándo se obtuvo la cuota.
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
    # Frontera de degradación con caché. Se consulta el endpoint a lo sumo una vez
    # cada ttl_seconds (throttle por último INTENTO, haya o no dato), para no
    # martillear el rate-limit (429). Ante fallo se conserva el último % obtenido.
    global _cached_reading, _last_reading, _last_attempt_ms, _bootstrapped

    ttl_ms = ttl_seconds * 1000

    with _cache_lock:
        # Una sola vez: recupera el último % persistido en disco tras un reinicio.
        if not _bootstrapped:
            persisted_reading, persisted_at_ms = _load_persisted_reading()
            if persisted_reading is not None:
                _cached_reading = persisted_reading
                _last_reading = persisted_reading
                _last_attempt_ms = persisted_at_ms
            _bootstrapped = True

        # Throttle: si se intentó hace poco, devuelve el último resultado SIN llamar.
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
                # Conserva el último % obtenido (no se reinicia a 100% manual).
                log_event("claude_usage_stale", detail=str(error))
                result: ToolUsageReading = _stale_copy(_cached_reading, f"Sin refrescar ({error})")
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
            # Se cachea también el fallo: no se reintenta hasta cumplir el TTL.
            _last_reading = result
            return result

        reading = map_claude_usage(payload, current_ms)
        _cached_reading = reading
        _last_reading = reading
        _persist_reading(reading, current_ms)
        return reading
