"""Processamento de uma entrada na estação.

Entrada (gravada pelo módulo):  rgb.jpg, nir.jpg, metadata.json
Saídas geradas aqui:
    ndvi.npy            NDVI recortado, uint8 (0 -> -1, 127 -> 0, 255 -> +1)
    analysis.json       estatísticas, focos de NDVI baixo e georreferenciamento
    preview/rgb.jpg     RGB recortada em resolução de visualização
    preview/ndvi.png    NDVI colorido, mesmo recorte
    preview/thumb.jpg   miniatura
analysis.json é gravado por último e marca a entrada como processada.
Em caso de falha, error.json registra o motivo.
Formato de metadata.json e georreferenciamento: ver docs/CONTRATO.md.
"""

import json
import math
import os
import traceback
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np

import config


ANALYSIS_VERSION = 4  # 4: escala de cores -0,2..+0,6


# ============================================================
# Utilidades
# ============================================================

def _write_atomic(path: Path, data: bytes):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp")
    temporary.write_bytes(data)
    os.replace(temporary, path)


def _json_default(value):
    if isinstance(value, np.generic):
        return value.item()
    raise TypeError(f"{type(value).__name__} não serializável")


def _write_json(path: Path, value):
    text = json.dumps(value, indent=2, ensure_ascii=False, default=_json_default)
    _write_atomic(path, text.encode("utf-8"))


def _encode(extension: str, image, params=()):
    ok, buffer = cv2.imencode(extension, image, list(params))
    if not ok:
        raise RuntimeError(f"Falha ao codificar {extension}")
    return buffer.tobytes()


def _resize_to_width(image, width, interpolation=cv2.INTER_AREA):
    height = round(image.shape[0] * width / image.shape[1])
    return cv2.resize(image, (width, height), interpolation=interpolation)


def _get(source, *keys):
    for key in keys:
        if not isinstance(source, dict):
            return None
        source = source.get(key)
    return source


# ============================================================
# Alinhamento e NDVI
# ============================================================

_matrices = {}


def base_matrix(mission_id=None):
    """Matriz calibrada para a missão (ou a padrão). Retorna (matriz, arquivo).

    A relação RGB/NIR muda quando o módulo é mexido entre voos; por isso a
    calibração é por missão (config.ALIGNMENT_BY_MISSION, por prefixo do id).
    """
    path = config.ALIGNMENT_PATH
    for prefix, candidate in config.ALIGNMENT_BY_MISSION.items():
        if mission_id and mission_id.startswith(prefix):
            path = candidate
            break
    path = Path(path)
    if path not in _matrices:
        matrix = np.load(path).astype(np.float32)
        if matrix.shape != (2, 3):
            raise RuntimeError(f"Matriz de alinhamento inválida em {path}: {matrix.shape}")
        _matrices[path] = matrix
    return _matrices[path], path.name


def warp_matrix():
    return base_matrix()[0]


def matrix_for(width, height, base=None):
    """Matriz de alinhamento convertida para a resolução da foto.

    As matrizes foram calibradas com a RGB em config.ALIGNMENT_RGB_SIZE. Com a
    foto em outra resolução (mesmo campo de visão, só reescalada), as
    coordenadas mudam por S = diag(sx, sy): a parte linear vira S·A·S⁻¹ e o
    deslocamento vira S·t.
    """
    base = warp_matrix() if base is None else base
    cal_w, cal_h = config.ALIGNMENT_RGB_SIZE
    sx, sy = width / cal_w, height / cal_h
    if sx == 1 and sy == 1:
        return np.float32(base)
    a = np.asarray(base, np.float64)
    scaled = np.array([
        [a[0, 0], a[0, 1] * sx / sy, a[0, 2] * sx],
        [a[1, 0] * sy / sx, a[1, 1], a[1, 2] * sy],
    ])
    return scaled.astype(np.float32)


def _ecc_band(img):
    """Banda usada no registro: média de azul e verde (a mesma do align.py legado)."""
    band = (img[:, :, 0].astype(np.float32) + img[:, :, 1].astype(np.float32)) * 0.5
    return cv2.normalize(band, None, 0, 1, cv2.NORM_MINMAX)


