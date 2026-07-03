import json
import threading
from json import JSONDecodeError
from pathlib import Path
from typing import Any, TypedDict, cast

from monitor_service.config import clamp_percent
from monitor_service.logutil import log_event
from monitor_service.types import ToolUsageReading, UsageWindowConfig

# Minutos por ventana de Codex (primary = 5h, secondary = semanal).
CODEX_PRIMARY_MINUTES = 300
CODEX_SECONDARY_MINUTES = 10080
_codex_cache_lock = threading.Lock()
_codex_last_reading: ToolUsageReading | None = None
_codex_last_attempt_ms = 0


def read_json_object(path: Path) -> dict[str, Any]:
    try:
        raw_value = json.loads(path.read_text(encoding="utf-8", errors="replace"))
    except JSONDecodeError as error:
        raise RuntimeError(f"JSON inválido en {path}: {error}") from error
    except OSError as error:
        raise RuntimeError(f"No se pudo leer {path}: {error}") from error

    if not isinstance(raw_value, dict):
        raise RuntimeError(f"{path} debe contener un objeto JSON")

    return cast(dict[str, Any], raw_value)


def get_claude_waiting_status(claude_home: Path, current_ms: int, stale_seconds: int) -> tuple[bool, str]:
    # Espera si CUALQUIER sesion fresca pide input, no solo la mas reciente: el detector debe
    # avisar de cualquier ventana/job que requiera intervencion, y muchas veces esa pregunta
    # esta en otra sesion. Mirar solo la mas reciente producia un flash: mientras un chat tiene
    # el dialogo abierto, otra sesion trabajando en paralelo "ganaba" por updatedAt y apagaba la
    # espera, para volver a encenderse al siguiente sondeo. El filtro de frescura descarta
    # sesiones obsoletas. Se reporta el texto de la sesion en espera actualizada mas recientemente.
    sessions_path = claude_home / "sessions"
    if not sessions_path.exists():
        return (False, "No existe sessions/")

    stale_ms = stale_seconds * 1000
    newest_updated_at = -1
    newest_status = "Sin sesiones activas"
    newest_waiting_updated_at = -1
    waiting_text = ""
    any_waiting = False

    for session_path in sorted(sessions_path.glob("*.json")):
        session = read_json_object(session_path)
        updated_at = int(session.get("updatedAt", 0))
        if (current_ms - updated_at) > stale_ms:
            continue
        status = str(session.get("status", "unknown"))
        waiting_for = session.get("waitingFor")
        session_text = str(waiting_for) if waiting_for is not None else status

        if updated_at > newest_updated_at:
            newest_updated_at = updated_at
            newest_status = session_text

        if status == "waiting" or waiting_for is not None:
            any_waiting = True
            if updated_at > newest_waiting_updated_at:
                newest_waiting_updated_at = updated_at
                waiting_text = session_text

    if any_waiting:
        return (True, waiting_text)

    return (False, newest_status)


def get_claude_busy(claude_home: Path, current_ms: int, stale_seconds: int, include_subagents: bool) -> bool:
    # Claude Code mantiene en sessions/<pid>.json un campo status ("busy"/"idle"/"waiting").
    # Busy si CUALQUIER sesión no obsoleta está "busy", no solo la de updatedAt más
    # reciente: abrir o resolver otro chat no debe apagar la animación mientras un chat
    # previo sigue pensando. Con include_subagents activo, tambien cuenta como busy una sesion
    # idle con un workflow o subagentes corriendo en background: el orquestador cerro turno
    # pero espera a que terminen, asi que para el medidor sigue trabajando.
    sessions_path = claude_home / "sessions"
    if not sessions_path.exists():
        return False

    stale_ms = stale_seconds * 1000
    for session_path in sorted(sessions_path.glob("*.json")):
        session = read_json_object(session_path)
        updated_at = int(session.get("updatedAt", 0))
        if (current_ms - updated_at) > stale_ms:
            continue

        if str(session.get("status", "idle")) == "busy":
            return True

        if not include_subagents:
            continue
        session_id = str(session.get("sessionId", ""))
        if session_id == "":
            continue
        transcript_path = _find_claude_transcript(claude_home, session_id)
        if transcript_path is None:
            continue
        if _session_has_active_subagents(transcript_path, current_ms, stale_ms):
            return True

    return False


