import gc
import cv2
import numpy as np

from config import (
    ALIGNMENT_PATH,
    NDVI_BLOCK_SIZE,
    CROP_X1,
    CROP_Y1,
    CROP_X2,
    CROP_Y2,
)


warp_matrix = np.load(
    ALIGNMENT_PATH
).astype(np.float32)

if warp_matrix.shape != (2, 3):
    raise RuntimeError(
        f"Matriz de alignment inválida: "
        f"{warp_matrix.shape}"
    )


def align_nir(nir_frame, width, height):
    if nir_frame.shape[:2] != (height, width):
        resized = cv2.resize(
            nir_frame,
            (width, height),
            interpolation=cv2.INTER_LINEAR,
        )

        del nir_frame
        nir_frame = resized

    aligned = cv2.warpAffine(
        nir_frame,
        warp_matrix,
        (width, height),
        flags=(
            cv2.INTER_LINEAR |
            cv2.WARP_INVERSE_MAP
        ),
        borderMode=cv2.BORDER_CONSTANT,
        borderValue=0,
    )

    return aligned


def calculate_ndvi(rgb_frame, nir_frame):
    height, width = rgb_frame.shape[:2]

    ndvi_u8 = np.empty(
        (height, width),
        dtype=np.uint8,
    )

    for y1 in range(
        0,
        height,
        NDVI_BLOCK_SIZE,
    ):
        y2 = min(
            y1 + NDVI_BLOCK_SIZE,
            height,
        )

        red = rgb_frame[
            y1:y2,
            :,
            2,
        ].astype(np.float32)

        nir = cv2.cvtColor(
            nir_frame[y1:y2],
            cv2.COLOR_BGR2GRAY,
        ).astype(np.float32)

        denominator = nir + red

        ndvi = np.divide(
            nir - red,
            denominator,
            out=np.zeros_like(red),
            where=denominator != 0,
        )

        np.clip(
            ndvi,
            -1.0,
            1.0,
            out=ndvi,
        )

        ndvi_u8[y1:y2] = np.round(
            (ndvi + 1.0) * 127.5
        ).astype(np.uint8)

        del red
        del nir
        del denominator
        del ndvi

    return ndvi_u8


def crop(array):
    return array[
        CROP_Y1:CROP_Y2,
        CROP_X1:CROP_X2,
    ].copy()