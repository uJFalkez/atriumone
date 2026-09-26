import cv2
import threading
import time

from picamera2 import Picamera2

from config import (
    USB_DEVICE,
    USB_SIZE,
    CSI_SIZE,
    USB_WARMUP_FRAMES,
    CSI_WARMUP_SECONDS,
    USB_FLUSH_FRAMES,
)


class CameraManager:

    def __init__(self):
        self.rgb_camera = None
        self.nir_camera = None

    def start(self):
        print("Inicializando câmeras...")

        self.rgb_camera = cv2.VideoCapture(
            USB_DEVICE,
            cv2.CAP_V4L2,
        )

        self.rgb_camera.set(
            cv2.CAP_PROP_FOURCC,
            cv2.VideoWriter_fourcc(*"MJPG"),
        )

        self.rgb_camera.set(
            cv2.CAP_PROP_FRAME_WIDTH,
            USB_SIZE[0],
        )

        self.rgb_camera.set(
            cv2.CAP_PROP_FRAME_HEIGHT,
            USB_SIZE[1],
        )

        self.rgb_camera.set(
            cv2.CAP_PROP_BUFFERSIZE,
            1,
        )

        if not self.rgb_camera.isOpened():
            raise RuntimeError(
                "Não foi possível abrir câmera RGB"
            )

        self.nir_camera = Picamera2()

        config = self.nir_camera.create_still_configuration(
            main={
                "size": CSI_SIZE,
                "format": "RGB888",
            },
            buffer_count=2,
        )

        self.nir_camera.configure(config)
        self.nir_camera.start()

        self._warmup()

        print("Câmeras prontas.")

    def _warmup(self):
        time.sleep(CSI_WARMUP_SECONDS)

        for _ in range(USB_WARMUP_FRAMES):
            self.rgb_camera.grab()

    def capture(self):
        rgb_frame = None
        nir_frame = None
        errors = []

        def capture_rgb():
            nonlocal rgb_frame

            try:
                # Descarta frames antigos que ficaram no buffer
                for _ in range(3):
                    if not self.rgb_camera.grab():
                        raise RuntimeError(
                            "Falha ao atualizar câmera RGB"
                        )

                success, frame = self.rgb_camera.read()

                if not success:
                    raise RuntimeError(
                        "Falha ao capturar RGB"
                    )

                rgb_frame = frame

            except Exception as error:
                errors.append(error)

        def capture_nir():
            nonlocal nir_frame

            try:
                nir_frame = self.nir_camera.capture_array()

            except Exception as error:
                errors.append(error)

        rgb_thread = threading.Thread(
            target=capture_rgb
        )

        nir_thread = threading.Thread(
            target=capture_nir
        )

        rgb_thread.start()
        nir_thread.start()

        rgb_thread.join()
        nir_thread.join()

        if errors:
            raise errors[0]

        if rgb_frame is None:
            raise RuntimeError(
                "Câmera RGB não retornou frame"
            )

        if nir_frame is None:
            raise RuntimeError(
                "Câmera NIR não retornou frame"
            )

        return rgb_frame, nir_frame

    def stop(self):
        if self.rgb_camera is not None:
            self.rgb_camera.release()

        if self.nir_camera is not None:
            self.nir_camera.stop()