def count_busy_claude_sessions(claude_home: Path, current_ms: int, stale_seconds: int) -> int:
    # Cuenta sessionIds DISTINTOS con status "busy" dentro de la ventana de frescura: cuantas
    # sesiones de Claude estan trabajando en paralelo ahora mismo. Alimenta el indicador de
    # puntos del ESP (2 puntos por sesion).
    sessions_path = claude_home / "sessions"
    if not sessions_path.exists():
        return 0

    stale_ms = stale_seconds * 1000
    busy_ids: set[str] = set()
    for session_path in sorted(sessions_path.glob("*.json")):
        session = read_json_object(session_path)
        updated_at = int(session.get("updatedAt", 0))
        if (current_ms - updated_at) > stale_ms:
            continue
        if str(session.get("status", "idle")) != "busy":
            continue
        session_id = str(session.get("sessionId", ""))
        if session_id == "":
            continue
        busy_ids.add(session_id)

    return len(busy_ids)


# Herramientas con las que Claude Code pide intervencion explicita del usuario: una
# pregunta estructurada o la aprobacion de un plan. Son senal de alta precision de "waiting".
_CLAUDE_QUESTION_TOOLS = frozenset({"AskUserQuestion", "ExitPlanMode"})
# Solo se lee la cola del transcript: el cierre del ultimo turno cabe de sobra en este
# tope y evita recorrer megabytes de historial en cada sondeo del long-poll.
_CLAUDE_TRANSCRIPT_TAIL_BYTES = 16384


def _newest_active_session(claude_home: Path, current_ms: int, stale_ms: int) -> dict[str, Any] | None:
    # La sesion con updatedAt mas reciente dentro de la ventana es con la que el usuario
    # interactua ahora; las preguntas se evaluan solo sobre ella.
    sessions_path = claude_home / "sessions"
    if not sessions_path.exists():
        return None

    newest_session: dict[str, Any] | None = None
    newest_updated_at = -1
    for session_path in sorted(sessions_path.glob("*.json")):
        session = read_json_object(session_path)
        updated_at = int(session.get("updatedAt", 0))
        if (current_ms - updated_at) > stale_ms:
            continue
        if updated_at > newest_updated_at:
            newest_updated_at = updated_at
            newest_session = session

    return newest_session


def _find_claude_transcript(claude_home: Path, session_id: str) -> Path | None:
    # El transcript vive en projects/<cwd-codificado>/<sessionId>.jsonl; no se conoce el
    # directorio codificado, asi que se localiza por nombre de archivo (el sessionId).
    projects_path = claude_home / "projects"
    if not projects_path.exists():
        return None

    matches = list(projects_path.glob(f"*/{session_id}.jsonl"))
    if len(matches) == 0:
        return None

    return max(matches, key=lambda path: path.stat().st_mtime)


def _session_has_active_subagents(transcript_path: Path, current_ms: int, stale_ms: int) -> bool:
    # Los subagentes y workflows escriben en <sessionId>/subagents/**/*.jsonl, hermano del
    # transcript principal <sessionId>.jsonl. Si alguno se escribio dentro de la ventana de
    # frescura, el orquestador esta supervisando trabajo en curso (no espera al usuario):
    # sirve para descartar el falso "waiting" que produce un cierre de turno terminado en "?"
    # mientras un workflow corre en background.
    subagents_path = transcript_path.with_suffix("") / "subagents"
    if not subagents_path.exists():
        return False

    for candidate in subagents_path.rglob("*.jsonl"):
        try:
            mtime_ms = int(candidate.stat().st_mtime * 1000)
        except OSError as error:
            log_event("claude_subagent_stat_failed", path=str(candidate), detail=str(error))
            continue
        if (current_ms - mtime_ms) <= stale_ms:
            return True

    return False


def _read_transcript_tail_events(path: Path, max_bytes: int) -> list[dict[str, Any]]:
    try:
        with path.open("rb") as handle:
            file_size = path.stat().st_size
            if file_size > max_bytes:
                handle.seek(file_size - max_bytes)
                # La primera linea tras el salto suele quedar partida; se descarta.
                handle.readline()
            raw = handle.read()
    except OSError as error:
        raise RuntimeError(f"No se pudo leer {path}: {error}") from error

    events: list[dict[str, Any]] = []
    for raw_line in raw.split(b"\n"):
        if raw_line.strip() == b"":
            continue
        try:
            event = json.loads(raw_line)
        except JSONDecodeError:
            continue
        if isinstance(event, dict):
            events.append(cast(dict[str, Any], event))

    return events


