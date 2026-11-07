"""Cameras used by missiond.

Each camera has:
    status()            -> {"connected": bool, "status": "ok"|"warning"|"error", "message": str|None}
                           cheap: never opens the device (called from the devices thread)
    capture(dest_path)  -> (t_mono, params): CLOCK_MONOTONIC seconds of the kept frame and the
                           real capture parameters for metadata.json "camera" (or None);
                           raises CameraError
    start() / stop()    -> optional; missiond calls them at mission start / end

Real cameras (measured 2026-11-06, see docs/module/PLANO.md WP3):
  RGB  Arducam 8MP USB, 1600x1200 MJPEG (full field of view, rescaled by the camera), via the
       small C tool build/camgrab: opens the camera, fixes white balance (5500 K) and focus,
       drops WARMUP_FRAMES frames, writes the camera's own JPEG. ~1.3 s, negligible RAM.
  NIR  Pi Camera V2 NoIR, 1640x1232 (2x2 binning, full field of view), one rpicam-still kept
       running during the mission in --signal mode: each SIGUSR1 writes a JPEG in ~0.35 s,
       29 MB RAM. The camera streams continuously, so exposure is always settled. Fixed AWB
       gains when calibrated (station's request: option b), automatic exposure.
FakeCamera copies a real sample photo (data/fake/<name>.jpg). Nothing here ever launches
ffmpeg: on this Pi it caused I/O thrash on the USB boot drive (docs/module/CLAUDE.md).
"""
import json
import os
import shutil
import signal
import subprocess
import time
from pathlib import Path

import config as C
from common import read_json


class CameraError(Exception):
    pass


def camera_calibration():
    """Section "camera" of the calibration file: rgb_focus, nir_colour_gains."""
    return (read_json(C.CALIB_PATH, {}) or {}).get("camera") or {}


class FakeCamera:
    def __init__(self, name):
        self.name = name
        self.sample = C.DATA / "fake" / f"{name}.jpg"

    def status(self):
        if not self.sample.exists():
            return {"connected": False, "status": "error", "message": f"amostra {self.sample.name} ausente"}
        return {"connected": True, "status": "warning", "message": "câmera simulada"}

    def capture(self, dest):
        if not self.sample.exists():
            raise CameraError(f"fake {self.name}: sample {self.sample} missing")
        shutil.copyfile(self.sample, dest)
        return time.monotonic(), None


class RgbCamera:
    def __init__(self):
        self.device = C.RGB_DEVICE
        self.last_error = None

    def _focus(self):
        """Focus position, "fixed" (the lens does not respond to the focus control: measured
        2026-11-06, positions 1 and 1023 equally sharp at > 20 m), or None (not checked)."""
        f = camera_calibration().get("rgb_focus")
        return f if isinstance(f, int) or f == "fixed" else None

    def status(self):
        if not os.path.exists(self.device):
            return {"connected": False, "status": "error", "message": "não detectada"}
        if self.last_error:
            return {"connected": True, "status": "error", "message": self.last_error[:80]}
        if self._focus() is None:
            return {"connected": True, "status": "warning", "message": "foco não calibrado"}
        return {"connected": True, "status": "ok"}

    def capture(self, dest):
        w, h = C.RGB_SIZE
        cmd = [str(C.CAMGRAB), "-d", self.device, "-W", str(w), "-H", str(h),
               "-n", str(C.WARMUP_FRAMES), "-t", "3000", "-w", str(C.RGB_WB_K), "-e", str(C.RGB_EXPOSURE),
               "-o", str(dest)]
        focus = self._focus()
        if isinstance(focus, int):
            cmd += ["-f", str(focus)]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
            out = json.loads(r.stdout.strip().splitlines()[-1]) if r.stdout.strip() else {}
        except (OSError, subprocess.TimeoutExpired, ValueError) as e:
            self.last_error = f"camgrab: {e}"
            raise CameraError(self.last_error) from e
        if not out.get("ok"):
            self.last_error = out.get("error") or f"camgrab exit {r.returncode}"
            raise CameraError(self.last_error)
        self.last_error = None
        c = out.get("controls", {})
        exp = c.get("exposure_time_absolute")
        params = {
            "exposure": exp,
            "exposure_us": None if exp is None else exp * 100,        # UVC unit: 100 us
            "auto_exposure": None if c.get("auto_exposure") is None else c["auto_exposure"] != 1,
            "gain": c.get("gain"),
            "white_balance_k": c.get("white_balance_temperature") if c.get("white_balance_automatic") == 0 else None,
            "white_balance_auto": None if c.get("white_balance_automatic") is None
            else bool(c["white_balance_automatic"]),
            "focus": c.get("focus_absolute"),
            "width": out.get("width"), "height": out.get("height"),
        }
        return out.get("frame_t_mono", time.monotonic()), params


