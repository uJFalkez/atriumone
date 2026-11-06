"""Módulo simulado: implementa a API do módulo descrita em CONTRATO.md.

Serve para desenvolver e demonstrar a estação (dashboard + ingestão) sem o
Raspberry Pi. Não substitui nem compartilha código com raspberry/.

O que ele faz:
  - máquina de estados idle / calibrating / in_mission / uploading;
  - voo em zigue-zague sobre um "mundo" sintético (vegetação com focos
    pequenos de estresse, um lago, um telhado, uma estrada de terra);
  - fotos liberadas após o aquecimento, uma a cada intervalo quando estável;
  - pares RGB/NIR crus coerentes com alignment.npy (a estação reconstrói o
    NIR alinhado exatamente);
  - ready_to_finish ao voltar, pousado, perto do início e com a estação
    acessível; ao finalizar, envia tudo para POST /entry e apaga a cópia local.

Uso:
    python tools/mock_module.py                  # porta 8000, tempo real
    MOCK_SPEED=8 python tools/mock_module.py     # 8x mais rápido (aquecimento de 15 s)

Variáveis: MOCK_PORT, STATION_URL (padrão http://127.0.0.1:5000), MOCK_SPEED,
MOCK_DATA_DIR, ALIGNMENT_PATH, CAMERA_HFOV_DEG.
"""

import json
import math
import os
import shutil
import sys
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np
import requests
from flask import Flask, jsonify, request

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import config  # noqa: E402  (config da estação: alinhamento e FOV)


STARTED = time.time()
SPEED = float(os.environ.get("MOCK_SPEED", "1"))
PORT = int(os.environ.get("MOCK_PORT", "8000"))
STATION_URL = os.environ.get("STATION_URL", "http://127.0.0.1:5000").rstrip("/")
DATA_DIR = Path(os.environ.get("MOCK_DATA_DIR", Path(__file__).resolve().parent / ".mock_data"))

WARMUP_S = 120 / SPEED
CAPTURE_INTERVAL_S = 10 / SPEED
# Calibração em etapas (não acelera: é o operador mexendo no módulo)
CALIBRATION_STEPS = [
    ("still", 4.0, "Deixe o módulo parado sobre uma superfície firme"),
    ("figure8", 7.0, "Gire o módulo fazendo um oito"),
    ("poses", 6.0, "Apoie o módulo em cada uma das faces"),
]
CALIBRATION_S = sum(d for _, d, _ in CALIBRATION_STEPS)

# Perda de GPS simulada: fração do voo sem fix (posição só por IMU, com deriva)
GPS_DROPOUT = (0.42, 0.62)
RADIO_RANGE_M = 90.0  # em voo, só alcança a estação dentro deste raio
DRIFT_M_S = (0.35, -0.25)  # deriva do dead reckoning, por segundo de voo real

RGB_SIZE = (3264, 2448)
NIR_SIZE = (3280, 2464)
FLIGHT_HEIGHT_M = 30.0
CRUISE_M_S = 3.0 * SPEED

# ============================================================
# Mundo sintético (metros N/L a partir da estação)
# ============================================================

WORLD_N = (-40.0, 170.0)
WORLD_E = (-60.0, 150.0)
WORLD_RES = 0.08  # m/px


def _smooth_noise(shape, sigma, rng):
    noise = rng.normal(0, 1, shape).astype(np.float32)
    noise = cv2.GaussianBlur(noise, (0, 0), sigma)
    return noise / (noise.std() + 1e-6)