def _assistant_event_question_text(event: dict[str, Any]) -> str | None:
    # Devuelve el texto de espera solo si el evento cierra turno con una herramienta de
    # pregunta estructurada (AskUserQuestion/ExitPlanMode); None en cualquier otro caso. La
    # prosa terminada en "?" NO cuenta: el monitor avisa unicamente ante el popup real, que es
    # senal de alta precision y no produce los falsos "waiting" de las preguntas retoricas o de
    # cortesia con las que Claude suele cerrar un turno.
    if event.get("type") != "assistant":
        return None

    message = event.get("message")
    if not isinstance(message, dict):
        return None

    content = message.get("content")
    if not isinstance(content, list):
        return None

    for block in content:
        if not isinstance(block, dict):
            continue
        if block.get("type") != "tool_use":
            continue
        tool_name = block.get("name")
        if tool_name == "ExitPlanMode":
            return "Esperando aprobacion del plan"
        if tool_name == "AskUserQuestion":
            return "Esperando tu respuesta"
        # Cualquier otra herramienta significa que Claude va a ejecutarla: sigue ocupado.
        return None

    return None


def get_claude_question_status(claude_home: Path, current_ms: int, stale_seconds: int) -> tuple[bool, str]:
    # Para detectar que Claude te pregunta algo se inspecciona la cola del transcript de la
    # sesion activa y se busca el cierre de turno con una herramienta de pregunta estructurada
    # (AskUserQuestion/ExitPlanMode). La prosa terminada en "?" no se considera.
    stale_ms = stale_seconds * 1000
    session = _newest_active_session(claude_home, current_ms, stale_ms)
    if session is None:
        return (False, "")

    session_id = str(session.get("sessionId", ""))
    if session_id == "":
        return (False, "")

    transcript_path = _find_claude_transcript(claude_home, session_id)
    if transcript_path is None:
        return (False, "")

    events = _read_transcript_tail_events(transcript_path, _CLAUDE_TRANSCRIPT_TAIL_BYTES)
    for event in reversed(events):
        # Los sub-agentes (Task) escriben en el mismo transcript; su turno no representa
        # una pregunta al usuario, asi que se ignoran al buscar el cierre del turno real.
        if event.get("isSidechain") is True:
            continue
        question_text = _assistant_event_question_text(event)
        if question_text is None:
            return (False, "")
        return (True, question_text)

    return (False, "")


def get_claude_observed_usage(claude_home: Path) -> tuple[int, int]:
    projects_path = claude_home / "projects"
    if not projects_path.exists():
        return (0, 0)

    total_tokens = 0
    total_messages = 0

    for jsonl_path in sorted(projects_path.glob("*/*.jsonl")):
        try:
            handle = jsonl_path.open("r", encoding="utf-8", errors="replace")
        except OSError as error:
            raise RuntimeError(f"No se pudo leer {jsonl_path}: {error}") from error

        with handle:
            for line in handle:
                if line.strip() == "":
                    continue

                try:
                    event = json.loads(line)
                except JSONDecodeError:
                    continue

                if not isinstance(event, dict):
                    continue

                message = event.get("message")
                if not isinstance(message, dict):
                    continue

                usage = message.get("usage")
                if not isinstance(usage, dict):
                    continue

                total_messages += 1
                total_tokens += int(usage.get("input_tokens", 0))
                total_tokens += int(usage.get("output_tokens", 0))
                total_tokens += int(usage.get("cache_creation_input_tokens", 0))
                total_tokens += int(usage.get("cache_read_input_tokens", 0))

    return (total_tokens, total_messages)


def get_codex_observed_usage(codex_home: Path) -> tuple[int, int, str]:
    session_index_path = codex_home / "session_index.jsonl"
    if not session_index_path.exists():
        return (0, 0, "Sin session_index.jsonl")

    try:
        with session_index_path.open("r", encoding="utf-8", errors="replace") as handle:
            message_count = sum(1 for line in handle if line.strip() != "")
    except OSError as error:
        raise RuntimeError(f"No se pudo leer {session_index_path}: {error}") from error

    return (0, message_count, "Índice local leído; uso porcentual manual")


