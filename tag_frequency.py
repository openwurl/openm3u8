#!/usr/bin/env python3
from __future__ import annotations

import argparse
import tempfile
import zipfile
from collections import Counter, defaultdict
from pathlib import Path
from shutil import copy2


def iter_m3u8_files(input_path: Path) -> tuple[Path, list[Path]]:
    if input_path.is_dir():
        return input_path, sorted(input_path.rglob("*.m3u8"))
    if input_path.is_file() and input_path.suffix == ".m3u8":
        return input_path.parent, [input_path]
    raise ValueError(f"Unsupported input path: {input_path}")


def count_tags(
    text: str,
    counter: Counter[str],
    files_for_tag: defaultdict[str, set[Path]] | None = None,
    source_path: Path | None = None,
) -> None:
    for line in text.splitlines():
        if not line.startswith("#"):
            continue
        tag = line.split(":", 1)[0].strip()
        if tag:
            counter[tag] += 1
            if files_for_tag is not None and source_path is not None:
                files_for_tag[tag].add(source_path)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Count playlist tag frequency across .m3u8 files."
    )
    parser.add_argument("input", type=Path, help="Directory, .m3u8 file, or zip.")
    parser.add_argument(
        "--min-count",
        type=int,
        default=1,
        help="Only show tags with at least this many occurrences.",
    )
    parser.add_argument(
        "--max-items",
        type=int,
        default=0,
        help="Limit number of tags shown (0 for all).",
    )
    parser.add_argument(
        "--rare-threshold",
        type=int,
        default=30,
        help="Copy playlists containing tags with counts below this value.",
    )
    parser.add_argument(
        "--copy-rare-dir",
        type=Path,
        default=Path("tests/playlists"),
        help="Directory to copy rare-tag playlists into.",
    )
    args = parser.parse_args()

    if zipfile.is_zipfile(args.input):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            with zipfile.ZipFile(args.input) as archive:
                archive.extractall(temp_path)
            files = sorted(temp_path.rglob("*.m3u8"))
            return report_counts(
                files,
                args.min_count,
                args.max_items,
                args.rare_threshold,
                args.copy_rare_dir,
            )

    try:
        _, files = iter_m3u8_files(args.input)
    except ValueError as exc:
        print(str(exc))
        return 2

    return report_counts(
        files,
        args.min_count,
        args.max_items,
        args.rare_threshold,
        args.copy_rare_dir,
    )


def report_counts(
    files: list[Path],
    min_count: int,
    max_items: int,
    rare_threshold: int,
    copy_rare_dir: Path,
) -> int:
    if not files:
        print("No .m3u8 files found.")
        return 1

    counter: Counter[str] = Counter()
    files_for_tag: defaultdict[str, set[Path]] = defaultdict(set)
    for file_path in files:
        text = file_path.read_text(errors="replace")
        count_tags(text, counter, files_for_tag, file_path)

    items = [(tag, count) for tag, count in counter.items() if count >= min_count]
    items.sort(key=lambda item: (item[1], item[0]))
    if max_items > 0:
        items = items[: max_items]

    for tag, count in items:
        print(f"{count:8d} {tag}")

    copy_rare_playlists(counter, files_for_tag, rare_threshold, copy_rare_dir)
    return 0


def copy_rare_playlists(
    counter: Counter[str],
    files_for_tag: defaultdict[str, set[Path]],
    rare_threshold: int,
    copy_rare_dir: Path,
) -> None:
    rare_tags = [tag for tag, count in counter.items() if count < rare_threshold]
    if not rare_tags:
        print("No rare tags found to copy.")
        return

    copy_rare_dir.mkdir(parents=True, exist_ok=True)
    seen_files: set[Path] = set()
    copied = 0
    skipped = 0

    for tag in sorted(rare_tags, key=lambda t: (counter[t], t)):
        for source_path in sorted(files_for_tag[tag]):
            if source_path in seen_files:
                continue
            seen_files.add(source_path)
            destination = copy_rare_dir / source_path.name
            if destination.exists():
                skipped += 1
                continue
            copy2(source_path, destination)
            copied += 1

    print(
        f"Copied {copied} playlist(s) with tags under {rare_threshold} "
        f"to {copy_rare_dir} (skipped {skipped})."
    )


if __name__ == "__main__":
    raise SystemExit(main())