class NirCamera:
    """rpicam-still kept open in --signal mode; spool files are renamed into the capture."""

    def __init__(self):
        self.spool = C.DATA / "tmp" / "nir_spool"
        self.proc = None
        self.started_at = 0.0
        self.last_error = None

    def _gains(self):
        g = camera_calibration().get("nir_colour_gains")
        return g if isinstance(g, list) and len(g) == 2 else None

    def start(self):
        if self.proc and self.proc.poll() is None:
            return
        shutil.rmtree(self.spool, ignore_errors=True)
        self.spool.mkdir(parents=True, exist_ok=True)
        w, h = C.NIR_SIZE
        cmd = ["rpicam-still", "-n", "-t", "0", "--signal", "--width", str(w), "--height", str(h),
               "--mode", f"{w}:{h}", "--viewfinder-mode", f"{w}:{h}", "-q", str(C.NIR_JPEG_QUALITY),
               "--rotation", str(C.NIR_ROTATION),
               "-o", str(self.spool / "nir%04d.jpg"),
               "--metadata", str(self.spool / "meta.json"), "--metadata-format", "json"]
        g = self._gains()
        if g:
            cmd += ["--awbgains", f"{g[0]},{g[1]}"]
        self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        self.started_at = time.monotonic()

    def stop(self):
        p, self.proc = self.proc, None
        if p and p.poll() is None:
            try:
                p.send_signal(signal.SIGUSR2)     # rpicam-apps: clean exit
                p.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                p.kill()

    def status(self):
        if not Path("/sys/bus/i2c/drivers/imx219").exists() or \
                not any(Path("/sys/bus/i2c/drivers/imx219").glob("*-0010")):
            return {"connected": False, "status": "error", "message": "não detectada"}
        if self.proc is not None and self.proc.poll() is not None:
            return {"connected": True, "status": "error", "message": "processo da câmera parou"}
        if self.last_error:
            return {"connected": True, "status": "error", "message": self.last_error[:80]}
        if self._gains() is None:
            return {"connected": True, "status": "warning", "message": "balanço de branco não calibrado"}
        return {"connected": True, "status": "ok"}

    def capture(self, dest):
        if self.proc is None or self.proc.poll() is not None:
            self.start()
            self.last_error = "câmera reiniciando"
            raise CameraError("rpicam-still was not running; restarted")
        if time.monotonic() - self.started_at < C.NIR_START_S:
            raise CameraError("rpicam-still still starting")
        before = {p.name for p in self.spool.glob("nir*.jpg")}
        t_trig = time.time()
        self.proc.send_signal(signal.SIGUSR1)
        deadline = time.monotonic() + C.NIR_CAPTURE_TIMEOUT_S
        new = None
        while time.monotonic() < deadline:
            time.sleep(0.03)
            fresh = [p for p in self.spool.glob("nir*.jpg") if p.name not in before]
            if fresh and _jpeg_complete(fresh[0]):
                new = fresh[0]
                break
        if new is None:
            self.last_error = "sem resposta do rpicam-still"
            raise CameraError("rpicam-still produced no image in time")
        t_found = time.monotonic()
        os.replace(new, dest)                       # same file system: no copy
        self.last_error = None
        params = {"exposure_us": None, "analogue_gain": None, "digital_gain": None, "colour_gains": None,
                  "awb_fixed": self._gains() is not None, "width": C.NIR_SIZE[0], "height": C.NIR_SIZE[1]}
        t_frame = t_found
        meta_path = self.spool / "meta.json"
        try:
            if meta_path.stat().st_mtime >= t_trig - 0.5:
                m = read_json(meta_path, {}) or {}
                params.update(exposure_us=m.get("ExposureTime"), analogue_gain=m.get("AnalogueGain"),
                              digital_gain=m.get("DigitalGain"), colour_gains=m.get("ColourGains"))
                ts = m.get("SensorTimestamp")
                if isinstance(ts, (int, float)) and abs(ts / 1e9 - t_found) < 5:
                    t_frame = ts / 1e9               # CLOCK_MONOTONIC (ns)
        except OSError:
            pass
        return t_frame, params


def _jpeg_complete(path):
    """Size stable and ends with the JPEG EOI marker (rpicam writes the file in place)."""
    try:
        s1 = path.stat().st_size
        if s1 < 1024:
            return False
        time.sleep(0.02)
        if path.stat().st_size != s1:
            return False
        with open(path, "rb") as f:
            f.seek(-2, os.SEEK_END)
            return f.read(2) == b"\xff\xd9"
    except OSError:
        return False


def make_cameras():
    if C.CAMERA_MODE == "fake":
        return {"rgb": FakeCamera("rgb"), "nir": FakeCamera("nir")}
    if C.CAMERA_MODE == "real":
        return {"rgb": RgbCamera(), "nir": NirCamera()}
    raise CameraError(f"unknown camera mode {C.CAMERA_MODE!r}")