# Cantidad maxima de rollouts recientes que se inspeccionan buscando la cuota del modelo
# principal. Si el rollout mas nuevo fue una sesion del modelo "spark" (cuota aparte), se
# sigue retrocediendo hasta hallar una sesion del modelo principal.
_CODEX_RATE_LIMIT_CANDIDATE_MAX = 24


def _is_spark_model(model: str) -> bool:
    # "spark" (gpt-5.x-codex-spark) consume un bucket de cuota separado del modelo
    # principal de Codex; su porcentaje no debe contar para el medidor.
    return "spark" in model.lower()


def _rollouts_by_mtime_desc(sessions_path: Path, limit: int) -> list[Path]:
    candidates: list[tuple[float, Path]] = []
    for candidate in sessions_path.rglob("*.jsonl"):
        try:
            mtime = candidate.stat().st_mtime
        except OSError as error:
            log_event("codex_rollout_stat_failed", path=str(candidate), detail=str(error))
            continue
        candidates.append((mtime, candidate))

    candidates.sort(key=lambda item: item[0], reverse=True)
    return [candidate for _, candidate in candidates[:limit]]


def _find_rate_limits(node: Any) -> dict[str, Any] | None:
    # Busca recursivamente el bloque rate_limits dentro de un evento de rollout.
    if isinstance(node, dict):
        rate_limits = node.get("rate_limits")
        if isinstance(rate_limits, dict):
            return cast(dict[str, Any], rate_limits)
        for value in node.values():
            found = _find_rate_limits(value)
            if found is not None:
                return found
    elif isinstance(node, list):
        for value in node:
            found = _find_rate_limits(value)
            if found is not None:
                return found
    return None


def _rollout_principal_rate_limits(rollout_path: Path) -> dict[str, Any] | None:
    # Devuelve la ultima cuota del modelo PRINCIPAL en este rollout, ignorando los turnos
    # del modelo "spark" (su % cuenta contra un bucket de cuota aparte).
    #
    # Notas del formato del rollout:
    #   - El modelo activo se declara en eventos turn_context (payload.model) y aplica a los
    #     token_count (que cargan rate_limits) que vienen despues.
    #   - Codex intercala dos tipos de rate_limits: las ventanas de uso reales (limit_id
    #     "codex", con primary/secondary poblados) y las de creditos (limit_id "premium",
    #     con primary/secondary en null). Se rastrean por separado el primary y el secondary
    #     no nulos mas recientes para que un snapshot de creditos no pise la cuota real.
    try:
        handle = rollout_path.open("r", encoding="utf-8", errors="replace")
    except OSError as error:
        raise RuntimeError(f"No se pudo leer {rollout_path}: {error}") from error

    current_model = ""
    latest_primary: dict[str, Any] | None = None
    latest_secondary: dict[str, Any] | None = None
    with handle:
        for line in handle:
            has_model = '"model"' in line
            has_rate_limits = '"rate_limits"' in line
            if not has_model and not has_rate_limits:
                continue
            try:
                event = json.loads(line)
            except JSONDecodeError:
                continue

            if event.get("type") == "turn_context":
                payload = event.get("payload")
                if isinstance(payload, dict):
                    model = payload.get("model")
                    if isinstance(model, str):
                        current_model = model
                continue

            if not has_rate_limits:
                continue
            # La cuota del modelo spark no debe contar; se conserva la del principal.
            if _is_spark_model(current_model):
                continue
            found = _find_rate_limits(event)
            if found is None:
                continue
            primary = found.get("primary")
            if isinstance(primary, dict):
                latest_primary = cast(dict[str, Any], primary)
            secondary = found.get("secondary")
            if isinstance(secondary, dict):
                latest_secondary = cast(dict[str, Any], secondary)

    if latest_primary is None and latest_secondary is None:
        return None

    return {"primary": latest_primary, "secondary": latest_secondary}


def _newest_principal_rate_limits(sessions_path: Path) -> tuple[Path, dict[str, Any]] | None:
    # Recorre los rollouts del mas nuevo al mas viejo y devuelve la primera cuota del modelo
    # principal hallada. Si la ultima sesion fue de spark, retrocede hasta una principal.
    for rollout_path in _rollouts_by_mtime_desc(sessions_path, _CODEX_RATE_LIMIT_CANDIDATE_MAX):
        rate_limits = _rollout_principal_rate_limits(rollout_path)
        if rate_limits is not None:
            return (rollout_path, rate_limits)

    return None


