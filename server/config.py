import os
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent.parent

ENTRIES_DIR = Path(os.environ.get("ENTRIES_DIR", PROJECT_ROOT / "entries")).resolve()
MISSIONS_DIR = Path(os.environ.get("MISSIONS_DIR", PROJECT_ROOT / "missions")).resolve()
CALIBRATION_DIR = Path(__file__).resolve().parent / "calibration"
# Matriz padrão (foco 144 da RGB) e matrizes por posição de foco da Arducam:
# mudar o foco move a lente e muda a relação entre as câmeras.
ALIGNMENT_PATH = Path(os.environ.get("ALIGNMENT_PATH", CALIBRATION_DIR / "alignment.npy"))
ALIGNMENT_BY_FOCUS = {
    144: CALIBRATION_DIR / "alignment.npy",        # missão 10 (719bc165), afim completa com alvos
    57: CALIBRATION_DIR / "alignment_focus57.npy", # missão 09 (6b93dceb), escala+translação sobre a de 144
}

# Correção por foto (ECC) sobre a calibração, com limites de plausibilidade.
ALIGNMENT_REFINE = True
REFINE_SCALE = 0.5            # resolução de trabalho do ECC
REFINE_MIN_CC = 0.6           # correlação mínima para aceitar
REFINE_MAX_SCALE = 0.07       # variação máxima de escala em relação à calibração
REFINE_MAX_ROT_DEG = 1.5      # rotação máxima em relação à calibração
REFINE_MAX_SHIFT_PX = 90      # deslocamento máximo nos cantos (px em 1600 de largura)

# ============================================================
# Recorte da região válida após o alinhamento
# Calculado automaticamente a partir de alignment.npy: maior retângulo da
# RGB coberto pelo NIR alinhado, menos uma margem de segurança.
# ============================================================

CROP_MARGIN_PX = 8

# Resolução da RGB usada para calcular alignment.npy (tools/align_targets.py,
# com alvos no chão, missão 719bc165 de 06/11/2026).
# Fotos em outra resolução (mesmo campo de visão, reescaladas) têm a matriz
# convertida automaticamente. Atualize se recalibrar em outra resolução.
ALIGNMENT_RGB_SIZE = (1600, 1200)

# Pixels com NIR + R abaixo disso são escuros demais para um NDVI confiável.
NDVI_MIN_SIGNAL = 20

# ============================================================
# Câmera RGB (georreferenciamento)
#
# A orientação no metadata.json já é a do referencial da câmera
# (x = topo da imagem, y = direita, z = eixo óptico); por isso o offset de
# rumo é 0 e só serve para corrigir uma montagem fora do padrão.
#
# HFOV da Arducam 8MP USB estimado pela matriz de alinhamento: a escala RGB→NIR
# (0,961 / 0,981 com a NIR redimensionada para 1600x1200) e o FOV conhecido da
# Pi Camera V2 (62,2° × 48,8°) dão f_RGB ≈ 1380 / 1347 px em 1600 px de
# largura, ou seja, HFOV ≈ 60,8°. Substitua se medir a lente.
# ============================================================

CAMERA_HFOV_DEG = float(os.environ.get("CAMERA_HFOV_DEG", 60.8))
CAMERA_YAW_OFFSET_DEG = float(os.environ.get("CAMERA_YAW_OFFSET_DEG", 0.0))

# ============================================================
# Detecção de focos de NDVI baixo
# ============================================================

HOTSPOT_WORK_WIDTH = 1000          # resolução de trabalho (px de largura)
HOTSPOT_MIN_DROP = 0.08            # queda mínima de NDVI frente ao entorno
HOTSPOT_SIGMA_FACTOR = 2.0         # queda mínima também >= k * dispersão da vegetação
HOTSPOT_MIN_AREA_M2 = 0.3
HOTSPOT_MAX_AREA_M2 = 20.0        # "pontos pequenos": ~5 m de diâmetro
HOTSPOT_MIN_AREA_PX = 25           # sem altitude: limites em pixels de trabalho
HOTSPOT_MAX_IMAGE_FRACTION = 0.015
HOTSPOT_MIN_RING_VEGETATION = 0.65 # fração de vegetação exigida ao redor do foco
HOTSPOT_MAX_ELONGATION = 5.0       # descarta trilhas e estradas
HOTSPOT_MIN_FILL = 0.25
HOTSPOT_MAX_COUNT = 60

# ============================================================
# Qualidade: quais fotos entram no levantamento
# ============================================================

MIN_SURVEY_HEIGHT_M = 5.0   # abaixo disso: chão, decolagem ou pouso
MAX_RGB_SATURATED = 0.05    # fração de pixels >= 250 na RGB
MAX_ANGULAR_SPEED = 0.15    # rad/s; acima disso o alinhamento RGB/NIR fica incerto

# ============================================================
# Pré-visualizações para o dashboard
# ============================================================

PREVIEW_WIDTH = 1600
THUMB_WIDTH = 480
