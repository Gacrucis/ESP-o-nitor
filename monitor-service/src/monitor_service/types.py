from typing import Literal, TypedDict

ToolId = Literal["claude", "codex"]
Pace = Literal["under", "on_track", "over", "unknown"]
# Activity state of a tool at the instant of the reading:
# idle = no activity, busy = thinking/executing now, waiting = waiting on the user.
Activity = Literal["idle", "busy", "waiting"]


class ToolConfig(TypedDict):
    enabled: bool
    label: str
    current: "UsageWindowConfig"
    weekly: "UsageWindowConfig"
    waiting_for_user: bool
    status_text: str
    manual_override: bool


class UsageWindowConfig(TypedDict):
    remaining_percent: float
    window_start_ms: int
    window_reset_ms: int


class ToolUsageReading(TypedDict):
    # Result of a real quota source (Claude OAuth or Codex snapshot).
    # ok=False indicates it must visibly degrade to the manual configuration.
    ok: bool
    source: str
    detail: str
    # Moment the quota information was obtained (epoch ms): for Claude it is the
    # last successful/persisted OAuth fetch; for Codex the mtime of the read rollout.
    # 0 indicates there is no real reading with a known date.
    observed_at_ms: int
    current: UsageWindowConfig
    weekly: UsageWindowConfig


class UsageWindowSnapshot(TypedDict):
    remaining_percent: float
    expected_remaining_percent: float
    pace: Pace
    reset_in_seconds: int


class ToolSnapshot(TypedDict):
    label: str
    enabled: bool
    remaining_percent: float
    expected_remaining_percent: float
    pace: Pace
    waiting_for_user: bool
    status_text: str
    reset_in_seconds: int
    current: UsageWindowSnapshot
    weekly: UsageWindowSnapshot
    observed_tokens: int
    observed_messages: int
    source: str
    # Date (epoch ms) of the last quota information obtained; 0 if unknown.
    usage_observed_at_ms: int
    # Live activity state (idle/busy/waiting) that the firmware uses to
    # animate the corner and to invert the screen when there is a wait.
    activity: Activity


class ActivityAnimationConfig(TypedDict):
    # Animation that the ESP32 stores and renders in the top-right corner.
    # The style is translated into a frame sequence served at /api/esp/activity-animation.
    style: str
    interval_ms: int
    invert_on_waiting: bool
    invert_blink_ms: int
    # Size of the animation box in the top-right corner (px). Allows
    # dynamically adjusting how much the animation takes up so it fits better per theme.
    frame_width: int
    frame_height: int
    # Window (s) to consider Codex "busy" based on the freshness of its rollout.
    codex_busy_window_seconds: int
    # Age (s) after which a busy/waiting state is considered stale -> idle.
    stale_seconds: int
    # Whether background subagents count toward activity and the session count that
    # feed the animation. Codex: child rollouts (session_meta thread_source "subagent").
    # Claude: subagents/workflows of the transcript. By default Codex includes them and Claude does not.
    include_codex_subagents: bool
    include_claude_subagents: bool


class ServiceConfig(TypedDict):
    tolerance_percent: float
    theme: str
    frame_cache_ttl_seconds: int
    claude_usage_ttl_seconds: int
    codex_usage_ttl_seconds: int
    activity_animation: ActivityAnimationConfig
    # Screensaver that the ESP renders when the service is down (anti burn-in).
    screensaver: str
    # Anti burn-in dimming: after dim_after_seconds without changes in the bars, the ESP
    # lowers the brightness to dim_brightness_percent. dim_after_seconds=0 disables dimming.
    dim_after_seconds: int
    dim_brightness_percent: int
    claude: ToolConfig
    codex: ToolConfig


class EspSnapshot(TypedDict):
    updated_at_ms: int
    tolerance_percent: float
    claude: ToolSnapshot
    codex: ToolSnapshot


class RuntimeStatus(TypedDict):
    service: str
    claude_home: str
    codex_home: str
    config_path: str
    claude_home_exists: bool
    codex_home_exists: bool
    config_exists: bool
    claude_credentials_exists: bool
    codex_sessions_exists: bool
    claude_usage_source: str
    codex_usage_source: str
    frame_cache_ttl_seconds: int
    claude_usage_ttl_seconds: int
    codex_usage_ttl_seconds: int
