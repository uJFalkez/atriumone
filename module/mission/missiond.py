#!/usr/bin/env python3
"""missiond: the module's mission daemon and HTTP API (docs/CONTRATO.md §1, docs/PIPELINE.md §3).

Threads:
  nav       polls navd at 5 Hz: cache for /status, track.csv, ready_to_finish, resume after reboot
  health    GET station /health every 2 s (one of the ready_to_finish conditions)
  devices   cameras, NTP, disk space, upload counters, uploading -> idle, uploader watchdog
  capture   photo scheduler (warm-up, >= 10 s apart, only when stable)
  main      ThreadingHTTPServer on 0.0.0.0:8000
The uploader is a separate low-priority process (uploader.py).

GET /status is built only from memory, so it answers in milliseconds even when navd, the
cameras or the disk are slow or broken.
"""
import json
import logging
import os
import shutil
import signal
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cameras                          # noqa: E402
import config as C                      # noqa: E402
import navclient                        # noqa: E402
from common import (fsync_dir, fsync_file, iso_plus, iso_utc, read_json,   # noqa: E402
                    write_json_atomic)
from track import TrackWriter, last_row  # noqa: E402

log = logging.getLogger("missiond")

STATE_FILE = C.DATA / "state.json"
MISSIONS = C.DATA / "missions"
TMP = C.DATA / "tmp"
REJECTED = C.DATA / "rejected"


class HttpError(Exception):
    def __init__(self, status, detail):
        super().__init__(detail)
        self.status, self.detail = status, detail


def mission_dir(mid):
    return MISSIONS / mid


