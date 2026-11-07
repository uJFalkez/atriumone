"""Small helpers shared by missiond and the uploader: time stamps and crash-safe file writes."""
import json
import os
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path


def iso_utc(epoch=None):
    """ISO 8601 with milliseconds and an explicit +00:00 offset (docs/CONTRATO.md)."""
    t = time.time() if epoch is None else epoch
    return datetime.fromtimestamp(t, timezone.utc).isoformat(timespec="milliseconds")


def iso_plus(iso, seconds):
    return (datetime.fromisoformat(iso) + timedelta(seconds=seconds)).isoformat(timespec="milliseconds")


def fsync_dir(path):
    fd = os.open(str(path), os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def fsync_file(path):
    fd = os.open(str(path), os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def write_atomic(path, data: bytes):
    """temp file in the same directory + fsync + rename + fsync(dir)."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f".{path.name}.tmp")
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    fsync_dir(path.parent)


def write_json_atomic(path, obj):
    write_atomic(path, json.dumps(obj, ensure_ascii=False, indent=2).encode())


def read_json(path, default=None):
    try:
        with open(path, "rb") as f:
            return json.loads(f.read())
    except (OSError, ValueError):
        return default
