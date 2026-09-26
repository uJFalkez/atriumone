import shutil
import time
from pathlib import Path

import requests

from config import (
    PENDING_DIR,
    SERVER_URL,
    UPLOAD_INTERVAL_SECONDS,
    UPLOAD_TIMEOUT_SECONDS,
)


# ============================================================
# Server
# ============================================================

def server_available() -> bool:
    try:
        response = requests.get(
            f"{SERVER_URL}/health",
            timeout=2,
        )

        return response.ok

    except requests.RequestException:
        return False


# ============================================================
# Queue
# ============================================================

def get_pending_captures() -> list[Path]:
    if not PENDING_DIR.exists():
        return []

    return sorted(
        path
        for path in PENDING_DIR.iterdir()
        if path.is_dir()
    )


# ============================================================
# Upload
# ============================================================

def upload_capture(capture_dir: Path) -> None:
    entry_id = capture_dir.name

    rgb_path = capture_dir / "rgb.jpg"
    metadata_path = capture_dir / "metadata.json"
    ndvi_path = capture_dir / "ndvi.npy"

    required_files = (
        rgb_path,
        metadata_path,
        ndvi_path,
    )

    for path in required_files:
        if not path.is_file():
            raise RuntimeError(
                f"Captura incompleta: {path}"
            )

    print(f"Enviando {entry_id}...")

    with (
        rgb_path.open("rb") as rgb,
        metadata_path.open("rb") as metadata,
        ndvi_path.open("rb") as ndvi,
    ):
        files = {
            "rgb": (
                "rgb.jpg",
                rgb,
                "image/jpeg",
            ),
            "metadata": (
                "metadata.json",
                metadata,
                "application/json",
            ),
            "ndvi": (
                "ndvi.npy",
                ndvi,
                "application/octet-stream",
            ),
        }

        data = {
            "entry_id": entry_id,
        }

        response = requests.post(
            f"{SERVER_URL}/entry",
            data=data,
            files=files,
            timeout=UPLOAD_TIMEOUT_SECONDS,
        )

    # 4xx / 5xx
    response.raise_for_status()

    try:
        result = response.json()

    except ValueError as error:
        raise RuntimeError(
            "Servidor retornou resposta inválida"
        ) from error

    # ========================================================
    # Validate ACK
    # ========================================================

    if result.get("entry_id") != entry_id:
        raise RuntimeError(
            "Servidor respondeu com entry_id diferente"
        )

    status = result.get("status")

    if status == "created":
        if response.status_code != 201:
            raise RuntimeError(
                f"Status 'created' com HTTP "
                f"{response.status_code}"
            )

    elif status == "already_exists":
        if response.status_code != 200:
            raise RuntimeError(
                f"Status 'already_exists' com HTTP "
                f"{response.status_code}"
            )

    else:
        raise RuntimeError(
            f"Status inesperado do servidor: {status}"
        )

    # ========================================================
    # Server owns the data now
    # ========================================================

    print(
        f"Servidor confirmou {entry_id}: {status}"
    )

    shutil.rmtree(capture_dir)

    print(
        f"Captura local removida: {entry_id}"
    )


# ============================================================
# Worker
# ============================================================

def upload_pending() -> None:
    captures = get_pending_captures()

    if not captures:
        return

    if not server_available():
        print(
            f"Servidor indisponível. "
            f"{len(captures)} captura(s) pendente(s)."
        )
        return

    print(
        f"{len(captures)} captura(s) pendente(s)."
    )

    for capture_dir in captures:
        try:
            upload_capture(capture_dir)

        except requests.RequestException as error:
            print(
                f"Falha de rede ao enviar "
                f"{capture_dir.name}: {error}"
            )

            # Se perdemos o servidor, não faz sentido
            # tentar martelar o resto da fila agora.
            break

        except Exception as error:
            print(
                f"Falha ao enviar "
                f"{capture_dir.name}: {error}"
            )

            # Problema específico dessa entrada.
            # Mantém no pending e tenta a próxima.
            continue


# ============================================================
# Main
# ============================================================

def main() -> None:
    PENDING_DIR.mkdir(
        parents=True,
        exist_ok=True,
    )

    print("Atrium uploader iniciado.")
    print(f"Servidor: {SERVER_URL}")

    while True:
        try:
            upload_pending()

        except Exception as error:
            # O worker não morre por uma tentativa problemática.
            print(
                f"Erro inesperado no uploader: {error}"
            )

        time.sleep(
            UPLOAD_INTERVAL_SECONDS
        )


if __name__ == "__main__":
    main()