#!/usr/bin/env python3
from __future__ import annotations

from hashlib import sha256
from pathlib import Path
import sys

repo_root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(repo_root))

from openm3u8 import loads as m3u8_loads


def main() -> int:
    origins_dir = repo_root / "tests" / "playlists"
    output_file = repo_root / "tests" / "playlist_hashes.txt"

    rows: list[str] = []
    for file_path in sorted(origins_dir.glob("*.m3u8")):
        old_playlist_text = file_path.read_text(errors="replace")
        new_playlist_text = m3u8_loads(old_playlist_text).dumps()
        hashed_text = sha256(new_playlist_text.encode()).hexdigest()[:8]
        rows.append(f"{hashed_text} {file_path.name}")

    output_file.write_text("\n".join(rows) + "\n")
    print(f"Wrote {len(rows)} hashes to {output_file}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