# Tope para sostener "busy" cuando hay task_started sin task_complete pero el
# rollout dejó de escribirse. Cubre comandos/respuestas silenciosas sin dejar una
# sesión abandonada como ocupada durante media hora.
_CODEX_OPEN_TASK_BUSY_MAX_SECONDS = 300
# Cantidad maxima de rollouts recientes que se inspeccionan para actividad. Permite
# detectar una sesion de trabajo abierta aunque otra conversacion escriba despues.
_CODEX_ACTIVITY_CANDIDATE_MAX = 12
# Solo se lee la cola del rollout para inferir actividad: los marcadores de turno recientes
# caben de sobra y evita leer megabytes en cada sondeo. Mas amplio que el de Claude porque las
# lineas de Codex (con salidas de herramientas embebidas) pueden ser grandes.
_CODEX_ROLLOUT_TAIL_BYTES = 65536


class CodexRolloutActivity(TypedDict):
    has_turn_marker: bool
    open_task: bool
    pending_user_input: bool
    last_agent_message: str


def _recent_rollouts(sessions_path: Path, current_ms: int, max_age_ms: int) -> list[Path]:
    # Selecciona por mtime reciente, pero no asume que el rollout mas nuevo sea el
    # unico activo: puede haber sesiones largas abiertas en paralelo.
    candidates: list[tuple[float, Path]] = []
    for candidate in sessions_path.rglob("*.jsonl"):
        try:
            mtime = candidate.stat().st_mtime
        except OSError as error:
            log_event("codex_rollout_stat_failed", path=str(candidate), detail=str(error))
            continue

        age_ms = current_ms - int(mtime * 1000)
        if age_ms <= max_age_ms:
            candidates.append((mtime, candidate))

    candidates.sort(key=lambda item: item[0], reverse=True)
    return [candidate for _, candidate in candidates[:_CODEX_ACTIVITY_CANDIDATE_MAX]]


def _read_rollout_activity(path: Path) -> CodexRolloutActivity:
    # Lee eventos reales del JSONL; no busca strings crudos en salidas de herramientas.
    # Esto evita falsos positivos cuando un output contiene texto como "task_complete".
    #
    # Solo se lee la COLA del rollout: los marcadores de turno relevantes
    # (task_started/complete, request_user_input, agent_message) estan al final, y leer
    # archivos completos en cada sondeo era el mayor costo de IO. Caveat: si un turno en curso
    # produce una salida enorme que empuja su task_started fuera de la cola, el conteo de
    # sesiones puede quedarse corto para esa sesion, pero la actividad sigue bien (cae al
    # respaldo "sin marcadores en sesion reciente => busy").
    last_started = -1
    last_complete = -1
    last_ask = -1
    last_answer = -1
    last_agent_message = ""

    try:
        with path.open("rb") as handle:
            file_size = path.stat().st_size
            if file_size > _CODEX_ROLLOUT_TAIL_BYTES:
                handle.seek(file_size - _CODEX_ROLLOUT_TAIL_BYTES)
                # La primera linea tras el salto suele quedar partida; se descarta.
                handle.readline()
            raw = handle.read()
    except OSError as error:
        raise RuntimeError(f"No se pudo leer {path}: {error}") from error

    index = 0
    for raw_line in raw.split(b"\n"):
        if raw_line.strip() == b"":
            continue
        index += 1
        try:
            event = json.loads(raw_line)
        except JSONDecodeError:
            continue
        if not isinstance(event, dict):
            continue

        payload = event.get("payload")
        if not isinstance(payload, dict):
            continue

        payload_type = payload.get("type")
        if payload_type == "task_started":
            last_started = index
        elif payload_type == "task_complete":
            last_complete = index
        elif payload_type == "function_call" and payload.get("name") == "request_user_input":
            last_ask = index
        elif payload_type == "function_call_output":
            last_answer = index
        elif payload_type == "agent_message":
            message = payload.get("message")
            if isinstance(message, str):
                last_agent_message = message

    has_turn_marker = last_started != -1 or last_complete != -1
    open_task = last_started > last_complete
    # Si la cola no alcanzo a incluir NINGUN marcador de turno, no es idle: es una tarea larga
    # en curso cuyo task_started quedo por encima de la cola (una peticion del usuario enmarca
    # decenas de tool calls con salidas grandes). Sin este respaldo, una sesion trabajando se
    # leia como idle porque solo la salvaba la ventana corta de "sin marcadores". Una sesion
    # terminada conserva su task_complete al final del archivo, dentro de la cola, asi que este
    # barrido completo solo se paga en el caso ambiguo.
    if not has_turn_marker:
        has_turn_marker, open_task = _scan_full_task_state(path)

    return {
        "has_turn_marker": has_turn_marker,
        "open_task": open_task,
        "pending_user_input": last_ask != -1 and last_ask > last_answer,
        "last_agent_message": last_agent_message,
    }


