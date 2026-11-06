"""Registro de missões enviado pelo módulo: mission.json + track.csv.

Ver CONTRATO.md, seção "POST /mission".
"""

import csv
import io
import json
import math

TRACK_COLUMNS = ("t_s", "north_m", "east_m")
MAX_TRACK_BYTES = 50 * 1024 * 1024


class InvalidMission(ValueError):
    pass


def validate_mission(raw: bytes, mission_id: str) -> dict:
    try:
        mission = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise InvalidMission("mission must contain valid JSON")
    if not isinstance(mission, dict):
        raise InvalidMission("mission must be a JSON object")
    if mission.get("id") != mission_id:
        raise InvalidMission("mission.id must match mission_id")
    for field in ("started_at", "finished_at"):
        if not isinstance(mission.get(field), str):
            raise InvalidMission(f"mission.{field} is required (ISO 8601)")
    return mission


def validate_track(raw: bytes) -> int:
    """Confere o CSV do trajeto e devolve o número de amostras."""
    if len(raw) > MAX_TRACK_BYTES:
        raise InvalidMission("track too large")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        raise InvalidMission("track must be UTF-8 CSV")

    reader = csv.reader(io.StringIO(text))
    header = [c.strip() for c in next(reader, [])]
    missing = [c for c in TRACK_COLUMNS if c not in header]
    if missing:
        raise InvalidMission(f"track header must include {', '.join(TRACK_COLUMNS)} (missing: {', '.join(missing)})")
    index = [header.index(c) for c in TRACK_COLUMNS]

    count, last_t = 0, -math.inf
    for line, row in enumerate(reader, start=2):
        if not row or all(not c.strip() for c in row):
            continue
        try:
            t, north, east = (float(row[i]) for i in index)
        except (IndexError, ValueError):
            raise InvalidMission(f"track line {line}: expected numbers in {', '.join(TRACK_COLUMNS)}")
        if not all(math.isfinite(v) for v in (t, north, east)):
            raise InvalidMission(f"track line {line}: non-finite value")
        if t < last_t:
            raise InvalidMission(f"track line {line}: t_s must not decrease")
        last_t = t
        count += 1
    if count < 2:
        raise InvalidMission("track must have at least 2 samples")
    return count