def build_world(seed=7):
    rng = np.random.default_rng(seed)
    h = int((WORLD_N[1] - WORLD_N[0]) / WORLD_RES)
    w = int((WORLD_E[1] - WORLD_E[0]) / WORLD_RES)

    def px(n, e):
        return int((e - WORLD_E[0]) / WORLD_RES), int((WORLD_N[1] - n) / WORLD_RES)

    vigor = 0.74 + 0.07 * _smooth_noise((h, w), 60, rng) + 0.03 * _smooth_noise((h, w), 8, rng)
    rows = 0.5 + 0.5 * np.sin(np.arange(h, dtype=np.float32) * WORLD_RES * 2 * math.pi / 0.9)[:, None]
    vigor = vigor - 0.05 * rows
    cls = np.zeros((h, w), np.uint8)  # 0 vegetação, 1 solo, 2 água, 3 telhado

    # Estrada de terra
    cv2.line(cls, px(-40, 18), px(170, 34), 1, int(4 / WORLD_RES))
    # Lago
    cv2.ellipse(cls, px(112, 96), (int(19 / WORLD_RES), int(12 / WORLD_RES)), 20, 0, 360, 2, -1)
    # Telhado de um galpão
    x0, y0 = px(46, 92)
    x1, y1 = px(34, 112)
    cv2.rectangle(cls, (x0, y0), (x1, y1), 3, -1)
    # Pátio da estação
    cv2.rectangle(cls, px(4, -4), px(-4, 4), 1, -1)

    # Focos pequenos de estresse dentro da vegetação
    spots = []
    while len(spots) < 16:
        n, e = rng.uniform(15, 150), rng.uniform(-40, 130)
        cx, cy = px(n, e)
        r = rng.uniform(0.7, 1.9)
        rr = int(r / WORLD_RES) + int(6 / WORLD_RES)
        if cls[max(0, cy - rr):cy + rr, max(0, cx - rr):cx + rr].any():
            continue
        mask = np.zeros((h, w), np.uint8)
        axes = (int(r / WORLD_RES), int(r * rng.uniform(0.6, 1.0) / WORLD_RES))
        cv2.ellipse(mask, (cx, cy), axes, rng.uniform(0, 180), 0, 360, 1, -1)
        mask = cv2.GaussianBlur(mask.astype(np.float32), (0, 0), 3)
        vigor -= mask * rng.uniform(0.45, 0.65)
        spots.append((round(n, 2), round(e, 2), round(r, 2)))
    vigor = np.clip(vigor, 0.05, 0.95)

    # Cores (0–255) e NIR
    red = 40 + (1 - vigor) * 125
    green = 72 + vigor * 55 + (1 - vigor) * 45
    blue = 28 + (1 - vigor) * 32
    nir = 85 + vigor * 155

    tone = 1 + 0.06 * _smooth_noise((h, w), 4, rng)
    for value, (r, g, b, n) in {1: (158, 128, 96, 150), 2: (28, 52, 64, 18), 3: (176, 178, 180, 170)}.items():
        m = cls == value
        red[m], green[m], blue[m], nir[m] = r * tone[m], g * tone[m], b * tone[m], n * tone[m]

    rgb = np.dstack([blue, green, red]).clip(0, 255).astype(np.uint8)  # BGR
    return rgb, nir.clip(0, 255).astype(np.uint8), spots


# ============================================================
# Câmera: mesmo modelo que a estação usa para georreferenciar
# ============================================================

def rotation_body_to_ned(roll, pitch, yaw):
    cr, sr, cp, sp, cy, sy = math.cos(roll), math.sin(roll), math.cos(pitch), math.sin(pitch), math.cos(yaw), math.sin(yaw)
    return np.array([
        [cy * cp, cy * sp * sr - sy * cr, cy * sp * cr + sy * sr],
        [sy * cp, sy * sp * sr + cy * cr, sy * sp * cr - cy * sr],
        [-sp, cp * sr, cp * cr],
    ], np.float32)


def quaternion(roll, pitch, yaw):
    cr, sr = math.cos(roll / 2), math.sin(roll / 2)
    cp, sp = math.cos(pitch / 2), math.sin(pitch / 2)
    cy, sy = math.cos(yaw / 2), math.sin(yaw / 2)
    return {
        "w": cr * cp * cy + sr * sp * sy,
        "x": sr * cp * cy - cr * sp * sy,
        "y": cr * sp * cy + sr * cp * sy,
        "z": cr * cp * sy - sr * sp * cy,
    }


