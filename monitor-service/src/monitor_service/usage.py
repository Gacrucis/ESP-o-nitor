from pathlib import Path

from monitor_service.claude_usage import read_claude_usage_reading
from monitor_service.collectors import (
    get_claude_busy,
    get_claude_observed_usage,
    get_claude_question_status,
    get_claude_waiting_status,
    get_codex_activity,
    get_codex_observed_usage,
    read_codex_rate_limits_reading,
)
from monitor_service.config import clamp_percent, now_ms
from monitor_service.logutil import log_event
from monitor_service.types import (
    Activity,
    Pace,
    ServiceConfig,
    ToolConfig,
    ToolSnapshot,
    ToolUsageReading,
    UsageWindowConfig,
    UsageWindowSnapshot,
)


# Ultima firma de espera logueada por herramienta, para emitir solo en transiciones.
_last_waiting_signature: dict[str, tuple[str, bool, str]] = {}

# Gracia (s) para sostener "busy" tras el ultimo busy real de Claude. Claude Code oscila su
# status busy/idle entre dos tool_use del mismo turno; sin este linger la animacion del ESP
# parpadearia entre trabajando e idle. Es el analogo al sostenimiento de turno abierto que
# Codex ya tiene (_CODEX_OPEN_TASK_BUSY_MAX_SECONDS en collectors.py). El blip entre tools es
# sub-segundo, asi que 1s basta para taparlo y el idle real aparece casi al instante.
_CLAUDE_BUSY_LINGER_SECONDS = 1
# Ultimo instante (epoch ms) en que se observo "busy" por herramienta, base del debounce.
_last_busy_ms: dict[str, int] = {}


def linger_busy(tool: str, raw_busy: bool, current_ms: int, linger_seconds: int) -> bool:
    # Debounce de busy->idle: cada busy real refresca la marca y devuelve True; cuando el
    # status cae a idle se sigue reportando busy hasta que el idle persista mas que la
    # gracia, absorbiendo el parpadeo momentaneo entre tool_use de un mismo turno.
    if raw_busy:
        _last_busy_ms[tool] = current_ms
        return True

    last_busy = _last_busy_ms.get(tool)
    if last_busy is None:
        return False

    return (current_ms - last_busy) <= (linger_seconds * 1000)


def resolve_activity(waiting_for_user: bool, busy: bool) -> Activity:
    # La espera al usuario manda sobre el "ocupado" para que la pantalla avise primero.
    if waiting_for_user:
        return "waiting"
    if busy:
        return "busy"
    return "idle"


def _log_waiting_transition(tool: str, waiting: bool, reason: str, detail: str) -> None:
    # Edge-triggered: el long-poll evalua la actividad ~4 veces/seg; loguear cada llamada
    # inundaria. Solo se emite cuando cambia el booleano de espera o su motivo.
    global _last_waiting_signature
    signature = (tool, waiting, reason)
    if _last_waiting_signature.get(tool) == signature:
        return
    _last_waiting_signature[tool] = signature
    log_event(
        "activity_waiting_on" if waiting else "activity_waiting_off",
        tool=tool,
        reason=reason,
        detail=detail[:160],
    )


def resolve_claude_waiting(config: ServiceConfig, claude_home: Path, current_ms: int) -> tuple[bool, str]:
    # Un solo lugar decide si Claude espera al usuario, en este orden de prioridad:
    #   1) override manual de la config,
    #   2) estado "waiting"/waitingFor en sessions/*.json: esta version de Claude expone aqui
    #      la pregunta real (AskUserQuestion -> waitingFor "permission prompt"/"dialog open"),
    #   3) pregunta estructurada en la cola del transcript, como respaldo.
    # La senal es directa (sin debounce): `get_claude_waiting_status` ya es estable porque mira
    # CUALQUIER sesion fresca (no la "mas reciente", que oscilaba) y la ventana de frescura es
    # amplia (1800s). Un debounce introducia un retardo durante el cual el frame cacheado y el
    # long-poll discrepaban -> el ESP parpadeaba; por eso se eliminó.
    if config["claude"]["waiting_for_user"]:
        configured = config["claude"]["status_text"]
        text = configured if configured != "" else "Esperando tu respuesta"
        _log_waiting_transition("claude", True, "config_override", text)
        return (True, text)

    stale_seconds = config["activity_animation"]["stale_seconds"]
    status_waiting, status_text = get_claude_waiting_status(claude_home, current_ms, stale_seconds)
    if status_waiting:
        _log_waiting_transition("claude", True, "session_status", status_text)
        return (True, status_text)

    question_waiting, question_text = get_claude_question_status(claude_home, current_ms, stale_seconds)
    if question_waiting:
        _log_waiting_transition("claude", True, "transcript_question", question_text)
        return (True, question_text)

    _log_waiting_transition("claude", False, "none", status_text)
    return (False, status_text)