def refine_alignment(rgb, nir_resized, M0):
    """Ajusta a matriz desta foto por ECC, partindo da calibração.

    As duas câmeras não registram a cena exatamente no mesmo instante e, com o
    módulo em movimento, a relação entre elas varia de foto para foto. O ECC
    corrige isso pela imagem inteira; a correção só é aceita dentro de limites
    físicos plausíveis em relação à calibração (senão fica a matriz calibrada).
    Validado nos alvos das missões 09 e 10: erro médio de 8,8 → 1,9 px.
    """
    info = {"refined": False, "cc": None, "reason": None}
    if not config.ALIGNMENT_REFINE:
        info["reason"] = "desativado"
        return M0, info
    h, w = rgb.shape[:2]
    S = config.REFINE_SCALE
    rs = cv2.resize(_ecc_band(rgb), None, fx=S, fy=S, interpolation=cv2.INTER_AREA)
    ns = cv2.resize(_ecc_band(nir_resized), None, fx=S, fy=S, interpolation=cv2.INTER_AREA)
    W = np.float32(M0).copy()
    W[:, 2] *= S
    try:
        cc, W = cv2.findTransformECC(
            rs, ns, W, cv2.MOTION_AFFINE,
            (cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_COUNT, 300, 1e-6), None, 5,
        )
    except cv2.error:
        info["reason"] = "ECC não convergiu"
        return M0, info
    W = W.astype(np.float64)
    W[:, 2] /= S
    info["cc"] = round(float(cc), 3)

    rel = W[:, :2] @ np.linalg.inv(np.asarray(M0, np.float64)[:, :2])
    sx, sy = np.hypot(*rel[:, 0]), np.hypot(*rel[:, 1])
    rot = abs(np.degrees(np.arctan2(rel[1, 0], rel[0, 0])))
    grid = np.array([[x, y] for x in (0, w / 2, w) for y in (0, h / 2, h)], np.float64)
    move = lambda M: grid @ np.asarray(M, np.float64)[:, :2].T + np.asarray(M, np.float64)[:, 2]
    shift = float(np.linalg.norm(move(W) - move(M0), axis=1).max())
    max_shift = config.REFINE_MAX_SHIFT_PX * w / config.ALIGNMENT_RGB_SIZE[0]
    if cc < config.REFINE_MIN_CC:
        info["reason"] = f"correlação baixa ({cc:.2f})"
    elif max(abs(sx - 1), abs(sy - 1)) > config.REFINE_MAX_SCALE or rot > config.REFINE_MAX_ROT_DEG or shift > max_shift:
        info["reason"] = "fora dos limites plausíveis"
    else:
        info["refined"] = True
        info["shift_px"] = round(shift, 1)
        return W.astype(np.float32), info
    return M0, info


def align_nir(nir, width, height, matrix=None):
    if nir.shape[:2] != (height, width):
        nir = cv2.resize(nir, (width, height), interpolation=cv2.INTER_LINEAR)
    return cv2.warpAffine(
        nir,
        matrix_for(width, height) if matrix is None else matrix,
        (width, height),
        flags=cv2.INTER_LINEAR | cv2.WARP_INVERSE_MAP,
        borderMode=cv2.BORDER_CONSTANT,
        borderValue=0,
    )


_crop_cache = {}