def _scan_full_task_state(path: Path) -> tuple[bool, bool]:
    # Devuelve (has_turn_marker, open_task) recorriendo TODO el rollout, pero json-parseando
    # solo las lineas que contienen los tokens de marcador (prefiltro por substring): el costo
    # es leer el archivo sin deserializar las salidas de herramienta voluminosas. Se usa como
    # respaldo cuando la cola de _CODEX_ROLLOUT_TAIL_BYTES no incluyo ningun task_started/
    # task_complete. open_task es True si el ultimo marcador del archivo fue un task_started.
    last_marker = ""
    try:
        with path.open("rb") as handle:
            for raw_line in handle:
                if b"task_started" not in raw_line and b"task_complete" not in raw_line:
                    continue
                try:
                    event = json.loads(raw_line)
                except JSONDecodeError:
                    continue
                if not isinstance(event, dict):
                    continue
                payload = event.get("payload")
                if not isinstance(payload, dict):
                    continue
                payload_type = payload.get("type")
                if payload_type == "task_started":
                    last_marker = "started"
                elif payload_type == "task_complete":
                    last_marker = "complete"
    except OSError as error:
        raise RuntimeError(f"No se pudo leer {path}: {error}") from error

    return (last_marker != "", last_marker == "started")


# El session_meta es siempre la PRIMERA linea del rollout y es inmutable tras crearse el
# archivo: identifica el rollout como sesion de usuario o como subagente (thread_source
# "subagent" + parent_thread_id). Se cachea de forma permanente por ruta porque nunca cambia;
# el universo de rutas esta acotado por la cantidad de rollouts del directorio sessions/.
_codex_meta_cache_lock = threading.Lock()
_codex_meta_cache: dict[str, str] = {}


def _read_rollout_thread_source(path: Path) -> str:
    # Devuelve el thread_source del session_meta ("user", "subagent", ...) o "" si no se
    # puede determinar. Lee solo la primera linea; el resultado se cachea para siempre.
    cache_key = str(path)
    with _codex_meta_cache_lock:
        cached = _codex_meta_cache.get(cache_key)
        if cached is not None:
            return cached

    thread_source = _parse_rollout_thread_source(path)
    with _codex_meta_cache_lock:
        _codex_meta_cache[cache_key] = thread_source
    return thread_source


def _parse_rollout_thread_source(path: Path) -> str:
    try:
        with path.open("rb") as handle:
            first_line = handle.readline()
    except OSError as error:
        raise RuntimeError(f"No se pudo leer {path}: {error}") from error

    try:
        event = json.loads(first_line)
    except JSONDecodeError:
        return ""
    if not isinstance(event, dict) or event.get("type") != "session_meta":
        return ""

    payload = event.get("payload")
    if not isinstance(payload, dict):
        return ""

    thread_source = payload.get("thread_source")
    return thread_source if isinstance(thread_source, str) else ""


def _looks_like_question(text: str) -> bool:
    # Señal de alta precisión: el mensaje (sin espacios ni énfasis Markdown al final)
    # termina en "?" — cubre tanto "...?" como "¿...?".
    stripped = text.rstrip()
    while stripped != "" and stripped[-1] in "*_`> ":
        stripped = stripped[:-1].rstrip()
    return stripped.endswith("?")


# Cache breve del estado+conteo de Codex. El long-poll lo consulta ~4 veces/seg y recorrer los
# rollouts (rglob + leer archivos) en cada sondeo era el mayor consumo de IO del servicio. Con
# este TTL se calcula a lo sumo una vez por intervalo; la reaccion del ESP a cambios de Codex
# queda acotada al TTL (~1s, imperceptible). El estado y el conteo salen del MISMO recorrido,
# asi que pedir ambos en el mismo poll no duplica el escaneo.
_CODEX_ACTIVITY_TTL_MS = 1000
_codex_state_cache_lock = threading.Lock()
_codex_state_last_ms = 0
_codex_state_last_key: tuple[int, int, bool] | None = None
_codex_state_last: tuple[str, int] | None = None


