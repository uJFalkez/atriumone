from contextlib import asynccontextmanager

from fastapi import FastAPI

from camera import CameraManager
from capture import capture
from config import PENDING_DIR


camera = CameraManager()


@asynccontextmanager
async def lifespan(app: FastAPI):
    PENDING_DIR.mkdir(
        parents=True,
        exist_ok=True,
    )

    camera.start()

    yield

    camera.stop()


app = FastAPI(
    title="Atrium One",
    lifespan=lifespan,
)


@app.get("/health")
def health():
    return {
        "status": "ok",
    }


@app.post("/capture")
def trigger_capture():
    capture_id = capture(camera)

    return {
        "id": capture_id,
        "status": "pending",
    }