class Module:
    def __init__(self):
        self.lock = threading.RLock()
        self.capture_lock = threading.Lock()
        self.p = read_json(STATE_FILE, None) or {"state": "idle", "mission": None}
        self.nav = None                  # last navd state
        self.nav_ok = False
        self.health_ok_at = None         # monotonic time of the last 200 from /health
        self.ready = False
        self.mono0 = None                # monotonic time where t_s = 0
        self.last_capture_mono = None
        self.enabled_mono = None
        self.track = None
        self.resume_pending = False
        self.starting = False            # a /mission/start is between its checks and its commit
        self.upload = {"pending": 0, "sent": 0, "total": 0}
        self.dev_cams = {}
        self.dev_clock = {"connected": True, "status": "ok"}
        self.dev_storage = {"connected": True, "status": "ok"}
        self.free_mb = None
        self.sensors = {"calibrated": False, "calibrated_at": None}
        self.camera_error = None
        self.calib = None                # {"started_at", "started_mono"} while calibrating
        self.uploader = None
        self.nir_delay = C.NIR_TRIGGER_DELAY_S   # adapted after each photo so both frames coincide
        self.cams = {}
        try:
            self.cams = cameras.make_cameras()
        except cameras.CameraError as e:
            log.error("cameras: %s", e)
            self.camera_error = str(e)
        self.load_calibration()
        self.recover()

    # ------------------------------------------------------------ persistence
    def persist(self):
        write_json_atomic(STATE_FILE, self.p)

    def load_calibration(self):
        """calibrated = a valid mag calibration exists (operator's decision 2026-11-06: apply
        what we have; the accel step never completed, so the accel stays optional and the gyro
        is zeroed every mission); calibrated_at = last completed /calibrate, in state.json."""
        cal = read_json(C.CALIB_PATH, {}) or {}
        ok = isinstance(cal.get("mag"), dict)
        at = self.p.get("calibrated_at")
        self.sensors = {"calibrated": bool(ok), "calibrated_at": at if isinstance(at, str) else None}

    def recover(self):
        """docs/PIPELINE.md §3.7: resume after a reboot or a crash."""
        shutil.rmtree(TMP, ignore_errors=True)       # half-written captures
        st, m = self.p.get("state"), self.p.get("mission")
        if st == "calibrating":
            self.p["state"] = "idle"
        if st == "in_mission" and m:
            mdir = mission_dir(m["id"])
            if (mdir / "mission.json").exists():
                self.p["state"] = "uploading"        # crashed right after finish
            elif mdir.exists():
                row = last_row(mdir / "track.csv")
                elapsed = row["t_s"] if row else 0.0
                now = time.monotonic()
                self.mono0 = now - elapsed
                self.enabled_mono = self.mono0 + C.WARMUP_S
                self.last_capture_mono = now           # wait a full interval after a reboot
                self.track = TrackWriter(mdir / "track.csv")
                self.resume_row = row or {"north_m": 0.0, "east_m": 0.0, "height_m": 0.0}
                self.resume_pending = True
                self.cameras_start()
                log.info("resuming mission %s at t_s=%.1f", m["id"], elapsed)
            else:
                self.p["state"], self.p["mission"] = "idle", None
        if self.p.get("state") == "idle":
            # Leftovers of a finished mission still in the queue -> keep uploading them.
            for mdir in sorted(MISSIONS.glob("*/mission.json")) if MISSIONS.is_dir() else []:
                mj = read_json(mdir, {}) or {}
                self.p["state"] = "uploading"
                self.p["mission"] = {"id": mj.get("id", mdir.parent.name),
                                     "started_at": mj.get("started_at"),
                                     "captures_enabled_at": iso_plus(mj["started_at"], C.WARMUP_S)
                                     if mj.get("started_at") else None,
                                     "captures": mj.get("captures", 0), "last_capture_at": None,
                                     "finished_at": mj.get("finished_at"), "forced": mj.get("forced", False)}
                break
        self.persist()

    # ------------------------------------------------------------ status
    def mission_view(self):
        m = self.p.get("mission")
        if not m or self.p["state"] not in ("in_mission", "uploading"):
            return None
        return {"id": m["id"], "started_at": m["started_at"], "captures_enabled_at": m["captures_enabled_at"],
                "captures": m.get("captures", 0), "last_capture_at": m.get("last_capture_at"),
                "ready_to_finish": bool(self.ready) if self.p["state"] == "in_mission" else False}

    def calibration_view(self):
        cs = (self.nav or {}).get("calibration_session") if self.nav_ok else None
        out = {"started_at": self.calib["started_at"] if self.calib else None,
               "step": "still", "instruction": "Preparando a calibração…", "remaining_s": None, "progress": None}
        if cs:
            out.update(step=cs["step"], instruction=cs["instruction"],
                       remaining_s=round(cs["remaining_s"]), progress=round(cs["progress"], 3))
        return out

    def devices(self):
        n = self.nav if self.nav_ok else None
        down = {"connected": False, "status": "error", "message": "navd sem resposta"}
        out = []

        def dev(i, d):
            out.append({"id": i, "label": C.LABELS[i], **d})

        if n:
            imu = n["devices"]["imu"]
            rate = imu.get("rate_hz", 0)
            d = {"connected": bool(imu.get("ok")), "status": "ok", "data": {"taxa": f"{rate:.0f} Hz"}}
            if not imu.get("ok"):
                d.update(status="error", message="sem leitura")
            elif rate < 45:
                d.update(status="warning", message="taxa baixa")
            dev("imu", d)

            g = n["devices"]["gps"]
            if not g.get("enabled"):
                dev("gps", {"connected": False, "status": "warning", "message": "desativado"})
            elif not g.get("present"):
                dev("gps", {"connected": False, "status": "warning", "message": "sem dados (só IMU)"})
            elif not g.get("fix"):
                dev("gps", {"connected": True, "status": "warning", "message": "sem fix (só IMU)",
                            "data": {"satélites": g.get("sats", 0)}})
            else:
                dev("gps", {"connected": True, "status": "ok",
                            "data": {"satélites": g.get("sats", 0), "hdop": g.get("hdop")}})

            mg = n["devices"]["mag"]
            cal = n.get("calibration", {})
            d = {"connected": bool(mg.get("ok")), "status": "ok"}
            if not mg.get("ok"):
                d.update(status="error", message="sem resposta")
            elif not cal.get("mag"):
                d.update(status="warning", message="não calibrado")
            elif not cal.get("mag_to_imu_measured"):
                d.update(status="warning", message="eixos não medidos")
            dev("mag", d)
        else:
            dev("imu", dict(down))
            dev("gps", dict(down))
            dev("mag", dict(down))

        for cid, key in (("camera_rgb", "rgb"), ("camera_nir", "nir")):
            if self.camera_error and key not in self.dev_cams:
                dev(cid, {"connected": False, "status": "error", "message": self.camera_error[:80]})
            else:
                dev(cid, dict(self.dev_cams.get(key, {"connected": False, "status": "error",
                                                       "message": "sem status"})))
        dev("clock", dict(self.dev_clock))
        dev("storage", dict(self.dev_storage))
        return out

    def status(self):
        with self.lock:
            st = self.p["state"]
            out = {
                "module_id": C.MODULE_ID,
                "version": C.VERSION,
                "uptime_s": int(float(open("/proc/uptime").read().split()[0])),
                "state": st,
                "devices": self.devices(),
                "sensors": dict(self.sensors),
                "calibration": self.calibration_view() if st == "calibrating" else None,
                "mission": self.mission_view(),
                "upload": dict(self.upload) if st in ("in_mission", "uploading")
                else {"pending": 0, "sent": 0, "total": 0},
            }
            if st == "in_mission" and self.nav_ok and self.nav:
                p, o = self.nav["position"], self.nav["orientation"]
                out["live"] = {"north_m": round(p["north_m"], 2), "east_m": round(p["east_m"], 2),
                               "height_m": round(p["height_m"], 2),
                               "yaw_deg": None if o.get("yaw_deg") is None else round(o["yaw_deg"], 1)}
            return out

    # ------------------------------------------------------------ commands
    def start_mission(self):
        with self.lock:
            if self.p["state"] != "idle" or self.starting:
                raise HttpError(409, f"Módulo ocupado (estado: {self.p['state']}).")
            self.starting = True
            mid = str(uuid.uuid4())
            mdir = mission_dir(mid)
            (mdir / "queue").mkdir(parents=True)
            fsync_dir(MISSIONS)
        try:
            r = navclient.reset(mid)       # ~2.5 s: gyro bias, level, zero position
            if not r.get("ok"):
                log.warning("navd reset: %s", r)
        except (navclient.NavError, ValueError) as e:
            log.error("navd reset failed: %s (mission starts anyway)", e)
        try:
            return self._commit_start(mid, mdir)
        finally:
            with self.lock:
                self.starting = False

    def _commit_start(self, mid, mdir):
        with self.lock:
            now_wall = time.time()
            self.mono0 = time.monotonic()
            self.enabled_mono = self.mono0 + C.WARMUP_S
            self.last_capture_mono = None
            started = iso_utc(now_wall)
            self.p["mission"] = {"id": mid, "started_at": started,
                                 "captures_enabled_at": iso_utc(now_wall + C.WARMUP_S),
                                 "captures": 0, "last_capture_at": None, "finished_at": None, "forced": False}
            self.track = TrackWriter(mdir / "track.csv")
            self.ready = False
            self.p["state"] = "in_mission"
            self.persist()
            self.cameras_start()
            log.info("mission %s started", mid)
            return {"mission": self.mission_view()}

    def finish_mission(self, force):
        with self.capture_lock:            # never cut a capture in half
            with self.lock:
                if self.p["state"] != "in_mission":
                    raise HttpError(409, f"Nenhuma missão em andamento (estado: {self.p['state']}).")
                if not self.ready and not force:
                    raise HttpError(409, "O módulo ainda não está pronto para finalizar "
                                         "(longe da estação, em movimento ou sem acesso à estação).")
                m = self.p["mission"]
                m["finished_at"] = iso_utc()
                m["forced"] = bool(force)
                if self.track:
                    self.track.close()
                    self.track = None
                mdir = mission_dir(m["id"])
                if not (mdir / "track.csv").exists():
                    (mdir / "track.csv").write_text("t_s,north_m,east_m,height_m,source,sigma_m\n")
                write_json_atomic(mdir / "mission.json", {
                    "id": m["id"], "module_id": C.MODULE_ID, "started_at": m["started_at"],
                    "finished_at": m["finished_at"], "captures": m.get("captures", 0), "forced": m["forced"]})
                self.p["state"] = "uploading"
                self.ready = False
                self.persist()
                log.info("mission %s finished (forced=%s), uploading", m["id"], force)
                view = self.mission_view()
        self.cameras_stop()
        try:
            navclient.stop()
        except (navclient.NavError, ValueError):
            pass
        return {"mission": view}

    def calibrate(self):
        with self.lock:
            if self.p["state"] != "idle" or self.starting:
                raise HttpError(409, f"Calibração só é possível com o módulo ocioso (estado: {self.p['state']}).")
        try:
            r = navclient.call("calib_start", timeout=5.0)
        except (navclient.NavError, ValueError) as e:
            raise HttpError(503, f"Sensores indisponíveis (navd sem resposta): {e}") from e
        if not r.get("ok"):
            raise HttpError(503, f"Os sensores recusaram a calibração ({r.get('error')}).")
        with self.lock:
            self.calib = {"started_at": iso_utc(), "started_mono": time.monotonic()}
            self.p["state"] = "calibrating"
            self.persist()
            if "calibration_session" in r:
                self.nav, self.nav_ok = r, True
            log.info("calibration started")
            return {"calibration": self.calibration_view()}

    # ------------------------------------------------------------ threads
    def nav_loop(self):
        next_track = 0.0
        while True:
            t0 = time.monotonic()
            try:
                st = navclient.state(timeout=C.NAV_TIMEOUT_S)
                ok = "position" in st
            except (navclient.NavError, ValueError):
                st, ok = None, False
            with self.lock:
                self.nav_ok = ok
                if ok:
                    self.nav = st
                if self.resume_pending and ok and self.p["state"] == "in_mission":
                    self._do_resume()
                if self.p["state"] == "calibrating":
                    self._follow_calibration(st if ok else None)
                if self.p["state"] == "in_mission":
                    self._update_ready()
                    if ok and self.track and t0 >= next_track and not self.resume_pending:
                        p = st["position"]
                        self.track.add(t0 - self.mono0, p["north_m"], p["east_m"], p["height_m"],
                                       p["source"], p.get("sigma_m"))
                        next_track = t0 + C.TRACK_PERIOD_S
            time.sleep(max(0.0, C.NAV_POLL_PERIOD_S - (time.monotonic() - t0)))

    def _follow_calibration(self, st):
        cs = st.get("calibration_session") if st else None
        elapsed = time.monotonic() - self.calib["started_mono"] if self.calib else 0
        if cs and cs["step"] == "done":
            if cs.get("updated", 0) > 0:
                self.p["calibrated_at"] = iso_utc()
            log.info("calibration finished: %s (updated %s)", cs["instruction"], cs.get("updated"))
        elif cs and cs.get("active") and elapsed < C.CALIBRATION_MAX_S:
            return
        elif st is None and elapsed < 30:
            return                       # navd briefly unreachable: wait
        else:
            log.warning("calibration abandoned (navd restarted or took too long)")
            try:
                navclient.call("calib_abort", timeout=2.0)
            except (navclient.NavError, ValueError):
                pass
        self.p["state"] = "idle"
        self.calib = None
        self.persist()
        self.load_calibration()

    def _do_resume(self):
        m, r = self.p["mission"], self.resume_row
        try:
            navclient.resume(m["id"], r["north_m"], r["east_m"], r["height_m"])
            self.resume_pending = False
            log.info("navd resumed at n=%.1f e=%.1f h=%.1f", r["north_m"], r["east_m"], r["height_m"])
        except (navclient.NavError, ValueError) as e:
            log.warning("navd resume failed: %s", e)

    def _update_ready(self):
        n = self.nav
        health = self.health_ok_at is not None and time.monotonic() - self.health_ok_at <= C.HEALTH_FRESH_S
        if not (self.nav_ok and n and health):
            self.ready = False
            return
        if n["devices"]["gps"].get("fix"):
            p, mo = n["position"], n["motion"]
            self.ready = (p["distance_from_start_m"] < C.RETURN_MAX_DISTANCE_M and
                          p["height_m"] < C.RETURN_MAX_HEIGHT_M and mo["stopped_for_s"] >= C.RETURN_STOPPED_S)
        else:
            self.ready = n["motion"]["landed_for_s"] >= C.RETURN_LANDED_S

    def health_loop(self):
        url = C.STATION_URL + "/health"
        while True:
            t0 = time.monotonic()
            try:
                with urllib.request.urlopen(url, timeout=C.HEALTH_TIMEOUT_S) as r:
                    if r.status == 200:
                        with self.lock:
                            self.health_ok_at = time.monotonic()
            except Exception:              # noqa: BLE001 - any failure just means "not reachable"
                pass
            time.sleep(max(0.2, C.HEALTH_PERIOD_S - (time.monotonic() - t0)))

    def devices_loop(self):
        next_ntp = 0.0
        while True:
            now = time.monotonic()
            cams = {}
            for k, cam in self.cams.items():
                try:
                    cams[k] = cam.status()
                except Exception as e:     # noqa: BLE001
                    cams[k] = {"connected": False, "status": "error", "message": str(e)[:80]}
            clock = None
            if now >= next_ntp:
                next_ntp = now + 30
                try:
                    v = subprocess.run(["timedatectl", "show", "-p", "NTPSynchronized", "--value"],
                                       capture_output=True, text=True, timeout=5).stdout.strip()
                    clock = {"connected": True, "status": "ok"} if v == "yes" else \
                        {"connected": True, "status": "warning", "message": "sem NTP"}
                except Exception:          # noqa: BLE001
                    clock = {"connected": True, "status": "warning", "message": "estado do NTP desconhecido"}
            du = shutil.disk_usage(C.DATA)
            free_mb = du.free / 1e6
            storage = {"connected": True, "status": "ok", "data": {"livre": f"{free_mb / 1000:.1f} GB"}}
            if free_mb < C.MIN_FREE_MB:
                storage.update(status="error", message="disco cheio: fotos suspensas")
            elif free_mb < 4 * C.MIN_FREE_MB:
                storage.update(status="warning", message="pouco espaço")
            counts = self._count_upload()
            self._check_uploader()
            with self.lock:
                self.dev_cams = cams
                if clock:
                    self.dev_clock = clock
                self.dev_storage = storage
                self.free_mb = free_mb
                if counts:
                    self.upload = counts
                if self.p["state"] == "uploading" and self.p.get("mission") and \
                        not mission_dir(self.p["mission"]["id"]).exists():
                    log.info("mission %s fully uploaded", self.p["mission"]["id"])
                    self.p["state"], self.p["mission"] = "idle", None
                    self.persist()
            time.sleep(1.0)

    def _count_upload(self):
        with self.lock:
            m = self.p.get("mission")
            if not m:
                return None
            mid, total = m["id"], m.get("captures", 0)
        q = mission_dir(mid) / "queue"
        pending = sum(1 for e in q.iterdir() if e.is_dir()) if q.is_dir() else 0
        rj = REJECTED / mid
        rejected = sum(1 for e in rj.iterdir() if e.is_dir() and e.name != "mission") if rj.is_dir() else 0
        return {"pending": pending, "sent": max(0, total - pending - rejected), "total": total}

    # ------------------------------------------------------------ cameras
    def cameras_start(self):
        for cam in self.cams.values():
            if hasattr(cam, "start"):
                try:
                    cam.start()
                except Exception as e:     # noqa: BLE001 - shown in devices, retried at capture
                    log.error("camera start: %s", e)

    def cameras_stop(self):
        for cam in self.cams.values():
            if hasattr(cam, "stop"):
                try:
                    cam.stop()
                except Exception as e:     # noqa: BLE001
                    log.error("camera stop: %s", e)

    # ------------------------------------------------------------ uploader process
    def start_uploader(self):
        pidfile = C.DATA / "uploader.pid"
        try:                               # a stale uploader may be left SIGSTOPped by a crash
            old = int(pidfile.read_text())
            if "uploader.py" in Path(f"/proc/{old}/cmdline").read_bytes().decode(errors="replace"):
                os.kill(old, signal.SIGCONT)
                os.kill(old, signal.SIGTERM)
        except (OSError, ValueError):
            pass
        cmd = [sys.executable, str(Path(__file__).with_name("uploader.py"))]
        if shutil.which("ionice"):
            cmd = ["ionice", "-c3"] + cmd
        cmd = ["nice", "-n", "10"] + cmd
        self.uploader = subprocess.Popen(cmd)
        pidfile.write_text(str(self.uploader.pid))

    def _check_uploader(self):
        if self.uploader and self.uploader.poll() is not None:
            log.warning("uploader exited (%s), restarting", self.uploader.returncode)
            self.start_uploader()

    def pause_uploader(self, pause):
        if self.uploader and self.uploader.poll() is None:
            try:
                os.kill(self.uploader.pid, signal.SIGSTOP if pause else signal.SIGCONT)
            except OSError:
                pass

    # ------------------------------------------------------------ captures
    def capture_loop(self):
        while True:
            time.sleep(C.CAPTURE_POLL_S)
            with self.lock:
                if self.p["state"] != "in_mission" or self.resume_pending or not self.cams:
                    continue
                now = time.monotonic()
                if now < self.enabled_mono:
                    continue
                if self.last_capture_mono is not None and now - self.last_capture_mono < C.MIN_CAPTURE_INTERVAL_S:
                    continue
                if self.free_mb is not None and self.free_mb < C.MIN_FREE_MB:
                    continue
                n = self.nav if self.nav_ok else None
            if C.REQUIRE_STABLE and (not n or not n["stability"]["stable"]):
                continue
            try:
                self.capture_once()
            except Exception as e:         # noqa: BLE001 - keep the scheduler alive
                log.exception("capture failed: %s", e)
                time.sleep(1.0)

    def capture_once(self):
        with self.capture_lock:
            with self.lock:
                if self.p["state"] != "in_mission":
                    return
                m = self.p["mission"]
                mid, seq = m["id"], m.get("captures", 0) + 1
            entry_id = str(uuid.uuid4())
            tmp = TMP / entry_id
            tmp.mkdir(parents=True)
            self.pause_uploader(True)
            try:
                try:
                    navclient.mag_burst()          # fresh yaw right before the photo
                except (navclient.NavError, ValueError) as e:
                    log.warning("mag burst failed: %s", e)
                t_start = time.monotonic()
                w0 = time.time()
                results, errors = {}, {}

                def shoot(key, delay):
                    try:
                        if delay > 0:              # the RGB needs ~1.3 s (open + warm-up), the NIR
                            time.sleep(delay)      # ~0.35 s: delay the NIR so the frames coincide
                        results[key] = self.cams[key].capture(tmp / f"{key}.jpg")
                    except Exception as e:         # noqa: BLE001
                        errors[key] = e

                th = [threading.Thread(target=shoot, args=("rgb", 0.0)),
                      threading.Thread(target=shoot, args=("nir", self.nir_delay))]
                for t in th:
                    t.start()
                for t in th:
                    t.join()
                w1 = time.time()
                if errors:
                    raise cameras.CameraError(f"camera failure: {errors}")
                try:
                    st = navclient.state(timeout=1.0)
                except (navclient.NavError, ValueError):
                    st = None
                # Still stable over the whole capture? Otherwise discard and retry soon.
                if C.REQUIRE_STABLE and st and not (st["stability"]["stable"] and
                               st["stability"]["stable_for_s"] >= time.monotonic() - t_start):
                    log.info("moved during the capture, discarded")
                    shutil.rmtree(tmp, ignore_errors=True)
                    return
                (t_rgb, p_rgb), (t_nir, p_nir) = results["rgb"], results["nir"]
                t_mid = (t_rgb + t_nir) / 2                       # monotonic instant of the pair
                wall = time.time() - (time.monotonic() - t_mid)
                if not (w0 - 5 <= wall <= w1 + 5):                # implausible frame time: fall back
                    wall = (w0 + w1) / 2
                offset = t_nir - t_rgb
                cam = {"rgb": p_rgb, "nir": p_nir, "rgb_nir_offset_ms": round(offset * 1000, 1)}
                if abs(offset) < 3.0:              # adapt the NIR delay (half step, bounded)
                    self.nir_delay = min(C.NIR_TRIGGER_DELAY_MAX_S, max(0.0, self.nir_delay - 0.5 * offset))
                meta = build_metadata(entry_id, mid, seq, iso_utc(wall), st, cam)
                write_json_atomic(tmp / "metadata.json", meta)
                for f in ("rgb.jpg", "nir.jpg"):
                    fsync_file(tmp / f)
                fsync_dir(tmp)
                qdir = mission_dir(mid) / "queue"
                os.replace(tmp, qdir / entry_id)
                fsync_dir(qdir)
                fsync_dir(TMP)
                with self.lock:
                    m = self.p["mission"]
                    m["captures"] = seq
                    m["last_capture_at"] = meta["timestamp"]
                    self.last_capture_mono = t_start
                    self.persist()
                log.info("capture %d (%s) queued", seq, entry_id)
            finally:
                if tmp.exists():
                    shutil.rmtree(tmp, ignore_errors=True)
                self.pause_uploader(False)


