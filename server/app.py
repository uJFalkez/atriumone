import json
import os
import shutil
import tempfile
import threading
import uuid
from pathlib import Path

from flask import Flask, jsonify, request

import config
from missions import InvalidMission, validate_mission, validate_track
from processing import needs_processing, process_safely


REQUIRED_FILES = {
    "rgb": "rgb.jpg",
    "nir": "nir.jpg",
    "metadata": "metadata.json",
}

# O processamento é pesado em memória; um por vez.
_processing_lock = threading.Lock()


def process(entry_dir: Path):
    with _processing_lock:
        print(f"Processando {entry_dir.name}...")
        if process_safely(entry_dir):
            print(f"Processado: {entry_dir.name}")


def process_backlog(entries_dir: Path):
    """Processa entradas recebidas mas ainda não processadas (ex.: após queda)."""
    if not entries_dir.is_dir():
        return
    for entry_dir in sorted(entries_dir.iterdir()):
        if entry_dir.is_dir() and not entry_dir.name.startswith(".") and needs_processing(entry_dir):
            process(entry_dir)


def create_app(entries_dir=None, backlog=True, missions_dir=None):
    app = Flask(__name__)
    app.config["ENTRIES_DIR"] = Path(entries_dir or config.ENTRIES_DIR).resolve()
    app.config["MISSIONS_DIR"] = Path(missions_dir or config.MISSIONS_DIR).resolve()

    if backlog:
        threading.Thread(
            target=process_backlog, args=(app.config["ENTRIES_DIR"],), daemon=True
        ).start()

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
        staging_dir = Path(tempfile.mkdtemp(prefix=f".{entry_id}.", dir=entries_dir))

        try:
            for field, filename in REQUIRED_FILES.items():
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
        finally:
            if staging_dir.exists():
                shutil.rmtree(staging_dir)

        # Os dados crus já estão seguros na estação. O processamento roda em
        # segundo plano para o ACK chegar rápido ao módulo; falhas ficam em
        # error.json e podem ser refeitas com reprocess.py.
        threading.Thread(target=process, args=(final_dir,), daemon=True).start()

        return jsonify(entry_id=entry_id, status="created"), 201

    @app.post("/mission")
    def register_mission():
        """Registro da missão: mission.json + track.csv (histórico de posições)."""
        try:
            mission_id = str(uuid.UUID(request.form.get("mission_id", "")))
        except (AttributeError, TypeError, ValueError):
            return jsonify(error="mission_id must be a valid UUID"), 400

        missions_dir = app.config["MISSIONS_DIR"]
        final_dir = missions_dir / mission_id
        if (final_dir / "mission.json").is_file():
            return jsonify(mission_id=mission_id, status="already_exists"), 200

        for field in ("mission", "track"):
            if field not in request.files:
                return jsonify(error=f"missing required file: {field}"), 400

        mission_raw = request.files["mission"].read()
        track_raw = request.files["track"].read()
        try:
            validate_mission(mission_raw, mission_id)
            samples = validate_track(track_raw)
        except InvalidMission as error:
            return jsonify(error=str(error)), 400

        # A pasta da missão pode já existir com o registro do dashboard
        # (record.json); os arquivos do módulo entram de forma atômica.
        final_dir.mkdir(parents=True, exist_ok=True)
        for name, raw in (("track.csv", track_raw), ("mission.json", mission_raw)):
            temporary = final_dir / f".{name}.{uuid.uuid4().hex}.tmp"
            temporary.write_bytes(raw)
            os.replace(temporary, final_dir / name)  # mission.json por último

        return jsonify(mission_id=mission_id, status="created", samples=samples), 201

    return app


app = create_app()
