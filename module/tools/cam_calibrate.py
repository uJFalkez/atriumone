#!/usr/bin/env python3
"""Camera calibration (once, in the field). Run with the module idle (no mission running).

  python3 tools/cam_calibrate.py focus   RGB: point it at a DISTANT scene (> 20 m, detailed,
                                         good light). Sweeps the focus motor; the sharpest frame
                                         gives the largest JPEG (no decoding needed). Saves
                                         camera.rgb_focus.
  python3 tools/cam_calibrate.py awb     NIR: fill the frame with a matte white/grey sheet in
                                         daylight. Takes 3 photos with automatic white balance and
                                         saves the mean ColourGains as camera.nir_colour_gains
                                         (fixed gains, station's request).
  add -n for a dry run (nothing saved).

Writes test frames to /tmp only. Never uses ffmpeg (it hung this Pi, see docs/module/CLAUDE.md).
The values go to the calibration file's "camera" section; missiond reads them at each capture
(RGB) or when the NIR camera starts (next mission).
"""
import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "mission"))
import config as C                                  # noqa: E402
from common import read_json, write_json_atomic     # noqa: E402

TMP = Path("/tmp/cam_calibrate")


def save(key, value, dry):
    cal = read_json(C.CALIB_PATH, {}) or {}
    cam = cal.get("camera") if isinstance(cal.get("camera"), dict) else {}
    cam[key] = value
    cam[key + "_time"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    cal["camera"] = cam
    if dry:
        print(f"(dry run: {key} = {value} não foi salvo)")
        return
    write_json_atomic(C.CALIB_PATH, cal)
    print(f"salvo: camera.{key} = {value} em {C.CALIB_PATH}")


def grab(focus):
    out = TMP / f"rgb_f{focus:04d}.jpg"
    w, h = C.RGB_SIZE
    r = subprocess.run([str(C.CAMGRAB), "-d", C.RGB_DEVICE, "-W", str(w), "-H", str(h),
                        "-n", "8", "-w", str(C.RGB_WB_K), "-f", str(focus), "-o", str(out)],
                       capture_output=True, text=True, timeout=15)
    res = json.loads(r.stdout.strip().splitlines()[-1]) if r.stdout.strip() else {"ok": False}
    if not res.get("ok"):
        raise SystemExit(f"erro na câmera RGB: {res.get('error', r.stderr.strip())}")
    return out.stat().st_size


def focus(dry):
    print("Foco da RGB: aponte para uma cena DISTANTE (> 20 m) com detalhes e não mexa.")
    sizes = {}
    coarse = [1, 60, 120, 180, 240, 320, 400, 500, 620, 760, 900, 1023]
    for f in coarse:
        sizes[f] = grab(f)
        print(f"  foco {f:4d}: {sizes[f] / 1024:6.1f} KB")
    best = max(sizes, key=sizes.get)
    i = coarse.index(best)
    lo = coarse[max(0, i - 1)]
    hi = coarse[min(len(coarse) - 1, i + 1)]
    step = max(1, (hi - lo) // 8)
    for f in range(lo, hi + 1, step):
        if f not in sizes:
            sizes[f] = grab(f)
            print(f"  foco {f:4d}: {sizes[f] / 1024:6.1f} KB (refino)")
    best = max(sizes, key=sizes.get)
    ordered = sorted(sizes.values())
    median = ordered[len(ordered) // 2]
    gain = sizes[best] / median - 1
    print(f"melhor foco: {best} ({sizes[best] / 1024:.1f} KB, {gain * 100:.0f}% acima da mediana)")
    print(f"foto no melhor foco: {TMP / f'rgb_f{best:04d}.jpg'}")
    if gain < 0.05:
        print("AVISO: pouca diferença entre as posições; a cena tem pouco detalhe ou está escura. "
              "Tente de novo com uma cena mais detalhada e bem iluminada.")
        if not dry:
            print("nada salvo")
            return 2
    save("rgb_focus", int(best), dry)
    return 0


def awb(dry):
    print("Balanço de branco da NIR: folha branca/cinza fosca ocupando a imagem, sob luz do dia.")
    gains = []
    w, h = C.NIR_SIZE
    for i in range(3):
        meta = TMP / f"nir_awb_{i}.json"
        img = TMP / f"nir_awb_{i}.jpg"
        r = subprocess.run(["rpicam-still", "-n", "-t", "1500", "--width", str(w), "--height", str(h),
                            "--metadata", str(meta), "--metadata-format", "json", "-o", str(img)],
                           capture_output=True, text=True, timeout=30)
        m = read_json(meta, {}) or {}
        g = m.get("ColourGains")
        if r.returncode != 0 or not (isinstance(g, list) and len(g) == 2):
            raise SystemExit(f"erro na câmera NIR (rc {r.returncode}): {r.stderr.strip()[-200:]}")
        gains.append(g)
        print(f"  foto {i + 1}: ColourGains R={g[0]:.3f} B={g[1]:.3f}  "
              f"(exp {m.get('ExposureTime')} us, ganho {m.get('AnalogueGain')})")
    r_mean = sum(g[0] for g in gains) / len(gains)
    b_mean = sum(g[1] for g in gains) / len(gains)
    spread = max(abs(g[0] - r_mean) + abs(g[1] - b_mean) for g in gains)
    print(f"média: R={r_mean:.3f} B={b_mean:.3f} (variação {spread:.3f})")
    if not (0.3 < r_mean < 8 and 0.3 < b_mean < 8) or spread > 0.3:
        print("AVISO: ganhos instáveis ou fora do esperado (luz mudando? alvo não ocupa a imagem?). Nada salvo.")
        return 2
    save("nir_colour_gains", [round(r_mean, 4), round(b_mean, 4)], dry)
    return 0


def main():
    args = [a for a in sys.argv[1:] if a != "-n"]
    dry = "-n" in sys.argv[1:]
    if not args or args[0] not in ("focus", "awb"):
        print(__doc__)
        return 1
    TMP.mkdir(parents=True, exist_ok=True)
    return focus(dry) if args[0] == "focus" else awb(dry)


if __name__ == "__main__":
    sys.exit(main())