def _compute_codex_activity_count(
    codex_home: Path, current_ms: int, busy_window_seconds: int, stale_seconds: int, include_subagents: bool
) -> tuple[str, int]:
    # Una sola pasada por los rollouts recientes que deriva A LA VEZ el estado de actividad y el
    # numero de sesiones de Codex con turno abierto. Codex enmarca cada turno con
    # task_started ... task_complete y no tiene un evento de "esperando" explicito; se infiere
    # del ultimo marcador: turno abierto => "busy" (hasta el tope de tarea abierta); sin
    # marcadores en una sesion reciente => "busy" solo dentro de la ventana corta.
    sessions_path = codex_home / "sessions"
    if not sessions_path.exists():
        return ("idle", 0)

    busy_ms = busy_window_seconds * 1000
    stale_ms = stale_seconds * 1000
    open_task_busy_ms = min(stale_seconds, _CODEX_OPEN_TASK_BUSY_MAX_SECONDS) * 1000
    max_activity_age_ms = max(busy_ms, stale_ms, open_task_busy_ms)
    rollout_paths = _recent_rollouts(sessions_path, current_ms, max_activity_age_ms)
    if len(rollout_paths) == 0:
        return ("idle", 0)

    rollout_states: list[tuple[int, CodexRolloutActivity]] = []
    for rollout_path in rollout_paths:
        # Con include_subagents apagado se descartan los rollouts hijos (subagentes) para
        # que no empujen actividad ni conteo. El short-circuit evita leer la cabecera en el
        # camino por defecto (incluir), preservando el costo de IO del long-poll.
        if not include_subagents and _read_rollout_thread_source(rollout_path) == "subagent":
            continue
        age_ms = current_ms - int(rollout_path.stat().st_mtime * 1000)
        rollout_states.append((age_ms, _read_rollout_activity(rollout_path)))

    # Conteo: rollouts con turno abierto dentro de la ventana = sesiones de Codex trabajando.
    busy_count = sum(
        1 for age_ms, state in rollout_states if state["open_task"] and age_ms <= open_task_busy_ms
    )

    # Pregunta pendiente: request_user_input sin respuesta posterior -> Codex espera al usuario.
    if any(state["pending_user_input"] and age_ms <= stale_ms for age_ms, state in rollout_states):
        return ("waiting", busy_count)
    # Turno explicitamente abierto: no debe parpadear a idle mientras el modelo razona.
    if any(state["open_task"] and age_ms <= open_task_busy_ms for age_ms, state in rollout_states):
        return ("busy", busy_count)
    # Respaldo para preguntas en prosa (el ultimo mensaje del agente termina en "?").
    if any(age_ms <= stale_ms and _looks_like_question(state["last_agent_message"]) for age_ms, state in rollout_states):
        return ("waiting", busy_count)
    # Sin marcadores en una sesion reciente: evidencia debil, solo dentro de la ventana corta.
    if any(not state["has_turn_marker"] and age_ms <= busy_ms for age_ms, state in rollout_states):
        return ("busy", busy_count)

    return ("idle", busy_count)


def read_codex_activity_count(
    codex_home: Path, current_ms: int, busy_window_seconds: int, stale_seconds: int, include_subagents: bool
) -> tuple[str, int]:
    # Devuelve (actividad, conteo) cacheado durante _CODEX_ACTIVITY_TTL_MS para no recorrer los
    # rollouts en cada sondeo. Se reevalua si cambian las ventanas o el flag de subagentes
    # (config) o caduca el TTL.
    global _codex_state_last_ms, _codex_state_last_key, _codex_state_last
    key = (busy_window_seconds, stale_seconds, include_subagents)
    with _codex_state_cache_lock:
        if (
            _codex_state_last is not None
            and _codex_state_last_key == key
            and (current_ms - _codex_state_last_ms) < _CODEX_ACTIVITY_TTL_MS
        ):
            return _codex_state_last
        result = _compute_codex_activity_count(codex_home, current_ms, busy_window_seconds, stale_seconds, include_subagents)
        _codex_state_last_ms = current_ms
        _codex_state_last_key = key
        _codex_state_last = result
        return result