def crop_box(width, height, nir_size=None, matrix=None):
    """Maior retângulo da imagem RGB totalmente coberto pelo NIR alinhado.

    Projeta um quadro NIR "todo válido" com a mesma transformação usada no
    alinhamento e encolhe a caixa envolvente até não restar pixel inválido
    nas bordas. Depende só da matriz e das resoluções, então é cacheado.
    """
    matrix = matrix_for(width, height) if matrix is None else np.float32(matrix)
    key = (width, height, nir_size, matrix.tobytes())
    if key in _crop_cache:
        return _crop_cache[key]

    factor = 8  # resolução reduzida: suficiente e rápido
    w, h = max(1, width // factor), max(1, height // factor)
    matrix = matrix.copy()
    matrix[:, 2] /= factor
    # O NIR é redimensionado para o tamanho da RGB antes do warp, então um
    # quadro todo válido no tamanho da RGB representa o NIR inteiro.
    valid = cv2.warpAffine(
        np.full((h, w), 255, np.uint8), matrix, (w, h),
        flags=cv2.INTER_NEAREST | cv2.WARP_INVERSE_MAP, borderValue=0,
    ) > 0
    rows, cols = np.where(valid)
    if rows.size == 0:
        raise RuntimeError("Matriz de alinhamento não deixa região válida")
    y1, y2, x1, x2 = rows.min(), rows.max() + 1, cols.min(), cols.max() + 1

    while True:
        sides = {
            "top": (~valid[y1, x1:x2]).sum(),
            "bottom": (~valid[y2 - 1, x1:x2]).sum(),
            "left": (~valid[y1:y2, x1]).sum(),
            "right": (~valid[y1:y2, x2 - 1]).sum(),
        }
        worst = max(sides, key=sides.get)
        if sides[worst] == 0:
            break
        if worst == "top": y1 += 1
        elif worst == "bottom": y2 -= 1
        elif worst == "left": x1 += 1
        else: x2 -= 1
        if x2 - x1 < 4 or y2 - y1 < 4:
            raise RuntimeError("Região válida do alinhamento pequena demais")

    m = config.CROP_MARGIN_PX
    box = (
        min(width, int(x1) * factor + m), min(height, int(y1) * factor + m),
        max(0, int(x2) * factor - m), max(0, int(y2) * factor - m),
    )
    _crop_cache[key] = box
    return box


def compute_ndvi(rgb, nir_aligned):
    """Retorna NDVI float32 em [-1, 1] e a máscara de pixels confiáveis."""
    red = rgb[:, :, 2].astype(np.float32)
    nir = cv2.cvtColor(nir_aligned, cv2.COLOR_BGR2GRAY).astype(np.float32)
    total = nir + red
    ndvi = np.divide(nir - red, total, out=np.zeros_like(red), where=total > 0)
    np.clip(ndvi, -1.0, 1.0, out=ndvi)
    valid = total >= config.NDVI_MIN_SIGNAL
    return ndvi, valid


def quantize(ndvi):
    return np.round((ndvi + 1.0) * 127.5).astype(np.uint8)


# ============================================================
# Escala de cores do NDVI (divergente: terra <- neutro -> verde)
# ============================================================
# Só exibição: os valores calculados não mudam. O pseudo-NDVI (NoIR sem filtro
# passa-IR) é comprimido para perto de zero (vegetação saudável ~0,4), então a
# escala cobre -0,2..+0,6 com o neutro em +0,2; fora disso, satura nas pontas.

NDVI_DISPLAY_MIN, NDVI_DISPLAY_MAX = -0.2, 0.6

_NDVI_STOPS = [
    (-0.20, (92, 38, 18)),
    (-0.04, (166, 78, 38)),
    (0.08, (214, 139, 92)),
    (0.20, (232, 228, 218)),
    (0.30, (150, 205, 176)),
    (0.40, (60, 160, 120)),
    (0.50, (22, 112, 86)),
    (0.60, (8, 70, 56)),
]


def _ndvi_lut():
    lut = np.zeros((256, 3), dtype=np.uint8)
    xs = [s[0] for s in _NDVI_STOPS]
    for q in range(256):
        value = (q - 127) / 127 if q <= 127 else (q - 127) / 128
        rgb = [np.interp(value, xs, [s[1][c] for s in _NDVI_STOPS]) for c in range(3)]
        lut[q] = [round(rgb[2]), round(rgb[1]), round(rgb[0])]  # BGR
    return lut


NDVI_LUT_BGR = _ndvi_lut()


def colorize(ndvi_u8):
    return NDVI_LUT_BGR[ndvi_u8]


# ============================================================
# Georreferenciamento (projeção do raio de cada pixel no solo)
# ============================================================

def _rotation_body_to_ned(roll, pitch, yaw):
    cr, sr = math.cos(roll), math.sin(roll)
    cp, sp = math.cos(pitch), math.sin(pitch)
    cy, sy = math.cos(yaw), math.sin(yaw)
    return np.array([
        [cy * cp, cy * sp * sr - sy * cr, cy * sp * cr + sy * sr],
        [sy * cp, sy * sp * sr + cy * cr, sy * sp * cr - cy * sr],
        [-sp, cp * sr, cp * cr],
    ])


def _rotation_from_quaternion(q):
    w, x, y, z = (q[k] for k in ("w", "x", "y", "z"))
    norm = math.sqrt(w * w + x * x + y * y + z * z)
    if norm < 1e-9:
        return None
    w, x, y, z = w / norm, x / norm, y / norm, z / norm
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
        [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
        [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)],
    ])


