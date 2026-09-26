import json
import os
import shutil
import tempfile
import uuid
from pathlib import Path

from flask import Flask, jsonify, request


PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ENTRIES_DIR = PROJECT_ROOT / "entries"
REQUIRED_FILES = ("rgb", "metadata", "ndvi")
SAVED_FILENAMES = {
    "rgb": "rgb.jpg",
    "metadata": "metadata.json",
    "ndvi": "ndvi.npy",
}


def create_app(entries_dir=None):
    app = Flask(__name__)
    configured_entries_dir = entries_dir or os.environ.get(
        "ENTRIES_DIR", DEFAULT_ENTRIES_DIR
    )
    app.config["ENTRIES_DIR"] = Path(configured_entries_dir).resolve()

    @app.get("/health")
    def health():
        return jsonify(status="ok")

    @app.post("/entry")
    def create_entry():
        entry_id_value = request.form.get("entry_id", "")
        try:
            entry_id = str(uuid.UUID(entry_id_value))
        except (AttributeError, TypeError, ValueError):
            return jsonify(error="entry_id must be a valid UUID"), 400

        entries_dir = app.config["ENTRIES_DIR"]
        final_dir = entries_dir / entry_id

        # A completed upload is the durable acknowledgement for a retry.
        if final_dir.is_dir():
            return jsonify(entry_id=entry_id, status="already_exists"), 200

        for field in REQUIRED_FILES:
            if field not in request.files:
                return jsonify(error=f"missing required file: {field}"), 400

        entries_dir.mkdir(parents=True, exist_ok=True)
        staging_dir = Path(
            tempfile.mkdtemp(prefix=f".{entry_id}.", dir=entries_dir)
        )

        try:
            for field, filename in SAVED_FILENAMES.items():
                request.files[field].save(staging_dir / filename)

            try:
                with (staging_dir / "metadata.json").open("r", encoding="utf-8") as file:
                    json.load(file)
            except (UnicodeDecodeError, json.JSONDecodeError):
                return jsonify(error="metadata must contain valid JSON"), 400

            try:
                os.rename(staging_dir, final_dir)
            except OSError:
                # Another concurrent retry may have finished first.
                if final_dir.is_dir():
                    return jsonify(entry_id=entry_id, status="already_exists"), 200
                raise

            return jsonify(entry_id=entry_id, status="created"), 201
        finally:
            if staging_dir.exists():
                shutil.rmtree(staging_dir)

    return app


app = create_app()