def build_metadata(entry_id, mid, seq, timestamp, st, camera=None):
    """metadata.json, docs/CONTRATO.md §3 (schema 2). Missing readings are null, never omitted."""
    meta = {"schema": 2, "id": entry_id, "mission_id": mid, "sequence": seq, "timestamp": timestamp,
            "position": {"north_m": None, "east_m": None, "height_m": None, "source": None, "sigma_m": None},
            "orientation": {"quaternion": None, "roll_deg": None, "pitch_deg": None, "yaw_deg": None},
            "stability": {"angular_speed_rad_s": None}}
    if camera is not None:
        meta["camera"] = camera            # real capture parameters (station's request)
    if not st:
        return meta
    p, o = st["position"], st["orientation"]
    meta["position"] = {"north_m": round(p["north_m"], 3), "east_m": round(p["east_m"], 3),
                        "height_m": round(p["height_m"], 3), "source": p["source"],
                        "sigma_m": round(p["sigma_m"], 2)}
    q = o.get("quaternion")
    meta["orientation"] = {
        "quaternion": None if q is None else {k: round(q[k], 6) for k in ("w", "x", "y", "z")},
        "roll_deg": round(o["roll_deg"], 2), "pitch_deg": round(o["pitch_deg"], 2),
        "yaw_deg": None if o.get("yaw_deg") is None else round(o["yaw_deg"], 2),
    }
    meta["stability"] = {"angular_speed_rad_s": round(st["stability"]["angular_speed_rad_s"], 4)}
    # Extra fields (accepted by the contract, shown as "outros dados")
    v = st.get("velocity", {})
    meta["velocity"] = {k: round(v[k], 3) for k in ("north_m_s", "east_m_s", "up_m_s") if k in v}
    meta["yaw_source"] = o.get("yaw_source")
    meta["mounting_measured"] = o.get("mounting_measured")
    meta["module_id"] = C.MODULE_ID
    return meta


