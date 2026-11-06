"""Reprocessa entradas da estação (ex.: após ajustar parâmetros de detecção).

Uso:
    python reprocess.py            # só as pendentes
    python reprocess.py --all      # todas as entradas com nir.jpg
    python reprocess.py <uuid>...  # entradas específicas
"""

import sys

import config
from processing import needs_processing, process_safely


def main(arguments):
    entries_dir = config.ENTRIES_DIR
    force_all = "--all" in arguments
    ids = [a for a in arguments if not a.startswith("--")]

    if ids:
        targets = [entries_dir / entry_id for entry_id in ids]
    else:
        targets = sorted(
            path for path in entries_dir.iterdir()
            if path.is_dir() and not path.name.startswith(".")
        )
        targets = [
            path for path in targets
            if (path / "nir.jpg").is_file() and (force_all or needs_processing(path))
        ]

    if not targets:
        print("Nada para processar.")
        return

    failures = 0
    for index, entry_dir in enumerate(targets, start=1):
        (entry_dir / "error.json").unlink(missing_ok=True)
        ok = process_safely(entry_dir)
        failures += not ok
        print(f"[{index}/{len(targets)}] {entry_dir.name}: {'ok' if ok else 'falhou'}")

    print(f"Concluído: {len(targets) - failures} ok, {failures} com falha.")


if __name__ == "__main__":
    main(sys.argv[1:])