def get_codex_activity(
    codex_home: Path, current_ms: int, busy_window_seconds: int, stale_seconds: int, include_subagents: bool
) -> str:
    return read_codex_activity_count(codex_home, current_ms, busy_window_seconds, stale_seconds, include_subagents)[0]


def count_busy_codex_sessions(
    codex_home: Path, current_ms: int, busy_window_seconds: int, stale_seconds: int, include_subagents: bool
) -> int:
    return read_codex_activity_count(codex_home, current_ms, busy_window_seconds, stale_seconds, include_subagents)[1]


def _window_from_codex(section: Any, window_minutes: int, current_ms: int) -> UsageWindowConfig:
    fallback_reset_ms = current_ms + (window_minutes * 60_000)

    if not isinstance(section, dict):
        return {
            "remaining_percent": 100.0,
            "window_start_ms": current_ms,
            "window_reset_ms": fallback_reset_ms,
        }

    used = section.get("used_percent")
    used_percent = float(used) if isinstance(used, (int, float)) else 0.0
    remaining_percent = clamp_percent(100.0 - used_percent)

    minutes_raw = section.get("window_minutes")
    minutes = int(minutes_raw) if isinstance(minutes_raw, (int, float)) else window_minutes

    resets_at = section.get("resets_at")
    if isinstance(resets_at, (int, float)) and resets_at > 0:
        window_reset_ms = int(resets_at) * 1000
    else:
        window_reset_ms = fallback_reset_ms

    window_start_ms = window_reset_ms - (minutes * 60_000)

    return {
        "remaining_percent": remaining_percent,
        "window_start_ms": window_start_ms,
        "window_reset_ms": window_reset_ms,
    }


def _empty_codex_window(current_ms: int, window_minutes: int) -> UsageWindowConfig:
    return {
        "remaining_percent": 0.0,
        "window_start_ms": current_ms,
        "window_reset_ms": current_ms + (window_minutes * 60_000),
    }


def read_codex_rate_limits_reading_uncached(codex_home: Path, current_ms: int) -> ToolUsageReading:
    # Lee la última cuota del modelo PRINCIPAL que Codex persistió en sus rollouts (se
    # ignora el modelo "spark", que usa un bucket de cuota aparte). Es pasivo y gratis:
    # refleja la cuota real a la última vez que se usó el modelo principal.
    failure_current = _empty_codex_window(current_ms, CODEX_PRIMARY_MINUTES)
    failure_weekly = _empty_codex_window(current_ms, CODEX_SECONDARY_MINUTES)

    sessions_path = codex_home / "sessions"
    if not sessions_path.exists():
        detail = "No existe sessions/ en CODEX_HOME"
        log_event("codex_usage_unavailable", detail=detail)
        return {"ok": False, "source": "codex-rollout-unavailable", "detail": detail, "observed_at_ms": 0, "current": failure_current, "weekly": failure_weekly}

    principal = _newest_principal_rate_limits(sessions_path)
    if principal is None:
        detail = "Sin rate_limits del modelo principal en rollouts de Codex"
        log_event("codex_usage_unavailable", detail=detail)
        return {"ok": False, "source": "codex-rollout-unavailable", "detail": detail, "observed_at_ms": 0, "current": failure_current, "weekly": failure_weekly}

    rollout_path, rate_limits = principal
    return {
        "ok": True,
        "source": "codex-rollout-snapshot",
        "detail": f"Snapshot de cuota (principal) desde {rollout_path.name}",
        "observed_at_ms": int(rollout_path.stat().st_mtime * 1000),
        "current": _window_from_codex(rate_limits.get("primary"), CODEX_PRIMARY_MINUTES, current_ms),
        "weekly": _window_from_codex(rate_limits.get("secondary"), CODEX_SECONDARY_MINUTES, current_ms),
    }


def read_codex_rate_limits_reading(codex_home: Path, current_ms: int, ttl_seconds: int) -> ToolUsageReading:
    # Throttle configurable para no recorrer sessions/** en cada render de la web o del ESP32.
    global _codex_last_attempt_ms, _codex_last_reading

    ttl_ms = ttl_seconds * 1000
    with _codex_cache_lock:
        if _codex_last_reading is not None and (current_ms - _codex_last_attempt_ms) < ttl_ms:
            return _codex_last_reading

        _codex_last_attempt_ms = current_ms
        reading = read_codex_rate_limits_reading_uncached(codex_home, current_ms)
        _codex_last_reading = reading
        return reading
