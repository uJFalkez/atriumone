import gc
import json
import shutil

import cv2
import numpy as np

from datetime import datetime, timezone
from uuid import uuid4

from config import (
    PROCESSING_DIR,
    PENDING_DIR,
)

from processing import (
    align_nir,
    calculate_ndvi,
    crop,
)


def capture(camera):
    capture_id = str(uuid4())
    timestamp = datetime.now(timezone.utc).isoformat()

    processing_dir = PROCESSING_DIR / capture_id
    pending_dir = PENDING_DIR / capture_id

    processing_dir.mkdir(
        parents=True,
        exist_ok=False,
    )

    rgb_path = processing_dir / "rgb.jpg"
    ndvi_path = processing_dir / "ndvi.npy"
    metadata_path = processing_dir / "metadata.json"

    try:
        # ====================================================
        # Capture
        # ====================================================

        print(f"Capturando {capture_id}...")

        rgb_frame, nir_frame = camera.capture()

        height, width = rgb_frame.shape[:2]

        # ====================================================
        # Alignment
        # ====================================================

        print("Alinhando NIR...")

        aligned_nir = align_nir(
            nir_frame,
            width,
            height,
        )

        del nir_frame
        gc.collect()

        # ====================================================
        # NDVI
        # ====================================================

        print("Calculando NDVI...")

        ndvi = calculate_ndvi(
            rgb_frame,
            aligned_nir,
        )

        del aligned_nir
        gc.collect()

        # ====================================================
        # Crop
        # ====================================================

        print("Recortando...")

        rgb_cropped = crop(rgb_frame)
        ndvi_cropped = crop(ndvi)

        del rgb_frame
        del ndvi
        gc.collect()

        # ====================================================
        # Save
        # ====================================================

        print("Salvando...")

        if not cv2.imwrite(
            str(rgb_path),
            rgb_cropped,
        ):
            raise RuntimeError(
                "Falha ao salvar RGB"
            )

        np.save(
            ndvi_path,
            ndvi_cropped,
        )

        metadata = {
            "id": capture_id,
            "timestamp": timestamp,

            "image": {
                "width": int(rgb_cropped.shape[1]),
                "height": int(rgb_cropped.shape[0]),
            },

            "position": {
                "latitude": None,
                "longitude": None,
                "altitude_m": None,
            },

            "attitude": {
                "quaternion": {
                    "w": None,
                    "x": None,
                    "y": None,
                    "z": None,
                },
                "roll_deg": None,
                "pitch_deg": None,
                "yaw_deg": None,
                "heading_deg": None,
            },

            "stability": {
                "gyro_rad_s": {
                    "x": None,
                    "y": None,
                    "z": None,
                },
                "accel_m_s2": {
                    "x": None,
                    "y": None,
                    "z": None,
                },
                "angular_speed_rad_s": None,
            },
        }

        with metadata_path.open(
            "w",
            encoding="utf-8",
        ) as file:
            json.dump(
                metadata,
                file,
                indent=2,
            )

        # ====================================================
        # Publish
        # ====================================================

        processing_dir.rename(
            pending_dir
        )

        print(f"Captura pronta: {capture_id}")

        return capture_id

    except Exception:
        shutil.rmtree(
            processing_dir,
            ignore_errors=True,
        )

        raise