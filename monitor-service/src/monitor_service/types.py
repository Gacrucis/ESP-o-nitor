from typing import Literal, TypedDict

ToolId = Literal["claude", "codex"]
Pace = Literal["under", "on_track", "over", "unknown"]
# Estado de actividad de una herramienta en el instante de la lectura:
# idle = sin actividad, busy = pensando/ejecutando ahora, waiting = espera al usuario.
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
    # Resultado de una fuente de cuota real (OAuth de Claude o snapshot de Codex).
    # ok=False indica que se debe degradar a la configuración manual de forma visible.
    ok: bool
    source: str
    detail: str
    # Momento en que se obtuvo la información de cuota (epoch ms): para Claude es el
    # último fetch OAuth exitoso/persistido; para Codex el mtime del rollout leído.
    # 0 indica que no hay una lectura real con fecha conocida.
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
    # Fecha (epoch ms) de la última información de cuota obtenida; 0 si se desconoce.
    usage_observed_at_ms: int
    # Estado de actividad en vivo (idle/busy/waiting) que el firmware usa para
    # animar la esquina y para invertir la pantalla cuando hay espera.
    activity: Activity


class ActivityAnimationConfig(TypedDict):
    # Animación que el ESP32 almacena y renderiza en la esquina superior derecha.
    # El estilo se traduce a una secuencia de frames servida en /api/esp/activity-animation.
    style: str
    interval_ms: int
    invert_on_waiting: bool
    invert_blink_ms: int
    # Tamaño del recuadro de la animación en la esquina superior derecha (px). Permite
    # ajustar dinámicamente cuánto ocupa la animación para que quepa mejor según el tema.
    frame_width: int
    frame_height: int
    # Ventana (s) para considerar a Codex "ocupado" según la frescura de su rollout.
    codex_busy_window_seconds: int
    # Antigüedad (s) tras la cual un estado busy/waiting se considera obsoleto -> idle.
    stale_seconds: int
    # Si los subagentes en background cuentan para la actividad y el conteo de sesiones que
    # alimentan la animación. Codex: rollouts hijos (session_meta thread_source "subagent").
    # Claude: subagentes/workflows del transcript. Por defecto Codex los incluye y Claude no.
    include_codex_subagents: bool
    include_claude_subagents: bool


class ServiceConfig(TypedDict):
    tolerance_percent: float
    theme: str
    frame_cache_ttl_seconds: int
    claude_usage_ttl_seconds: int
    codex_usage_ttl_seconds: int
    activity_animation: ActivityAnimationConfig
    # Salvapantallas que el ESP renderiza cuando el servicio esta caido (anti burn-in).
    screensaver: str
    # Atenuado anti burn-in: tras dim_after_seconds sin cambios en las barras, el ESP
    # baja el brillo a dim_brightness_percent. dim_after_seconds=0 desactiva el atenuado.
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
