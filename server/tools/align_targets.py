"""Calibra o alinhamento RGB→NIR a partir de fotos com alvos.

Alvos: quadrados azuis (com círculo e cruz brancos) no chão, visíveis nas
duas câmeras. De cada alvo saem o centro e os 4 cantos; a transformação afim
é ajustada só com fotos em que o módulo estava parado (o movimento entre os
disparos e a leitura linha a linha dos sensores desalinham as duas imagens).

Saída: matriz 2x3 que leva um pixel da RGB ao pixel correspondente da NIR já
redimensionada para o tamanho da RGB (mesma convenção de server/processing.py,
usada com cv2.WARP_INVERSE_MAP). Depois de gerar, ajuste ALIGNMENT_RGB_SIZE em
server/config.py para a resolução da RGB usada aqui e rode reprocess.py --all.

Uso (de dentro de server/):
    python tools/align_targets.py ../entries <mission_id (prefixo)> calibration/alignment.npy
        [--max-angular 0.15] [--relative-to calibration/alignment.npy]

--relative-to: em vez da afim livre (6 parâmetros), ajusta só escala uniforme
e translação sobre a parte linear de outra calibração. Use quando houver poucos
alvos (ex.: um por foto), caso em que a afim livre não fica bem determinada.
Foi assim que se gerou calibration/alignment_focus57.npy (missão 6b93dceb).
"""

import argparse
import glob
import json
import os

import cv2
import numpy as np