class Georeference:
    """Projeta pixels da imagem RGB completa no solo, no referencial local
    da missão (metros ao norte/leste do ponto de partida, que é a estação).

    A orientação do metadata.json é a do referencial da câmera (x = topo da
    imagem, y = direita, z = eixo óptico), então qualquer montagem e
    câmeras fora do nadir são tratadas. Solo plano na altura do ponto de
    partida. A conversão para lat/lon é
    feita no dashboard, a partir da posição configurada da estação.
    """

    def __init__(self, metadata, width, height):
        self.ok = False
        position = metadata.get("position") or {}
        orientation = metadata.get("orientation") or {}
        self.north = position.get("north_m")
        self.east = position.get("east_m")
        self.height = position.get("height_m")

        if None in (self.north, self.east, self.height) or self.height <= 1:
            return

        quaternion = orientation.get("quaternion") or {}
        rotation = None
        if all(isinstance(quaternion.get(k), (int, float)) for k in ("w", "x", "y", "z")):
            rotation = _rotation_from_quaternion(quaternion)
        if rotation is None:
            if orientation.get("yaw_deg") is None:
                return
            rotation = _rotation_body_to_ned(
                math.radians(orientation.get("roll_deg") or 0.0),
                math.radians(orientation.get("pitch_deg") or 0.0),
                math.radians(orientation["yaw_deg"]),
            )
        offset = math.radians(config.CAMERA_YAW_OFFSET_DEG)
        if offset:
            rotation = rotation @ _rotation_body_to_ned(0.0, 0.0, offset)

        self.rotation = rotation
        self.cx, self.cy = width / 2, height / 2
        self.focal = (width / 2) / math.tan(math.radians(config.CAMERA_HFOV_DEG) / 2)
        self.gsd_m = self.height / self.focal  # metros por pixel no nadir
        self.ok = True

    def pixel_to_local(self, u, v):
        """(north_m, east_m) do ponto do solo visto no pixel (u, v)."""
        # Câmera -> corpo (frente, direita, baixo)
        body = np.array([-(v - self.cy) / self.focal, (u - self.cx) / self.focal, 1.0])
        north, east, down = self.rotation @ body
        if down <= 0.05:
            return None
        scale = self.height / down
        return round(self.north + north * scale, 3), round(self.east + east * scale, 3)


# ============================================================
# Detecção de focos de NDVI baixo
#
# Procura regiões PEQUENAS onde o NDVI cai bruscamente em relação à
# vegetação ao redor. Regiões grandes (água, prédios, solo exposto),
# alongadas (estradas, trilhas) ou que não estejam cercadas por
# vegetação são descartadas.
# ============================================================

def _otsu_threshold(values):
    quantized = np.clip((values + 1.0) * 127.5, 0, 255).astype(np.uint8)
    threshold, _ = cv2.threshold(quantized.reshape(-1, 1), 0, 255, cv2.THRESH_BINARY + cv2.THRESH_OTSU)
    return threshold / 127.5 - 1.0


