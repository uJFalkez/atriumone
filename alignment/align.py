import cv2
import numpy as np
import sys


if len(sys.argv) != 4:
    print(
        "Uso: python calibrate_alignment.py "
        "<rgb.jpg> <nir.jpg> <alignment.npy>"
    )
    sys.exit(1)


RGB_PATH = sys.argv[1]
NIR_PATH = sys.argv[2]
OUTPUT_PATH = sys.argv[3]

# Faz a calibração em resolução menor.
# 0.25 transforma ~8 MP em ~0.5 MP.
SCALE = 0.25


# ============================================================
# Carregar imagens
# ============================================================

rgb = cv2.imread(RGB_PATH, cv2.IMREAD_COLOR)
nir = cv2.imread(NIR_PATH, cv2.IMREAD_COLOR)

if rgb is None:
    raise RuntimeError(f"Não consegui abrir {RGB_PATH}")

if nir is None:
    raise RuntimeError(f"Não consegui abrir {NIR_PATH}")


print("RGB:", rgb.shape)
print("NIR:", nir.shape)


# ============================================================
# Primeiro igualar o tamanho do NIR ao RGB
#
# Essa operação também fará parte da pipeline do Raspberry.
# Portanto a transformação calculada aqui assume que o NIR
# já foi redimensionado para as dimensões do RGB.
# ============================================================

height, width = rgb.shape[:2]

nir = cv2.resize(
    nir,
    (width, height),
    interpolation=cv2.INTER_LINEAR
)


# ============================================================
# Criar imagens auxiliares para alinhamento
#
# OpenCV usa BGR.
#
# Não estamos usando RED porque posteriormente queremos usar
# essa banda para NDVI.
# ============================================================

def alignment_band(image):
    blue = image[:, :, 0].astype(np.float32)
    green = image[:, :, 1].astype(np.float32)

    band = (blue + green) * 0.5

    cv2.normalize(
        band,
        band,
        0.0,
        1.0,
        cv2.NORM_MINMAX
    )

    return band


rgb_align = alignment_band(rgb)
nir_align = alignment_band(nir)


# ============================================================
# Reduzir resolução para calcular transformação
# ============================================================

small_width = int(width * SCALE)
small_height = int(height * SCALE)

rgb_small = cv2.resize(
    rgb_align,
    (small_width, small_height),
    interpolation=cv2.INTER_AREA
)

nir_small = cv2.resize(
    nir_align,
    (small_width, small_height),
    interpolation=cv2.INTER_AREA
)


# ============================================================
# ECC
#
# Transformação:
#
# NIR -> RGB
# ============================================================

warp_matrix = np.eye(
    2,
    3,
    dtype=np.float32
)

criteria = (
    cv2.TERM_CRITERIA_EPS |
    cv2.TERM_CRITERIA_COUNT,
    500,
    1e-7
)

print()
print("Calculando alinhamento...")

correlation, warp_matrix = cv2.findTransformECC(
    rgb_small,
    nir_small,
    warp_matrix,
    cv2.MOTION_AFFINE,
    criteria,
    None,
    5
)


# ============================================================
# Corrigir escala
#
# A transformação foi calculada em resolução SCALE.
#
# Os coeficientes lineares permanecem iguais.
# As translações precisam voltar para coordenadas full-res.
# ============================================================

warp_matrix[0, 2] /= SCALE
warp_matrix[1, 2] /= SCALE


# ============================================================
# Salvar
# ============================================================

np.save(
    OUTPUT_PATH,
    warp_matrix
)


print()
print(f"Correlação ECC: {correlation:.6f}")
print()
print("Matriz NIR -> RGB:")
print(warp_matrix)
print()
print(f"Salvo em: {OUTPUT_PATH}")


# ============================================================
# Gerar preview para conferir visualmente
#
# Isso NÃO vai para o Raspberry.
# ============================================================

nir_aligned = cv2.warpAffine(
    nir,
    warp_matrix,
    (width, height),

    flags=(
        cv2.INTER_LINEAR |
        cv2.WARP_INVERSE_MAP
    ),

    borderMode=cv2.BORDER_CONSTANT,
    borderValue=0
)


# Mistura 50/50 para vermos desalinhamento
preview = cv2.addWeighted(
    rgb,
    0.5,
    nir_aligned,
    0.5,
    0
)

cv2.imwrite(
    "alignment_preview.jpg",
    preview
)

cv2.imwrite(
    "nir_aligned.jpg",
    nir_aligned
)

print("Preview: alignment_preview.jpg")
print("NIR alinhado: nir_aligned.jpg")