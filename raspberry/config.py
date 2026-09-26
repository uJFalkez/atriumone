from pathlib import Path


BASE_DIR = Path(__file__).resolve().parent

DATA_DIR = BASE_DIR / "data"
PROCESSING_DIR = DATA_DIR / "processing"
PENDING_DIR = DATA_DIR / "pending"

ALIGNMENT_PATH = BASE_DIR / "alignment.npy"

# Cameras

USB_DEVICE = "/dev/video0"
USB_SIZE = (3264, 2448)
CSI_SIZE = (3280, 2464)

USB_WARMUP_FRAMES = 60
CSI_WARMUP_SECONDS = 2

USB_FLUSH_FRAMES = 3

# Processing

NDVI_BLOCK_SIZE = 128

CROP_X1 = 49
CROP_Y1 = 0
CROP_X2 = 3062
CROP_Y2 = 2049

# Server

SERVER_URL = "http://station:5000"

UPLOAD_INTERVAL_SECONDS = 5
UPLOAD_TIMEOUT_SECONDS = 30