def resolve_codex_activity(config: ServiceConfig, codex_home: Path, current_ms: int) -> Activity:
    # El override manual de la config fuerza "waiting"; si no, se infiere del rollout
    # (turno en curso => busy, turno recién terminado => waiting).
    if config["codex"]["waiting_for_user"]:
        _log_waiting_transition("codex", True, "config_override", "")
        return "waiting"
    anim_config = config["activity_animation"]
    activity = get_codex_activity(
        codex_home,
        current_ms,
        anim_config["codex_busy_window_seconds"],
        anim_config["stale_seconds"],
        anim_config["include_codex_subagents"],
    )
    if activity == "waiting":
        _log_waiting_transition("codex", True, "rollout_inferred", "")
    else:
        _log_waiting_transition("codex", False, "none", activity)
    return activity


def apply_usage_reading(tool_config: ToolConfig, reading: ToolUsageReading) -> ToolConfig:
    # Reemplaza las ventanas manuales por la cuota real cuando la lectura es válida.
    if not reading["ok"]:
        return tool_config

    return {
        "enabled": tool_config["enabled"],
        "label": tool_config["label"],
        "current": reading["current"],
        "weekly": reading["weekly"],
        "waiting_for_user": tool_config["waiting_for_user"],
        "status_text": tool_config["status_text"],
        "manual_override": False,
    }


def resolve_status_text(waiting_for_user: bool, waiting_status: str, reading: ToolUsageReading, configured_status: str) -> str:
    if waiting_for_user:
        return waiting_status
    if not reading["ok"]:
        return reading["detail"]
    return configured_status


def calculate_expected_remaining_percent(start_ms: int, reset_ms: int, current_ms: int) -> float:
    if reset_ms <= start_ms:
        return 0.0

    remaining_ms = reset_ms - current_ms
    total_ms = reset_ms - start_ms

    return clamp_percent((remaining_ms * 100.0) / total_ms)


def calculate_pace(actual_remaining: float, expected_remaining: float, tolerance_percent: float) -> Pace:
    if actual_remaining > expected_remaining + tolerance_percent:
        return "under"
    if actual_remaining < expected_remaining - tolerance_percent:
        return "over"

    return "on_track"


def calculate_reset_seconds(reset_ms: int, current_ms: int) -> int:
    remaining_ms = reset_ms - current_ms
    if remaining_ms <= 0:
        return 0

    return int(remaining_ms / 1000)


def build_tool_snapshot(
    tool_config: ToolConfig,
    current_ms: int,
    tolerance_percent: float,
    waiting_for_user: bool,
    status_text: str,
    observed_tokens: int,
    observed_messages: int,
    source: str,
    usage_observed_at_ms: int,
    activity: Activity,
) -> ToolSnapshot:
    current_window = build_usage_window_snapshot(tool_config["current"], current_ms, tolerance_percent)
    weekly_window = build_usage_window_snapshot(tool_config["weekly"], current_ms, tolerance_percent)

    return {
        "label": tool_config["label"],
        "enabled": tool_config["enabled"],
        "remaining_percent": current_window["remaining_percent"],
        "expected_remaining_percent": current_window["expected_remaining_percent"],
        "pace": current_window["pace"],
        "waiting_for_user": waiting_for_user,
        "status_text": status_text,
        "reset_in_seconds": current_window["reset_in_seconds"],
        "current": current_window,
        "weekly": weekly_window,
        "observed_tokens": observed_tokens,
        "observed_messages": observed_messages,
        "source": source,
        "usage_observed_at_ms": usage_observed_at_ms,
        "activity": activity,
    }


def project_window_reset(window_config: UsageWindowConfig, current_ms: int) -> UsageWindowConfig:
    # La cuota de Codex se lee de forma pasiva: queda congelada en el snapshot de la ultima
    # request. Cuando el reloj supera window_reset_ms la ventana ya rodo y el bucket vuelve a
    # 100%, pero no habria una lectura nueva que lo refleje: al llegar a 0% no se pueden enviar
    # mensajes, asi que nunca se generaria un rollout con el reinicio.
    #
    # La ventana de Codex no esta anclada a un reloj absoluto: el contador arranca con el
    # PRIMER mensaje de la nueva ventana y vence window_length despues. Mientras no haya
    # mensajes la ventana no ha empezado, asi que se muestra fresca desde el ahora (restante
    # 100% y "se reinicia en 5h" de forma constante) hasta que un rollout real traiga el
    # resets_at verdadero del primer mensaje.
    window_length_ms = window_config["window_reset_ms"] - window_config["window_start_ms"]
    if window_length_ms <= 0 or current_ms < window_config["window_reset_ms"]:
        return window_config

    return {
        "remaining_percent": 100.0,
        "window_start_ms": current_ms,
        "window_reset_ms": current_ms + window_length_ms,
    }


