from __future__ import annotations

from hashlib import sha256
from pathlib import Path

from openm3u8 import loads as m3u8_loads


def load_expected_hashes(hash_file: Path) -> dict[str, str]:
    expected: dict[str, str] = {}
    for line in hash_file.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        digest, filename = line.split(maxsplit=1)
        expected[filename] = digest
    return expected


def test_roundtrip_hashes_match() -> None:
    test_root = Path(__file__).parent
    playlists_dir = test_root / "playlists"
    hash_file = test_root / "playlist_hashes.txt"

    expected = load_expected_hashes(hash_file)
    playlist_files = sorted(playlists_dir.glob("*.m3u8"))

    assert playlist_files, "No playlists found for roundtrip test."

    actual_files = {path.name for path in playlist_files}
    expected_files = set(expected.keys())
    assert actual_files == expected_files, (
        "Playlist set does not match playlist_hashes.txt."
    )

    for playlist_path in playlist_files:
        original = playlist_path.read_text(errors="replace")
        dumped = m3u8_loads(original).dumps()
        digest = sha256(dumped.encode()).hexdigest()[:8]
        assert digest == expected[playlist_path.name], (
            f"Hash mismatch for {playlist_path.name}."
        )