def target_mask(img):
    """Pixels do alvo pelo matiz azul (independe do brilho/vinheta), com buracos preenchidos."""
    h, s, v = cv2.split(cv2.cvtColor(img, cv2.COLOR_BGR2HSV))
    m = ((h >= 92) & (h <= 125) & (s >= 90) & (v >= 70)).astype(np.uint8) * 255
    m = cv2.morphologyEx(m, cv2.MORPH_CLOSE, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (9, 9)))
    contours, _ = cv2.findContours(m, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    filled = np.zeros_like(m)
    cv2.drawContours(filled, contours, -1, 255, cv2.FILLED)
    return filled


def detect(img, min_area=300, max_area=20000):
    """Alvos inteiros no quadro: centro e 4 cantos (subpixel), da esquerda para a direita."""
    contours, _ = cv2.findContours(target_mask(img), cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    h, w = img.shape[:2]
    found = []
    for c in contours:
        area = cv2.contourArea(c)
        if not min_area <= area <= max_area:
            continue
        x, y, bw, bh = cv2.boundingRect(c)
        if x <= 2 or y <= 2 or x + bw >= w - 2 or y + bh >= h - 2:
            continue
        rect = cv2.minAreaRect(c)
        if area / (rect[1][0] * rect[1][1] + 1e-6) < 0.7 or max(rect[1]) / max(1, min(rect[1])) > 1.4:
            continue
        hull = cv2.convexHull(c)
        quad = cv2.approxPolyDP(hull, 0.04 * cv2.arcLength(hull, True), True).reshape(-1, 2)
        if len(quad) != 4:
            quad = cv2.boxPoints(rect)
        corners = cv2.cornerSubPix(
            gray, quad.astype(np.float32).reshape(-1, 1, 2), (5, 5), (-1, -1),
            (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 50, 0.01),
        ).reshape(-1, 2)
        found.append({"center": corners.mean(0), "corners": corners})
    return sorted(found, key=lambda t: t["center"][0])


def apply(M, points):
    return np.asarray(points, float) @ np.asarray(M, float)[:, :2].T + np.asarray(M, float)[:, 2]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("entries_dir")
    parser.add_argument("mission")
    parser.add_argument("output")
    parser.add_argument("--max-angular", type=float, default=0.15, help="rad/s; acima disso a foto não entra no ajuste")
    parser.add_argument("--relative-to", help="matriz base: ajusta só escala + translação sobre a parte linear dela")
    args = parser.parse_args()

    photos = []
    for d in sorted(glob.glob(os.path.join(args.entries_dir, "*", ""))):
        try:
            meta = json.load(open(os.path.join(d, "metadata.json")))
        except (OSError, ValueError):
            continue
        if meta.get("mission_id", "").startswith(args.mission):
            photos.append((meta["sequence"], d, meta))

    pairs, angular, size, partial = {}, {}, None, {}
    for seq, d, meta in sorted(photos):
        rgb = cv2.imread(os.path.join(d, "rgb.jpg"))
        nir = cv2.imread(os.path.join(d, "nir.jpg"))
        size = (rgb.shape[1], rgb.shape[0])
        scale = np.array([size[0] / nir.shape[1], size[1] / nir.shape[0]])
        tr, tn = detect(rgb), detect(nir)
        for t in tn:
            t["center"] = t["center"] * scale
            t["corners"] = t["corners"] * scale
        angular[seq] = (meta.get("stability") or {}).get("angular_speed_rad_s") or 0.0
        if tr and len(tr) == len(tn):
            pairs[seq] = list(zip(tr, tn))
        elif tr and tn:
            partial[seq] = (tr, tn)  # um alvo a mais numa das câmeras: pareado depois

    # Fotos com contagens diferentes: pareia cada alvo da RGB com o da NIR mais
    # próximo da posição prevista por uma primeira estimativa.
    first = [s for s in pairs if angular[s] <= args.max_angular] or list(pairs)
    if first and partial:
        P = [a["center"] for s in first for a, b in pairs[s]]
        Q = [b["center"] for s in first for a, b in pairs[s]]
        M0, _ = cv2.estimateAffine2D(np.float32(P), np.float32(Q), method=cv2.LMEDS)
        for seq, (tr, tn) in partial.items():
            matched = []
            for a in tr:
                pred = apply(M0, [a["center"]])[0]
                b = min(tn, key=lambda t: np.linalg.norm(t["center"] - pred))
                if np.linalg.norm(b["center"] - pred) < 30:
                    matched.append((a, b))
            if matched:
                pairs[seq] = matched

    still = [s for s in pairs if angular[s] <= args.max_angular]
    moving = [s for s in pairs if s not in still]
    print(f"fotos com alvos nas duas câmeras: {sorted(pairs)} | paradas: {still} | em movimento (ignoradas): {moving}")
    if len(still) < 2:
        raise SystemExit("São necessárias pelo menos 2 fotos paradas com alvos.")

    def points(seqs, M0=None):
        P = [a["center"] for s in seqs for a, b in pairs[s]]
        Q = [b["center"] for s in seqs for a, b in pairs[s]]
        if M0 is None:
            M0, _ = cv2.estimateAffine2D(np.float32(P), np.float32(Q), method=cv2.LMEDS)
        for s in seqs:
            for a, b in pairs[s]:
                for i, c in enumerate(apply(M0, a["corners"])):
                    j = int(np.argmin(np.linalg.norm(b["corners"] - c, axis=1)))
                    if np.linalg.norm(b["corners"][j] - c) < 10:
                        P.append(a["corners"][i])
                        Q.append(b["corners"][j])
        return np.float32(P), np.float32(Q)

    base = np.load(args.relative_to).astype(float) if args.relative_to else None

    def fit(seqs):
        P, Q = points(seqs)
        if base is None:
            M, _ = cv2.estimateAffine2D(P, Q, method=cv2.LMEDS)
            return M
        X = P.astype(float) @ base[:, :2].T          # escala uniforme k e translação t: Q ≈ k·A·P + t
        Xc, Qc = X - X.mean(0), Q - Q.mean(0)
        k = float((Xc * Qc).sum() / (Xc * Xc).sum())
        return np.c_[k * base[:, :2], Q.mean(0) - k * X.mean(0)]

    def error(M, seq):
        P, Q = points([seq], M)
        return float(np.linalg.norm(apply(M, P) - Q, axis=1).mean())

    if len(still) >= 3:
        loo = [error(fit([o for o in still if o != s]), s) for s in still]
        print(f"validação deixando uma foto de fora: média {np.mean(loo):.2f} px, máx {np.max(loo):.2f} px")
    M = fit(still)
    for s in moving:
        print(f"  foto em movimento {s} ({angular[s]:.2f} rad/s): erro {error(M, s):.2f} px")
    A = M[:, :2]
    print(f"escala x {np.hypot(*A[:, 0]):.4f} y {np.hypot(*A[:, 1]):.4f} | rotação {np.degrees(np.arctan2(A[1, 0], A[0, 0])):.2f}° "
          f"| translação ({M[0, 2]:.1f}, {M[1, 2]:.1f}) px | RGB {size[0]}x{size[1]}")
    np.save(args.output, M.astype(np.float32))
    print(f"salvo em {args.output}; ajuste ALIGNMENT_RGB_SIZE = {size} em server/config.py")


if __name__ == "__main__":
    main()