class Renderer:

    def __init__(self):
        print("Gerando mundo sintético…")
        self.rgb, self.nir, self.spots = build_world()
        self.warp = np.load(config.ALIGNMENT_PATH).astype(np.float32)
        w, h = RGB_SIZE
        self.focal = (w / 2) / math.tan(math.radians(config.CAMERA_HFOV_DEG) / 2)
        u, v = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
        self.rays = np.stack([-(v - h / 2) / self.focal, (u - w / 2) / self.focal, np.ones_like(u)], -1)
        self.rng = np.random.default_rng()
        print(f"Mundo pronto: {len(self.spots)} focos plantados.")

    def render(self, north, east, height, roll, pitch, yaw):
        rotation = rotation_body_to_ned(roll, pitch, yaw)
        ned = self.rays @ rotation.T
        scale = height / np.maximum(ned[..., 2], 1e-3)
        n = north + ned[..., 0] * scale
        e = east + ned[..., 1] * scale
        map_x = ((e - WORLD_E[0]) / WORLD_RES).astype(np.float32)
        map_y = ((WORLD_N[1] - n) / WORLD_RES).astype(np.float32)
        del ned, scale, n, e

        rgb = cv2.remap(self.rgb, map_x, map_y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REFLECT)
        nir = cv2.remap(self.nir, map_x, map_y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REFLECT)

        # Textura fina e ruído de sensor
        grain = self.rng.normal(1.0, 0.05, rgb.shape[:2]).astype(np.float32)
        rgb = np.clip(rgb * grain[..., None], 0, 255).astype(np.uint8)
        nir = np.clip(nir * grain + self.rng.normal(0, 2, nir.shape), 0, 255).astype(np.uint8)

        # NIR cru: desfaz o alinhamento que a estação vai aplicar.
        nir_raw = cv2.warpAffine(nir, self.warp, RGB_SIZE, flags=cv2.INTER_LINEAR, borderValue=0)
        nir_raw = cv2.resize(nir_raw, NIR_SIZE, interpolation=cv2.INTER_LINEAR)
        return rgb, cv2.merge([nir_raw, nir_raw, nir_raw])


# ============================================================
# Plano de voo
# ============================================================

def flight_plan():
    lines = [-30, 0, 30, 60, 90, 120]
    waypoints = [(0, 0, 0), (0, 0, FLIGHT_HEIGHT_M)]
    for i, east in enumerate(lines):
        legs = [(20, east), (150, east)] if i % 2 == 0 else [(150, east), (20, east)]
        waypoints += [(n, e, FLIGHT_HEIGHT_M) for n, e in legs]
    waypoints += [(0, 0, FLIGHT_HEIGHT_M), (0.8, -0.6, 0)]

    segments = []
    for a, b in zip(waypoints, waypoints[1:]):
        distance = math.dist(a, b)
        vertical = abs(b[2] - a[2]) > 1
        speed = (2.0 * SPEED) if vertical else CRUISE_M_S
        segments.append((max(distance / speed, 0.5), a, b))
        segments.append((2.0 / SPEED, b, b))  # curva/pairar: instável
    return segments


def sample(plan, t):
    for duration, a, b in plan:
        if t <= duration:
            k = t / duration
            pos = tuple(x + (y - x) * k for x, y in zip(a, b))
            vel = tuple((y - x) / duration for x, y in zip(a, b))
            return pos, vel, a == b
        t -= duration
    return plan[-1][2], (0, 0, 0), True


# ============================================================
# Estado do módulo
# ============================================================

def iso(ts=None):
    return datetime.fromtimestamp(ts or time.time(), timezone.utc).isoformat()


