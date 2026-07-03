import json
from datetime import datetime


def log_event(event: str, **fields: object) -> None:
    # Structured log: local ISO-8601 timestamp + stable event name +
    # fields as JSON, without interpolating dynamic values into a prose message.
    payload: dict[str, object] = {"ts": datetime.now().astimezone().isoformat(timespec="seconds"), "event": event}
    payload.update(fields)
    print(json.dumps(payload, ensure_ascii=False), flush=True)
