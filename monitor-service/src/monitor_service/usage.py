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


# Last waiting signature logged per tool, to emit only on transitions.
_last_waiting_signature: dict[str, tuple[str, bool, str]] = {}

# Grace period (s) to hold "busy" after Claude's last real busy. Claude Code oscillates its
# busy/idle status between two tool_use in the same turn; without this linger the ESP animation
# would flicker between working and idle. It is the analog of the open-turn hold that
# Codex already has (_CODEX_OPEN_TASK_BUSY_MAX_SECONDS in collectors.py). The blip between tools
# is sub-second, so 1s is enough to cover it and the real idle shows up almost instantly.
_CLAUDE_BUSY_LINGER_SECONDS = 1
# Last instant (epoch ms) at which "busy" was observed per tool, base of the debounce.
_last_busy_ms: dict[str, int] = {}


def linger_busy(tool: str, raw_busy: bool, current_ms: int, linger_seconds: int) -> bool:
    # Debounce of busy->idle: each real busy refreshes the mark and returns True; when the
    # status drops to idle it keeps reporting busy until the idle persists longer than the
    # grace period, absorbing the momentary flicker between tool_use of the same turn.
    if raw_busy:
        _last_busy_ms[tool] = current_ms
        return True

    last_busy = _last_busy_ms.get(tool)
    if last_busy is None:
        return False

    return (current_ms - last_busy) <= (linger_seconds * 1000)


def resolve_activity(waiting_for_user: bool, busy: bool) -> Activity:
    # Waiting on the user takes precedence over "busy" so the screen warns first.
    if waiting_for_user:
        return "waiting"
    if busy:
        return "busy"
    return "idle"


def _log_waiting_transition(tool: str, waiting: bool, reason: str, detail: str) -> None:
    # Edge-triggered: the long-poll evaluates activity ~4 times/sec; logging every call
    # would flood. It is only emitted when the waiting boolean or its reason changes.
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
    # A single place decides whether Claude is waiting on the user, in this priority order:
    #   1) manual override from the config,
    #   2) "waiting"/waitingFor status in sessions/*.json: this version of Claude exposes here
    #      the real question (AskUserQuestion -> waitingFor "permission prompt"/"dialog open"),
    #   3) structured question in the transcript queue, as a fallback.
    # The signal is direct (no debounce): `get_claude_waiting_status` is already stable because it
    # looks at ANY fresh session (not the "most recent", which oscillated) and the freshness window
    # is wide (1800s). A debounce introduced a delay during which the cached frame and the
    # long-poll disagreed -> the ESP flickered; that is why it was removed.
    if config["claude"]["waiting_for_user"]:
        configured = config["claude"]["status_text"]
        text = configured if configured != "" else "Waiting for your reply"
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
    # The manual config override forces "waiting"; otherwise it is inferred from the rollout
    # (turn in progress => busy, turn just finished => waiting).
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
    # Replaces the manual windows with the real quota when the reading is valid.
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
    # La cuota de Codex se lee de forma pasiva desde el último snapshot persistido. Cuando la
    # hora pasa window_reset_ms no hay evidencia suficiente para decir que volvió a 100%: el
    # siguiente valor real solo aparece cuando Codex escribe otro rate_limits. Por eso, al mover
    # una ventana vencida se conserva el último porcentaje conocido y solo se proyecta el reloj.
    window_length_ms = window_config["window_reset_ms"] - window_config["window_start_ms"]
    if window_length_ms <= 0 or current_ms < window_config["window_reset_ms"]:
        return window_config

    return {
        "remaining_percent": window_config["remaining_percent"],
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
        "window_start_ms": projected["window_start_ms"],
        "window_reset_ms": projected["window_reset_ms"],
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
    fallback_status = source_status if configured_status == "" or configured_status == "No recent data" else configured_status
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
    # Only the activity state (idle/busy/waiting), which is cheap: it reads Claude's
    # sessions/*.json and the tail of Codex's rollout, without scanning the heavy
    # JSONL transcripts of the snapshot. It is what feeds the long-poll channel
    # through which the ESP reacts almost instantly.
    anim_config = config["activity_animation"]
    raw_claude_busy = get_claude_busy(
        claude_home, current_ms, anim_config["stale_seconds"], anim_config["include_claude_subagents"]
    )
    claude_busy = linger_busy("claude", raw_claude_busy, current_ms, _CLAUDE_BUSY_LINGER_SECONDS)
    claude_waiting, _ = resolve_claude_waiting(config, claude_home, current_ms)
    claude_activity = resolve_activity(claude_waiting, claude_busy)

    codex_activity = resolve_codex_activity(config, codex_home, current_ms)

    return (claude_activity, codex_activity)
