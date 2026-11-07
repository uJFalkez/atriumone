"""track.csv writer (docs/CONTRATO.md §2): append-only, fsync about once per second.

Lines are queued by the caller and written by a dedicated thread, so a slow disk never
delays the navigation polling or GET /status.
"""
import os
import queue
import threading
import time

HEADER = "t_s,north_m,east_m,height_m,source,sigma_m\n"


def last_row(path):
    """Last data row as a dict (t_s, north_m, east_m, height_m), or None."""
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - 4096))
            lines = f.read().decode(errors="replace").splitlines()
    except OSError:
        return None
    for line in reversed(lines):
        parts = line.split(",")
        if len(parts) >= 4 and parts[0] != "t_s":
            try:
                return {"t_s": float(parts[0]), "north_m": float(parts[1]),
                        "east_m": float(parts[2]), "height_m": float(parts[3])}
            except ValueError:
                continue
    return None


class TrackWriter:
    def __init__(self, path):
        self.path = path
        self.q = queue.Queue()
        self.last_t = -1.0
        new = not os.path.exists(path) or os.path.getsize(path) == 0
        self.f = open(path, "a", buffering=1)
        if new:
            self.f.write(HEADER)
        else:
            row = last_row(path)
            if row:
                self.last_t = row["t_s"]
        self.thread = threading.Thread(target=self._run, name="track", daemon=True)
        self.thread.start()

    def add(self, t_s, north, east, height, source, sigma):
        if t_s <= self.last_t:                 # t_s must never decrease (contract)
            t_s = self.last_t + 0.001
        self.last_t = t_s
        sig = "" if sigma is None else f"{sigma:.2f}"
        self.q.put(f"{t_s:.3f},{north:.3f},{east:.3f},{height:.3f},{source},{sig}\n")

    def _run(self):
        done = False
        while not done:
            item = self.q.get()
            if item is None:
                break
            lines = [item]
            deadline = time.monotonic() + 1.0      # gather ~1 s of lines per fsync
            while True:
                left = deadline - time.monotonic()
                if left <= 0:
                    break
                try:
                    nxt = self.q.get(timeout=left)
                except queue.Empty:
                    break
                if nxt is None:
                    done = True
                    break
                lines.append(nxt)
            self.f.write("".join(lines))
            self.f.flush()
            os.fsync(self.f.fileno())
        self.f.flush()
        os.fsync(self.f.fileno())

    def close(self):
        self.q.put(None)
        self.thread.join(timeout=10)
        self.f.close()
