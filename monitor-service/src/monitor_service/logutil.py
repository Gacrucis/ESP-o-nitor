import json
from datetime import datetime


def log_event(event: str, **fields: object) -> None:
    # Log estructurado: marca de tiempo local ISO-8601 + nombre de evento estable +
    # campos como JSON, sin interpolar valores dinámicos dentro de un mensaje en prosa.
    payload: dict[str, object] = {"ts": datetime.now().astimezone().isoformat(timespec="seconds"), "event": event}
    payload.update(fields)
    print(json.dumps(payload, ensure_ascii=False), flush=True)