# ---------------------------------------------------------------- HTTP API
MODULE: Module = None


class Handler(BaseHTTPRequestHandler):
    server_version = "atrium-module"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):       # quiet: the dashboard polls every 2 s
        pass

    def _send(self, status, obj):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def _drain(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n:
            self.rfile.read(min(n, 1 << 16))

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path.rstrip("/")
        if path == "/status":
            self._send(200, MODULE.status())
        else:
            self._send(404, {"detail": "Rota inexistente."})

    def do_POST(self):
        self._drain()
        u = urllib.parse.urlsplit(self.path)
        path = u.path.rstrip("/")
        q = urllib.parse.parse_qs(u.query)
        try:
            if path == "/mission/start":
                self._send(200, MODULE.start_mission())
            elif path == "/mission/finish":
                force = q.get("force", ["false"])[0].lower() in ("1", "true", "yes")
                self._send(200, MODULE.finish_mission(force))
            elif path == "/calibrate":
                self._send(200, MODULE.calibrate())
            else:
                self._send(404, {"detail": "Rota inexistente."})
        except HttpError as e:
            self._send(e.status, {"detail": e.detail})
        except Exception as e:             # noqa: BLE001
            log.exception("request failed")
            self._send(500, {"detail": f"Erro interno do módulo: {e}"})


def main():
    global MODULE
    logging.basicConfig(level=logging.INFO, format="missiond: %(message)s", stream=sys.stderr)
    C.DATA.mkdir(parents=True, exist_ok=True)
    MISSIONS.mkdir(parents=True, exist_ok=True)
    MODULE = Module()
    MODULE.start_uploader()
    for name, fn in (("nav", MODULE.nav_loop), ("health", MODULE.health_loop),
                     ("devices", MODULE.devices_loop), ("capture", MODULE.capture_loop)):
        threading.Thread(target=fn, name=name, daemon=True).start()

    class Server(ThreadingHTTPServer):
        def handle_error(self, request, client_address):
            if not isinstance(sys.exc_info()[1], (ConnectionResetError, BrokenPipeError, TimeoutError)):
                super().handle_error(request, client_address)

    srv = Server((C.API_HOST, C.API_PORT), Handler)
    srv.daemon_threads = True

    def stop(*_):
        threading.Thread(target=srv.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    log.info("listening on %s:%d (state %s)", C.API_HOST, C.API_PORT, MODULE.p["state"])
    srv.serve_forever()
    MODULE.cameras_stop()
    with MODULE.lock:
        if MODULE.track:
            MODULE.track.close()
    if MODULE.uploader and MODULE.uploader.poll() is None:
        MODULE.pause_uploader(False)
        MODULE.uploader.terminate()


if __name__ == "__main__":
    main()