class Module:

    def __init__(self):
        self.lock = threading.RLock()
        self.state = "idle"
        self.calibrated_at = None
        self.calibration_started = None
        self.mission = None
        self.track = []
        self.final = None
        self.plan = None
        self.t_fly = None
        self.yaw = 0.0
        self.upload = {"pending": 0, "sent": 0, "total": 0}
        self.station_ok = False
        self.renderer = None
        self.renderer_ready = threading.Event()
        self.queue_dir = DATA_DIR / "pending"
        self.queue_dir.mkdir(parents=True, exist_ok=True)
        threading.Thread(target=self._loop, daemon=True).start()
        threading.Thread(target=self._health_loop, daemon=True).start()
        threading.Thread(target=self._build_renderer, daemon=True).start()
        threading.Thread(target=self._uploader, daemon=True).start()

    def _build_renderer(self):
        self.renderer = Renderer()
        self.renderer_ready.set()

    # ----- comandos -----

    def calibrate(self):
        with self.lock:
            if self.state != "idle":
                raise ValueError(f"Não é possível calibrar agora ({self.state}).")
            self.state = "calibrating"
            self.calibration_started = time.time()
            return self._calibration()

    def start(self):
        with self.lock:
            if self.state != "idle":
                raise ValueError(f"Módulo ocupado ({self.state}).")
            now = time.time()
            self.mission = {
                "id": str(uuid.uuid4()),
                "started_at": iso(now),
                "captures_enabled_at": iso(now + WARMUP_S),
                "captures": 0,
                "last_capture_at": None,
                "ready_to_finish": False,
                "_t0": now,
                "_last_capture": 0.0,
                "_stable_since": None,
                "_still_since": None,
            }
            self.plan = flight_plan()
            self.t_fly = now + WARMUP_S  # decola depois do aquecimento
            self.track = []  # histórico de posições: (t_s, north, east, height, source, sigma)
            self.upload = {"pending": 0, "sent": 0, "total": 0}
            self.final = None
            self.state = "in_mission"
            return self._public_mission()

    def finish(self, force=False):
        with self.lock:
            if self.state != "in_mission":
                raise ValueError("Nenhuma missão em andamento.")
            if not force and not self.mission["ready_to_finish"]:
                raise ValueError("O módulo ainda não voltou à estação.")
            mission = self._public_mission()
            mission["finished_at"] = iso()
            ready = self.mission["ready_to_finish"]
            self.state = "uploading"
            record = {
                "id": mission["id"],
                "module_id": "atrium-one-sim",
                "started_at": mission["started_at"],
                "finished_at": mission["finished_at"],
                "captures": mission["captures"],
                "forced": bool(force and not ready),
            }
            # Fotos restantes e, por último, o registro da missão: o laço de envio cuida.
            self.final = (record, list(self.track))
            return mission

    # ----- simulação -----

    def _pose(self):
        """Pose atual: (north, east, height, roll, pitch, yaw, speed, angular_rate)."""
        if self.state != "in_mission" or time.time() < (self.t_fly or 0):
            return 0.0, 0.0, 0.0, 0.0, 0.0, self.yaw, 0.0, 0.01
        (n, e, h), (vn, ve, vh), hovering = sample(self.plan, time.time() - self.t_fly)
        speed = math.hypot(vn, ve)
        if speed > 0.3:
            self.yaw = math.degrees(math.atan2(ve, vn)) % 360
        turning = hovering and h > 1
        rate = abs(np.random.normal(0.6 if turning else 0.03, 0.02))
        roll = np.random.normal(0, 6 if turning else 1.2)
        pitch = np.random.normal(-4 if speed > 0.3 else 0, 1.2)
        return n, e, h, roll, pitch, self.yaw, speed, rate

    def _loop(self):
        while True:
            time.sleep(0.1)
            with self.lock:
                if self.state == "calibrating" and time.time() - self.calibration_started >= CALIBRATION_S:
                    self.state = "idle"
                    self.calibrated_at = iso()
                if self.state != "in_mission":
                    continue
                m = self.mission
                n, e, h, roll, pitch, yaw, speed, rate = self._pose()
                now = time.time()
                t_s = now - m["_t0"]
                source, dn, de, sigma = self._gps(now)
                if not self.track or t_s - self.track[-1][0] >= 0.2:  # 5 Hz
                    self.track.append((
                        round(t_s, 2),
                        round(n + dn + np.random.normal(0, 0.15), 3),
                        round(e + de + np.random.normal(0, 0.15), 3),
                        round(h, 2), source, sigma,
                    ))

                m["_stable_since"] = (m["_stable_since"] or now) if rate < 0.15 else None
                m["_still_since"] = (m["_still_since"] or now) if speed < 0.2 and h < 1.5 else None
                flown = now - (self.t_fly or now) > 5
                landed = flown and m["_still_since"] and now - m["_still_since"] > 2
                if source == "gps+imu":
                    m["ready_to_finish"] = bool(landed and math.hypot(n, e) < 10 and h < 2 and self.station_ok)
                else:  # sem fix: basta estar pousado e alcançar a estação
                    m["ready_to_finish"] = bool(landed and self.station_ok)

                due = (
                    now >= m["_t0"] + WARMUP_S
                    and now - m["_last_capture"] >= CAPTURE_INTERVAL_S
                    and m["_stable_since"] and now - m["_stable_since"] > 0.3
                    and h > 5
                )
                if not due:
                    continue
                m["_last_capture"] = now
                m["captures"] += 1
                m["last_capture_at"] = iso(now)
                shot = (m["id"], m["captures"], now, n, e, h, roll, pitch, yaw, rate, (source, dn, de, sigma))
            threading.Thread(target=self._capture, args=shot, daemon=True).start()

    def _capture(self, mission_id, sequence, ts, n, e, h, roll, pitch, yaw, rate, gps):
        source, dn, de, sigma = gps
        self.renderer_ready.wait()
        capture_id = str(uuid.uuid4())
        rgb, nir = self.renderer.render(n, e, h, math.radians(roll), math.radians(pitch), math.radians(yaw))
        tmp = DATA_DIR / "tmp" / capture_id
        tmp.mkdir(parents=True)
        cv2.imwrite(str(tmp / "rgb.jpg"), rgb, [cv2.IMWRITE_JPEG_QUALITY, 92])
        cv2.imwrite(str(tmp / "nir.jpg"), nir, [cv2.IMWRITE_JPEG_QUALITY, 92])
        metadata = {
            "schema": 2,
            "id": capture_id,
            "mission_id": mission_id,
            "sequence": sequence,
            "timestamp": iso(ts),
            "position": {
                # a imagem é renderizada na posição real; a reportada carrega a deriva
                "north_m": round(n + dn + np.random.normal(0, 0.3), 3),
                "east_m": round(e + de + np.random.normal(0, 0.3), 3),
                "height_m": round(h + np.random.normal(0, 0.6), 2),  # acelerômetro: impreciso
                "source": source,
                "sigma_m": sigma,
            },
            "orientation": {
                "quaternion": {k: round(v, 6) for k, v in quaternion(*map(math.radians, (roll, pitch, yaw))).items()},
                "roll_deg": round(roll, 2),
                "pitch_deg": round(pitch, 2),
                "yaw_deg": round(yaw, 2),
            },
            "stability": {"angular_speed_rad_s": round(rate, 4)},
        }
        (tmp / "metadata.json").write_text(json.dumps(metadata, indent=2))
        tmp.rename(self.queue_dir / capture_id)
        with self.lock:
            self.upload["total"] = max(self.upload["total"], sequence)
            self.upload["pending"] = len(list(self.queue_dir.iterdir()))
        print(f"Foto {sequence} capturada em N{n:.0f} L{e:.0f} h{h:.0f}")

    def _upload_mission(self, record, track):
        csv_text = "t_s,north_m,east_m,height_m,source,sigma_m\n" + "".join(f"{t},{n},{e},{h},{src},{sig}\n" for t, n, e, h, src, sig in track)
        try:
            r = requests.post(
                f"{STATION_URL}/mission",
                data={"mission_id": record["id"]},
                files={"mission": ("mission.json", json.dumps(record), "application/json"), "track": ("track.csv", csv_text, "text/csv")},
                timeout=30,
            )
        except requests.RequestException as error:
            print(f"Falha ao registrar missão: {error}")
            return False
        if r.status_code in (200, 201):
            print(f"Missão registrada na estação: {len(track)} posições")
            return True
        print(f"Estação recusou o registro da missão: {r.status_code} {r.text}")
        return r.status_code == 400  # bug do cliente: não adianta repetir

    def _upload_entry(self, folder):
        try:
            with open(folder / "rgb.jpg", "rb") as rgb, open(folder / "nir.jpg", "rb") as nir, open(folder / "metadata.json", "rb") as meta:
                r = requests.post(
                    f"{STATION_URL}/entry",
                    data={"entry_id": folder.name},
                    files={"rgb": ("rgb.jpg", rgb, "image/jpeg"), "nir": ("nir.jpg", nir, "image/jpeg"), "metadata": ("metadata.json", meta, "application/json")},
                    timeout=30,
                )
        except requests.RequestException as error:
            print(f"Falha ao enviar {folder.name}: {error}")
            return False
        if r.status_code in (200, 201) and r.json().get("entry_id") == folder.name:
            shutil.rmtree(folder)  # confirmado: apaga do "Pi"
            return True
        return False

    def _uploader(self):
        """Envia a fila sempre que alcança a estação, inclusive em voo.

        Em voo, o rádio só alcança a estação dentro de RADIO_RANGE_M. Ao
        finalizar: fotos restantes e, por último, POST /mission.
        """
        while True:
            time.sleep(0.3)
            with self.lock:
                state = self.state
                if state not in ("in_mission", "uploading"):
                    continue
                n, e = self._pose()[:2]
                reachable = self.station_ok and (state == "uploading" or math.hypot(n, e) < RADIO_RANGE_M)
                final = self.final
            if not reachable:
                continue
            items = sorted(self.queue_dir.iterdir(), key=lambda p: p.stat().st_mtime)
            if items:
                ok = self._upload_entry(items[0])
                with self.lock:
                    if ok:
                        self.upload["sent"] += 1
                    self.upload["pending"] = len(list(self.queue_dir.iterdir()))
                if not ok:
                    time.sleep(2)
                continue
            if state == "uploading" and final:
                if self._upload_mission(*final):
                    with self.lock:
                        self.state, self.mission, self.final = "idle", None, None
                else:
                    time.sleep(2)

    def _health_loop(self):
        while True:
            try:
                self.station_ok = requests.get(f"{STATION_URL}/health", timeout=1.5).ok
            except requests.RequestException:
                self.station_ok = False
            time.sleep(2)

    # ----- status -----

    def _calibration(self):
        if self.state != "calibrating":
            return None
        elapsed = time.time() - self.calibration_started
        step, instruction, acc = "done", "Calibração concluída", 0.0
        for name, duration, text in CALIBRATION_STEPS:
            if elapsed < acc + duration:
                step, instruction = name, text
                break
            acc += duration
        return {
            "started_at": iso(self.calibration_started),
            "step": step,
            "instruction": instruction,
            "remaining_s": round(max(0, CALIBRATION_S - elapsed), 1),
            "progress": round(min(1, elapsed / CALIBRATION_S), 3),
        }

    def _gps(self, now):
        """(source, deriva_n, deriva_l, sigma_m) da posição reportada agora."""
        if self.plan is None or self.t_fly is None or now < self.t_fly:
            return "gps+imu", 0.0, 0.0, 2.5
        total = sum(d for d, _, _ in self.plan)
        start, end = (total * f for f in GPS_DROPOUT)
        t = now - self.t_fly
        if not start <= t < end:
            return "gps+imu", 0.0, 0.0, 2.5
        flown_s = (t - start) * SPEED  # segundos "reais" de voo sem fix
        dn, de = DRIFT_M_S[0] * flown_s, DRIFT_M_S[1] * flown_s
        return "imu", dn, de, round(3 + math.hypot(dn, de), 1)

    def _public_mission(self):
        if not self.mission:
            return None
        return {k: v for k, v in self.mission.items() if not k.startswith("_")}

    def status(self):
        with self.lock:
            n, e, h, _, _, yaw, _, _ = self._pose()
            in_flight = self.state == "in_mission"
            fix = self._gps(time.time())[0] == "gps+imu" if in_flight else True
            gps_device = (
                {"id": "gps", "label": "GPS · NEO-6M", "connected": True, "status": "ok", "data": {"satélites": 9}}
                if fix else
                {"id": "gps", "label": "GPS · NEO-6M", "connected": True, "status": "warning", "message": "sem fix: posição só pela IMU", "data": {"satélites": 2}}
            )
            return {
                "module_id": "atrium-one-sim",
                "version": "sim-1.0",
                "uptime_s": round(time.time() - STARTED),
                "state": self.state,
                "devices": [
                    {"id": "imu", "label": "IMU · MPU6500", "connected": True, "status": "ok", "data": {"taxa": "100 Hz"}},
                    gps_device,
                    {"id": "mag", "label": "Mag · QMC5883P", "connected": True, "status": "ok" if self.calibrated_at else "warning", "message": None if self.calibrated_at else "aguardando calibração"},
                    {"id": "camera_rgb", "label": "RGB · Arducam 8MP USB", "connected": True},
                    {"id": "camera_nir", "label": "NIR · Pi Camera V2 NoIR", "connected": True},
                ],
                "sensors": {"calibrated": self.calibrated_at is not None, "calibrated_at": self.calibrated_at},
                "calibration": self._calibration(),
                "mission": self._public_mission() if self.state in ("in_mission", "uploading") else None,
                "upload": dict(self.upload),
                "live": {"north_m": round(n, 2), "east_m": round(e, 2), "height_m": round(h, 2), "yaw_deg": round(yaw, 1)} if in_flight else None,
            }


# ============================================================
# API
# ============================================================

app = Flask(__name__)
module = Module()


def conflict(error):
    return jsonify(detail=str(error)), 409


@app.get("/health")
def health():
    return jsonify(status="ok", module_id="atrium-one-sim")


@app.get("/status")
def status():
    return jsonify(module.status())


@app.post("/mission/start")
def mission_start():
    try:
        return jsonify(mission=module.start())
    except ValueError as error:
        return conflict(error)


@app.post("/mission/finish")
def mission_finish():
    try:
        return jsonify(mission=module.finish(force=request.args.get("force") == "true"))
    except ValueError as error:
        return conflict(error)


@app.post("/calibrate")
def calibrate():
    try:
        return jsonify(calibration=module.calibrate())
    except ValueError as error:
        return conflict(error)


if __name__ == "__main__":
    from waitress import serve

    print(f"Módulo simulado em :{PORT} (velocidade {SPEED}x) -> estação {STATION_URL}")
    serve(app, host="0.0.0.0", port=PORT, threads=8)