def build_usage_window_snapshot(
    window_config: UsageWindowConfig,
    current_ms: int,
    tolerance_percent: float,
) -> UsageWindowSnapshot:
    projected = project_window_reset(window_config, current_ms)
    expected_remaining = calculate_expected_remaining_percent(
        projected["window_start_ms"],
        projected["window_reset_ms"],
        current_ms,
    )
    remaining_percent = clamp_percent(projected["remaining_percent"])

    return {
        "remaining_percent": remaining_percent,
        "expected_remaining_percent": expected_remaining,
        "pace": calculate_pace(remaining_percent, expected_remaining, tolerance_percent),
        "reset_in_seconds": calculate_reset_seconds(projected["window_reset_ms"], current_ms),
    }


def build_claude_snapshot(config: ServiceConfig, claude_home: Path, current_ms: int) -> ToolSnapshot:
    raw_busy = get_claude_busy(
        claude_home,
        current_ms,
        config["activity_animation"]["stale_seconds"],
        config["activity_animation"]["include_claude_subagents"],
    )
    busy = linger_busy("claude", raw_busy, current_ms, _CLAUDE_BUSY_LINGER_SECONDS)
    waiting_for_user, waiting_status = resolve_claude_waiting(config, claude_home, current_ms)
    observed_tokens, observed_messages = get_claude_observed_usage(claude_home)
    tool_config = config["claude"]
    reading = read_claude_usage_reading(claude_home, current_ms, config["claude_usage_ttl_seconds"])
    effective_config = apply_usage_reading(tool_config, reading)
    status_text = resolve_status_text(waiting_for_user, waiting_status, reading, tool_config["status_text"])

    return build_tool_snapshot(
        effective_config,
        current_ms,
        config["tolerance_percent"],
        waiting_for_user,
        status_text,
        observed_tokens,
        observed_messages,
        reading["source"],
        reading["observed_at_ms"],
        resolve_activity(waiting_for_user, busy),
    )


def build_codex_snapshot(config: ServiceConfig, codex_home: Path, current_ms: int) -> ToolSnapshot:
    observed_tokens, observed_messages, source_status = get_codex_observed_usage(codex_home)
    tool_config = config["codex"]
    reading = read_codex_rate_limits_reading(codex_home, current_ms, config["codex_usage_ttl_seconds"])
    effective_config = apply_usage_reading(tool_config, reading)
    configured_status = tool_config["status_text"]
    fallback_status = source_status if configured_status == "" or configured_status == "Sin datos recientes" else configured_status
    codex_activity = resolve_codex_activity(config, codex_home, current_ms)
    status_text = resolve_status_text(codex_activity == "waiting", fallback_status, reading, fallback_status)

    return build_tool_snapshot(
        effective_config,
        current_ms,
        config["tolerance_percent"],
        codex_activity == "waiting",
        status_text,
        observed_tokens,
        observed_messages,
        reading["source"],
        reading["observed_at_ms"],
        codex_activity,
    )


def build_snapshot(config: ServiceConfig, claude_home: Path, codex_home: Path) -> dict[str, object]:
    current_ms = now_ms()

    return {
        "updated_at_ms": current_ms,
        "tolerance_percent": config["tolerance_percent"],
        "claude": build_claude_snapshot(config, claude_home, current_ms),
        "codex": build_codex_snapshot(config, codex_home, current_ms),
    }


def compute_activity_states(
    config: ServiceConfig, claude_home: Path, codex_home: Path, current_ms: int
) -> tuple[Activity, Activity]:
    # Solo el estado de actividad (idle/busy/waiting), que es barato: lee los
    # sessions/*.json de Claude y el tail del rollout de Codex, sin escanear los
    # transcripts JSONL pesados del snapshot. Es lo que alimenta el canal de
    # long-poll con el que el ESP reacciona casi al instante.
    anim_config = config["activity_animation"]
    raw_claude_busy = get_claude_busy(
        claude_home, current_ms, anim_config["stale_seconds"], anim_config["include_claude_subagents"]
    )
    claude_busy = linger_busy("claude", raw_claude_busy, current_ms, _CLAUDE_BUSY_LINGER_SECONDS)
    claude_waiting, _ = resolve_claude_waiting(config, claude_home, current_ms)
    claude_activity = resolve_activity(claude_waiting, claude_busy)

    codex_activity = resolve_codex_activity(config, codex_home, current_ms)

    return (claude_activity, codex_activity)
