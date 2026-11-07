#!/usr/bin/env python3
"""Uploader: sends the local queue to the station (docs/PIPELINE.md §3.4.1 and §3.6).

Runs as its own low-priority process (missiond starts it under `ionice -c3 nice -n10`, and
SIGSTOPs it during each capture), so it can never delay a photo or GET /status.
The file system is the only interface with missiond:

  data/missions/<mid>/queue/<entry_id>/   complete captures -> POST /entry, one at a time,
                                          deleted after 201 / 200 (already_exists)
  data/missions/<mid>/mission.json        written by missiond at finish; once the queue of
                                          that mission is empty -> POST /mission with
                                          track.csv, then the mission directory is deleted
  data/rejected/<mid>/...                 items the station answered 400 to (module bug)

Network errors, timeouts and 5xx keep the item and retry with backoff (up to 30 s).
"""
import http.client
import json
import logging
import os
import shutil
import sys
import time
import urllib.parse
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import config as C                      # noqa: E402
from common import fsync_dir, read_json, write_atomic   # noqa: E402

log = logging.getLogger("uploader")
MISSIONS = C.DATA / "missions"
REJECTED = C.DATA / "rejected"


class Retry(Exception):
    """Transient failure: keep the item, try again later."""


def _conn(timeout):
    u = urllib.parse.urlsplit(C.STATION_URL)
    cls = http.client.HTTPSConnection if u.scheme == "https" else http.client.HTTPConnection
    return cls(u.hostname, u.port, timeout=timeout), u.path.rstrip("/")


def post_multipart(path, fields, files):
    """Streams the files from disk (no full copy in RAM). Returns (status, parsed json or {})."""
    boundary = uuid.uuid4().hex
    parts = []
    for k, v in fields.items():
        parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{k}"\r\n\r\n{v}\r\n'.encode())
    for k, (fname, fpath, ctype) in files.items():
        parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{k}"; filename="{fname}"\r\n'
                     f'Content-Type: {ctype}\r\n\r\n'.encode())
        parts.append(Path(fpath))
        parts.append(b"\r\n")
    parts.append(f"--{boundary}--\r\n".encode())
    length = sum(p.stat().st_size if isinstance(p, Path) else len(p) for p in parts)

    try:
        conn, base = _conn(C.UPLOAD_TIMEOUT_S)
        conn.putrequest("POST", base + path)
        conn.putheader("Content-Type", f"multipart/form-data; boundary={boundary}")
        conn.putheader("Content-Length", str(length))
        conn.endheaders()
        for p in parts:
            if isinstance(p, Path):
                with open(p, "rb") as f:
                    while chunk := f.read(65536):
                        conn.send(chunk)
            else:
                conn.send(p)
        r = conn.getresponse()
        body = r.read()
        conn.close()
    except (OSError, http.client.HTTPException) as e:
        raise Retry(f"{path}: {e}") from e
    try:
        data = json.loads(body) if body else {}
    except ValueError:
        data = {"raw": body[:500].decode(errors="replace")}
    return r.status, data


def reject(src: Path, dest: Path, status, data):
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists():
        shutil.rmtree(dest)
    os.replace(src, dest)
    write_atomic(dest / "rejection.json", json.dumps({"status": status, "response": data}).encode())
    fsync_dir(src.parent)
    log.error("rejected by the station (%s): %s -> %s", status, data, dest)


def entry_sequence(d: Path):
    m = read_json(d / "metadata.json", {}) or {}
    return m.get("sequence", 1 << 30), d.name


def send_entry(mid, d: Path):
    files = {"rgb": ("rgb.jpg", d / "rgb.jpg", "image/jpeg"),
             "nir": ("nir.jpg", d / "nir.jpg", "image/jpeg"),
             "metadata": ("metadata.json", d / "metadata.json", "application/json")}
    status, data = post_multipart("/entry", {"entry_id": d.name}, files)
    if status in (200, 201) and data.get("entry_id", d.name) == d.name:
        shutil.rmtree(d)
        fsync_dir(d.parent)
        log.info("entry %s sent (%s)", d.name, data.get("status", status))
    elif status == 400:
        reject(d, REJECTED / mid / d.name, status, data)
    else:
        raise Retry(f"/entry {d.name}: HTTP {status} {data}")


def send_mission(mid, mdir: Path):
    files = {"mission": ("mission.json", mdir / "mission.json", "application/json"),
             "track": ("track.csv", mdir / "track.csv", "text/csv")}
    status, data = post_multipart("/mission", {"mission_id": mid}, files)
    if status in (200, 201) and data.get("mission_id", mid) == mid:
        shutil.rmtree(mdir)
        fsync_dir(mdir.parent)
        log.info("mission %s registered (%s)", mid, data.get("status", status))
    elif status == 400:
        reject(mdir, REJECTED / mid / "mission", status, data)
    else:
        raise Retry(f"/mission {mid}: HTTP {status} {data}")


def next_work():
    """Oldest mission first; its queued entries in sequence order, then its registration."""
    if not MISSIONS.is_dir():
        return None
    dirs = sorted((d for d in MISSIONS.iterdir() if d.is_dir()), key=lambda d: d.stat().st_mtime_ns)
    for mdir in dirs:
        q = mdir / "queue"
        entries = sorted((e for e in q.iterdir() if e.is_dir()), key=entry_sequence) if q.is_dir() else []
        if entries:
            return ("entry", mdir.name, entries[0])
        if (mdir / "mission.json").exists() and (mdir / "track.csv").exists():
            return ("mission", mdir.name, mdir)
    return None


def main():
    logging.basicConfig(level=logging.INFO, format="uploader: %(message)s", stream=sys.stderr)
    parent = os.getppid()
    backoff = 1.0
    while True:
        if os.getppid() != parent:           # missiond is gone: exit with it
            return
        work = next_work()
        if work is None:
            time.sleep(1.0)
            continue
        # No /health pre-check: on a slow link /health itself times out while uploads still
        # get through. A failed upload is the real signal, and it backs off below.
        kind, mid, target = work
        try:
            if kind == "entry":
                send_entry(mid, target)
            else:
                send_mission(mid, target)
            backoff = 1.0
        except Retry as e:
            log.warning("%s (retry in %.0f s)", e, backoff)
            time.sleep(backoff)
            backoff = min(backoff * 2, C.UPLOAD_BACKOFF_MAX_S)
        except FileNotFoundError as e:       # item changed under us (e.g. rejected meanwhile)
            log.warning("skipped: %s", e)


if __name__ == "__main__":
    main()
