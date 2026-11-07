"""Configuration of the mission daemon. Every value can be overridden by an environment
variable (used by the tests to shorten the timings)."""
import os
from pathlib import Path


def _env(name, default, cast=str):
    v = os.environ.get(name)
    return default if v is None or v == "" else cast(v)


ROOT = Path(__file__).resolve().parent.parent
DATA = Path(_env("ATRIUM_DATA", str(ROOT / "data")))

MODULE_ID = _env("ATRIUM_MODULE_ID", "atrium-one-01")
VERSION = "0.1.0"

API_HOST = _env("ATRIUM_API_HOST", "0.0.0.0")
API_PORT = _env("ATRIUM_API_PORT", 8000, int)
STATION_URL = _env("ATRIUM_STATION_URL", "http://station:5000").rstrip("/")
NAVD_SOCK = _env("ATRIUM_NAVD_SOCK", "payload-navd")
CALIB_PATH = Path(_env("ATRIUM_CALIB_PATH", str(DATA / "calibration" / "calibration.json")))

# Mission timing (docs/PIPELINE.md §3.3, §3.4). Intervals use the monotonic clock.
WARMUP_S = _env("ATRIUM_WARMUP_S", 120.0, float)
MIN_CAPTURE_INTERVAL_S = _env("ATRIUM_MIN_CAPTURE_INTERVAL_S", 10.0, float)
CAPTURE_POLL_S = 0.1
# Field decision (2026-11-06): the drone never met the stability gate in flight, so photos are
# taken purely on the timer (every MIN_CAPTURE_INTERVAL_S). Set ATRIUM_REQUIRE_STABLE=1 to
# restore the gate. The angular speed is still recorded in metadata.json.
REQUIRE_STABLE = _env("ATRIUM_REQUIRE_STABLE", "0") == "1"
TRACK_PERIOD_S = 0.2               # 5 Hz
NAV_POLL_PERIOD_S = 0.2
NAV_TIMEOUT_S = 0.5

# Return detection (docs/PIPELINE.md §3.5)
RETURN_MAX_DISTANCE_M = 10.0
RETURN_MAX_HEIGHT_M = 2.0
RETURN_STOPPED_S = 3.0
RETURN_LANDED_S = 3.0
HEALTH_PERIOD_S = 2.0
# PIPELINE suggests "200 within the last ~5 s". Over the tailnet /health was measured at
# 0.6-5 s, so a 2 s timeout made ready_to_finish flicker. Accept slow answers.
HEALTH_TIMEOUT_S = 4.0
HEALTH_FRESH_S = 8.0

# Guided calibration (navd's session: still 30 s + figure8 90 s + poses 120 s at most)
CALIBRATION_MAX_S = 300.0

# Storage and upload
MIN_FREE_MB = _env("ATRIUM_MIN_FREE_MB", 300, int)
UPLOAD_BACKOFF_MAX_S = 30.0
UPLOAD_TIMEOUT_S = 60.0

# Cameras: "real" (WP3) or "fake" (copies data/fake/<name>.jpg).
CAMERA_MODE = _env("ATRIUM_CAMERA_MODE", "real")
WARMUP_FRAMES = 5                  # RGB: frames dropped after opening the camera (operator's finding)
CAMGRAB = ROOT / "build" / "camgrab"
RGB_DEVICE = _env("ATRIUM_RGB_DEVICE",
                  "/dev/v4l/by-id/usb-Arducam_Technology_Co.__Ltd._Arducam_8mp_SN0001-video-index0")
RGB_WB_K = 5500                    # fixed white balance (station's request); "auto" to disable
# Fixed manual exposure, operator's decision in the field (2026-11-06): 1 ms. In sun, the camera's
# auto mode was ~1 ms already (1.7 % red saturated), 3 ms saturated 63 %. Unit: 100 us. "auto" for
# the camera's aperture-priority mode. Manual also makes the real exposure appear in metadata.
RGB_EXPOSURE = _env("ATRIUM_RGB_EXPOSURE", "10")
NIR_JPEG_QUALITY = 92
# The NoIR camera is mounted upside down relative to the RGB one (seen in the field 2026-11-06):
# rotate it in the sensor readout (rpicam --rotation 180, no CPU cost) so both images agree.
NIR_ROTATION = 180
NIR_START_S = 6.0                  # rpicam-still start-up before the first capture
NIR_CAPTURE_TIMEOUT_S = 3.0
# RGB frame arrives ~1.3 s after its trigger, NIR ~0.35 s: start the NIR later so both frames
# coincide. Initial guess; missiond adapts it from the measured offset of each photo.
NIR_TRIGGER_DELAY_S = 0.95
NIR_TRIGGER_DELAY_MAX_S = 2.0
# Reduced resolution (2026-11-06): the USB pen drive the Pi boots from cannot keep up with
# full-resolution writes (the whole Pi hung). ~2 MP, 4:3 like the sensors, so the field of
# view is kept (16:9 modes such as 1080p/720p crop the sensor):
#   RGB: 1600x1200 is a native MJPEG mode of the Arducam.
#   NIR: 1640x1232 is the imx219 2x2-binned full-FOV mode.
RGB_SIZE = (_env("ATRIUM_RGB_W", 1600, int), _env("ATRIUM_RGB_H", 1200, int))
NIR_SIZE = (_env("ATRIUM_NIR_W", 1640, int), _env("ATRIUM_NIR_H", 1232, int))

# Device labels (docs/CONTRATO.md §1 example)
LABELS = {
    "imu": "IMU · MPU6500",
    "gps": "GPS · NEO-6M",
    "mag": "Mag · QMC5883P",
    "camera_rgb": "RGB · Arducam 8MP USB",
    "camera_nir": "NIR · Pi Camera V2 NoIR",
    "clock": "Relógio",
    "storage": "Armazenamento",
}