def detect_hotspots(ndvi, valid, gsd_full_m=None):
    height, width = ndvi.shape
    scale = min(1.0, config.HOTSPOT_WORK_WIDTH / width)
    work_w, work_h = round(width * scale), round(height * scale)

    small = cv2.resize(ndvi, (work_w, work_h), interpolation=cv2.INTER_AREA)
    small_valid = cv2.resize(valid.astype(np.uint8), (work_w, work_h), interpolation=cv2.INTER_NEAREST) > 0
    small = cv2.GaussianBlur(small, (3, 3), 0)

    summary = {"vegetation_threshold": None, "drop_threshold": None, "candidates": 0, "bimodal": None}
    if small_valid.sum() < 100:
        return [], summary

    # Vegetação: limiar adaptativo. Otsu só vale se a cena for bimodal
    # (vegetação + solo/água); numa cena quase toda vegetada ele cortaria a
    # própria vegetação ao meio, então o limiar fica bem abaixo da mediana.
    values = small[small_valid]
    otsu = _otsu_threshold(values)
    low, high = values[values <= otsu], values[values > otsu]
    bimodal = False
    if min(low.size, high.size) > 0.03 * values.size:
        pooled = math.sqrt((low.var() * low.size + high.var() * high.size) / values.size)
        bimodal = (high.mean() - low.mean()) > 3.5 * max(pooled, 1e-3)
    if bimodal:
        vegetation_threshold = otsu
    else:
        median = float(np.median(values))
        spread = 1.4826 * float(np.median(np.abs(values - median)))
        vegetation_threshold = median - max(0.15, 3 * spread)
    vegetation_threshold = float(np.clip(vegetation_threshold, -0.3, 0.6))
    summary["bimodal"] = bool(bimodal)
    vegetation = small_valid & (small > vegetation_threshold)
    summary["vegetation_threshold"] = round(vegetation_threshold, 4)
    if vegetation.sum() < 100:
        return [], summary

    # Fundo local: média do NDVI só da vegetação numa janela bem maior que
    # o maior foco aceito (senão o centro de um foco não tem vegetação ao
    # redor e só a borda vira candidata).
    if gsd_full_m:
        max_diameter_px = 2 * math.sqrt(config.HOTSPOT_MAX_AREA_M2 / math.pi) / (gsd_full_m / scale)
        window = int(np.clip(3 * max_diameter_px, 31, work_w // 3)) | 1
    else:
        window = max(31, (work_w // 8) | 1)
    veg_f = vegetation.astype(np.float32)
    density = cv2.blur(veg_f, (window, window))
    background = cv2.blur(small * veg_f, (window, window)) / np.maximum(density, 1e-6)
    drop = background - small

    veg_values = small[vegetation]
    sigma = 1.4826 * float(np.median(np.abs(veg_values - np.median(veg_values))))
    drop_threshold = max(config.HOTSPOT_MIN_DROP, config.HOTSPOT_SIGMA_FACTOR * sigma)
    summary["drop_threshold"] = round(drop_threshold, 4)

    candidates = small_valid & (drop > drop_threshold) & (density > 0.5)
    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (3, 3))
    candidates = cv2.morphologyEx(candidates.astype(np.uint8), cv2.MORPH_OPEN, kernel)
    candidates = cv2.morphologyEx(candidates, cv2.MORPH_CLOSE, kernel)
    # Preenche buracos internos dos candidatos.
    contours, _ = cv2.findContours(candidates, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    candidates = np.zeros_like(candidates)
    cv2.drawContours(candidates, contours, -1, 1, thickness=cv2.FILLED)

    count, labels, stats, centroids = cv2.connectedComponentsWithStats(candidates, connectivity=8)
    summary["candidates"] = int(count - 1)

    # Limites de área em pixels de trabalho.
    if gsd_full_m:
        pixel_area_m2 = (gsd_full_m / scale) ** 2
        min_area = max(4, config.HOTSPOT_MIN_AREA_M2 / pixel_area_m2)
        max_area = config.HOTSPOT_MAX_AREA_M2 / pixel_area_m2
    else:
        pixel_area_m2 = None
        min_area = config.HOTSPOT_MIN_AREA_PX
        max_area = float("inf")
    max_area = min(max_area, config.HOTSPOT_MAX_IMAGE_FRACTION * work_w * work_h)

    hotspots = []
    for label in range(1, count):
        x, y, w, h, area = (int(v) for v in stats[label])
        if area < min_area or area > max_area:
            continue
        if x <= 1 or y <= 1 or x + w >= work_w - 1 or y + h >= work_h - 1:
            continue  # encosta na borda: não dá para avaliar o entorno

        pad = max(4, int(math.sqrt(area) * 0.8))
        x0, y0 = max(0, x - pad), max(0, y - pad)
        x1, y1 = min(work_w, x + w + pad), min(work_h, y + h + pad)
        component = (labels[y0:y1, x0:x1] == label).astype(np.uint8)

        contours, _ = cv2.findContours(component, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        contour = max(contours, key=cv2.contourArea)
        (_, _), (rw, rh), _ = cv2.minAreaRect(contour)
        elongation = max(rw, rh) / max(1.0, min(rw, rh))
        fill = area / max(1.0, rw * rh)
        if elongation > config.HOTSPOT_MAX_ELONGATION or fill < config.HOTSPOT_MIN_FILL:
            continue

        ring_size = 2 * pad + 1
        ring_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (ring_size, ring_size))
        ring = cv2.dilate(component, ring_kernel).astype(bool)
        ring &= ~cv2.dilate(component, kernel).astype(bool)
        ring_valid = small_valid[y0:y1, x0:x1][ring]
        ring_vegetation = vegetation[y0:y1, x0:x1][ring]
        if ring.sum() == 0 or ring_valid.mean() < 0.9:
            continue
        ring_fraction = float(ring_vegetation.mean())
        if ring_fraction < config.HOTSPOT_MIN_RING_VEGETATION:
            continue

        inside = small[y0:y1, x0:x1][component.astype(bool)]
        surrounding = small[y0:y1, x0:x1][ring & vegetation[y0:y1, x0:x1]]
        ndvi_mean = float(inside.mean())
        ring_mean = float(surrounding.mean())
        ndvi_drop = ring_mean - ndvi_mean
        if ndvi_drop < 0.75 * drop_threshold:
            continue  # resto de borda: a queda real frente ao entorno é pequena

        polygon = (contour.reshape(-1, 2) + [x0, y0]).astype(np.float64)
        epsilon = max(0.5, 0.02 * cv2.arcLength(contour, True))
        polygon = cv2.approxPolyDP(polygon.astype(np.float32), epsilon, True).reshape(-1, 2)

        cx, cy = (float(v) for v in centroids[label])
        hotspots.append({
            "centroid_px": [round(cx / scale, 1), round(cy / scale, 1)],
            "bbox_px": [round(x / scale), round(y / scale), round(w / scale), round(h / scale)],
            "polygon": [[round(float(px) / work_w, 5), round(float(py) / work_h, 5)] for px, py in polygon],
            "area_px": round(area / scale ** 2),
            "area_m2": round(area * pixel_area_m2, 3) if pixel_area_m2 else None,
            "ndvi_mean": round(ndvi_mean, 4),
            "surrounding_ndvi_mean": round(ring_mean, 4),
            "ndvi_drop": round(ndvi_drop, 4),
            "surrounding_vegetation": round(ring_fraction, 3),
            "elongation": round(elongation, 2),
        })

    hotspots.sort(key=lambda h: h["ndvi_drop"] * math.sqrt(h["area_px"]), reverse=True)
    hotspots = hotspots[: config.HOTSPOT_MAX_COUNT]

    for index, hotspot in enumerate(hotspots, start=1):
        hotspot["id"] = index
        relative = hotspot["ndvi_drop"] / drop_threshold
        hotspot["severity"] = "alta" if relative >= 3 else "media" if relative >= 1.8 else "baixa"

    return hotspots, summary


# ============================================================
# Estatísticas
# ============================================================

def ndvi_statistics(ndvi, valid, vegetation_threshold):
    values = ndvi[valid]
    if values.size == 0:
        return None
    sample = values[:: max(1, values.size // 2_000_000)]
    histogram, edges = np.histogram(sample, bins=40, range=(-1.0, 1.0))
    p10, p25, p50, p75, p90 = np.percentile(sample, [10, 25, 50, 75, 90])
    threshold = vegetation_threshold if vegetation_threshold is not None else 0.2
    return {
        "mean": round(float(sample.mean()), 4),
        "std": round(float(sample.std()), 4),
        "p10": round(float(p10), 4),
        "p25": round(float(p25), 4),
        "median": round(float(p50), 4),
        "p75": round(float(p75), 4),
        "p90": round(float(p90), 4),
        "vegetation_fraction": round(float((sample > threshold).mean()), 4),
        "valid_fraction": round(float(valid.mean()), 4),
        "histogram": {
            "edges": [round(float(e), 3) for e in edges],
            "counts": [int(c) for c in histogram],
        },
    }


# ============================================================
# Pipeline completo de uma entrada
# ============================================================

def process_entry(entry_dir: Path) -> dict:
    entry_dir = Path(entry_dir)
    metadata = json.loads((entry_dir / "metadata.json").read_text(encoding="utf-8"))

    rgb = cv2.imread(str(entry_dir / "rgb.jpg"), cv2.IMREAD_COLOR)
    nir = cv2.imread(str(entry_dir / "nir.jpg"), cv2.IMREAD_COLOR)
    if rgb is None:
        raise RuntimeError("rgb.jpg ilegível")
    if nir is None:
        raise RuntimeError("nir.jpg ilegível")

    full_h, full_w = rgb.shape[:2]
    nir = cv2.resize(nir, (full_w, full_h), interpolation=cv2.INTER_LINEAR)
    base, base_name = base_matrix(metadata.get("mission_id"))
    matrix, alignment = refine_alignment(rgb, nir, matrix_for(full_w, full_h, base))
    alignment.update({"base": base_name, "matrix": np.round(matrix, 6).tolist()})
    nir_aligned = align_nir(nir, full_w, full_h, matrix)
    del nir

    x1, y1, x2, y2 = crop_box(full_w, full_h, matrix=matrix)
    rgb_crop = rgb[y1:y2, x1:x2]
    ndvi, valid = compute_ndvi(rgb_crop, nir_aligned[y1:y2, x1:x2])
    del nir_aligned

    ndvi_u8 = quantize(ndvi)
    buffer_path = entry_dir / ".ndvi.npy.tmp"
    with buffer_path.open("wb") as file:
        np.save(file, ndvi_u8)
    os.replace(buffer_path, entry_dir / "ndvi.npy")

    geo = Georeference(metadata, full_w, full_h)
    quality = assess_quality(metadata, rgb_crop, alignment["refined"])
    if quality["survey"]:
        hotspots, detection = detect_hotspots(ndvi, valid, geo.gsd_m if geo.ok else None)
    else:
        # Fora do levantamento (chão, decolagem/pouso, RGB estourada): sem focos.
        hotspots, detection = [], {"vegetation_threshold": None, "drop_threshold": None, "candidates": 0, "bimodal": None, "skipped": True}

    warnings = []
    footprint = None
    if geo.ok:
        corners = [(x1, y1), (x2, y1), (x2, y2), (x1, y2)]
        projected = [geo.pixel_to_local(u, v) for u, v in corners]
        if all(projected):
            footprint = [[north, east] for north, east in projected]
        else:
            warnings.append("Inclinação alta: recorte não intercepta o solo por completo.")
        for hotspot in hotspots:
            cx, cy = hotspot["centroid_px"]
            location = geo.pixel_to_local(cx + x1, cy + y1)
            hotspot["north_m"], hotspot["east_m"] = location if location else (None, None)
    else:
        warnings.append("Sem posição, altura ou orientação: imagem não georreferenciada.")
        for hotspot in hotspots:
            hotspot["north_m"] = hotspot["east_m"] = None

    # Pré-visualizações
    preview = entry_dir / "preview"
    width = min(config.PREVIEW_WIDTH, rgb_crop.shape[1])
    _write_atomic(preview / "rgb.jpg", _encode(".jpg", _resize_to_width(rgb_crop, width), [cv2.IMWRITE_JPEG_QUALITY, 86]))
    colored = colorize(ndvi_u8)
    colored[~valid] = (40, 40, 40)
    _write_atomic(preview / "ndvi.png", _encode(".png", _resize_to_width(colored, width), [cv2.IMWRITE_PNG_COMPRESSION, 6]))
    _write_atomic(preview / "thumb.jpg", _encode(".jpg", _resize_to_width(rgb_crop, config.THUMB_WIDTH), [cv2.IMWRITE_JPEG_QUALITY, 80]))

    analysis = {
        "version": ANALYSIS_VERSION,
        "processed_at": datetime.now(timezone.utc).isoformat(),
        "image": {"width": full_w, "height": full_h},
        "crop": {"x1": x1, "y1": y1, "x2": x2, "y2": y2},
        "ndvi": ndvi_statistics(ndvi, valid, detection["vegetation_threshold"]),
        "detection": detection,
        "hotspots": hotspots,
        "geo": {
            "georeferenced": geo.ok and footprint is not None,
            "gsd_cm": round(geo.gsd_m * 100, 2) if geo.ok else None,
            "frame": "mission_start_ned",
            "footprint_ne_m": footprint,
            "area_m2": _polygon_area_m2(footprint) if footprint else None,
            "camera_hfov_deg": config.CAMERA_HFOV_DEG,
        },
        "warnings": warnings,
        "quality": quality,
        "alignment": alignment,
    }
    _write_json(entry_dir / "analysis.json", analysis)
    (entry_dir / "error.json").unlink(missing_ok=True)
    return analysis


def assess_quality(metadata, rgb_crop, refined=False):
    """Classifica a foto: entra no levantamento (focos, médias, mapa) ou não.

    survey = False quando a foto não representa o levantamento aéreo:
    abaixo da altura mínima (chão, decolagem, pouso) ou RGB estourada.
    alignment_uncertain = True quando o módulo girava rápido na captura
    (as duas câmeras não registram a cena no mesmo instante).
    """
    reasons = []
    height = (metadata.get("position") or {}).get("height_m")
    if height is None or height < config.MIN_SURVEY_HEIGHT_M:
        reasons.append("abaixo da altura mínima de levantamento")
    gray = cv2.cvtColor(cv2.resize(rgb_crop, None, fx=0.25, fy=0.25, interpolation=cv2.INTER_AREA), cv2.COLOR_BGR2GRAY)
    saturated = float((gray >= 250).mean())
    if saturated > config.MAX_RGB_SATURATED:
        reasons.append("RGB estourada")
    angular = (metadata.get("stability") or {}).get("angular_speed_rad_s")
    return {
        "survey": not reasons,
        "reasons": reasons,
        # em movimento e sem correção por foto aceita
        "alignment_uncertain": angular is not None and angular > config.MAX_ANGULAR_SPEED and not refined,
        "rgb_saturated": round(saturated, 4),
        "angular_speed_rad_s": angular,
        "height_m": height,
    }


def _polygon_area_m2(points):
    area = 0.0
    for (na, ea), (nb, eb) in zip(points, points[1:] + points[:1]):
        area += ea * nb - eb * na
    return round(abs(area) / 2, 1)


def process_safely(entry_dir: Path) -> bool:
    try:
        process_entry(entry_dir)
        return True
    except Exception as error:
        traceback.print_exc()
        _write_json(Path(entry_dir) / "error.json", {
            "error": str(error),
            "at": datetime.now(timezone.utc).isoformat(),
        })
        return False


def needs_processing(entry_dir: Path) -> bool:
    entry_dir = Path(entry_dir)
    if not (entry_dir / "nir.jpg").is_file():
        return False  # pasta antiga (NDVI calculado no módulo): ignorada
    if (entry_dir / "error.json").is_file():
        return False
    try:
        analysis = json.loads((entry_dir / "analysis.json").read_text(encoding="utf-8"))
        return analysis.get("version") != ANALYSIS_VERSION
    except (OSError, ValueError):
        return True
