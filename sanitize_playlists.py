#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import re
import sys
import tempfile
import zipfile
from pathlib import Path
from urllib.parse import urlsplit, urlunsplit

URL_PATTERN = re.compile(r"[a-zA-Z][a-zA-Z0-9+.-]*://[^\s\"',]+")


def hash_value(value: str, length: int = 12) -> str:
    digest = hashlib.sha256(value.encode("utf-8")).hexdigest()
    return digest[:length]


def split_attribute_list(attr_text: str) -> list[str]:
    parts: list[str] = []
    current: list[str] = []
    in_quotes = False
    quote_char = ""
    for char in attr_text:
        if char in ("'", '"'):
            if in_quotes and char == quote_char:
                in_quotes = False
                quote_char = ""
            elif not in_quotes:
                in_quotes = True
                quote_char = char
        if char == "," and not in_quotes:
            parts.append("".join(current))
            current = []
        else:
            current.append(char)
    if current:
        parts.append("".join(current))
    return parts


def sanitize_asset_line(line: str) -> str:
    prefix, attr_text = line.split(":", 1)
    attrs = split_attribute_list(attr_text)
    sanitized_attrs: list[str] = []
    for attr in attrs:
        if "=" not in attr:
            sanitized_attrs.append(attr)
            continue
        name, value = attr.split("=", 1)
        value = value.strip()
        quote_char = ""
        if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
            quote_char = value[0]
            inner = value[1:-1]
        else:
            inner = value

        if inner:
            inner = hash_value(inner)
        if quote_char:
            new_value = f"{quote_char}{inner}{quote_char}"
        else:
            new_value = inner
        sanitized_attrs.append(f"{name}={new_value}")

    return prefix + ":" + ",".join(sanitized_attrs)


def build_sanitized_query(query: str) -> str:
    if not query:
        return ""
    tokens = query.split("&")
    rebuilt: list[str] = []
    for index, token in enumerate(tokens, start=1):
        if "=" in token:
            rebuilt.append(f"key{index}=val{index}")
        else:
            rebuilt.append(f"key{index}")
    return "&".join(rebuilt)


def sanitize_url(url: str) -> str:
    parsed = urlsplit(url)
    if not parsed.scheme or not parsed.netloc:
        return url

    host = parsed.hostname or ""
    domain_parts = [part for part in host.split(".") if part]
    if len(domain_parts) >= 2:
        for index in range(len(domain_parts) - 1):
            domain_parts[index] = hash_value(domain_parts[index], 8)
    elif domain_parts:
        domain_parts[0] = hash_value(domain_parts[0], 8)
    new_host = ".".join(domain_parts)

    netloc = new_host
    if parsed.port:
        netloc = f"{new_host}:{parsed.port}"
    if parsed.username:
        user = hash_value(parsed.username, 8)
        if parsed.password:
            user = f"{user}:{hash_value(parsed.password, 8)}"
        netloc = f"{user}@{netloc}"

    segments = parsed.path.split("/")
    start_index = 1 if segments and segments[0] == "" else 0
    last_non_empty = -1
    for index, segment in enumerate(segments):
        if segment:
            last_non_empty = index
    for index in range(start_index, len(segments)):
        segment = segments[index]
        if not segment:
            continue
        if index < last_non_empty:
            segments[index] = hash_value(segment, 8)
            continue
        name_parts = segment.split(".")
        if len(name_parts) > 1:
            for part_index in range(len(name_parts) - 1):
                name_parts[part_index] = hash_value(name_parts[part_index], 8)
            segments[index] = ".".join(name_parts)
        else:
            segments[index] = hash_value(segment, 8)
    new_path = "/".join(segments) if segments else "/"

    new_query = build_sanitized_query(parsed.query)
    new_fragment = "frag1" if parsed.fragment else ""

    return urlunsplit((parsed.scheme, netloc, new_path, new_query, new_fragment))


def replace_urls_in_line(line: str) -> str:
    return URL_PATTERN.sub(lambda match: sanitize_url(match.group(0)), line)


def sanitize_text(text: str) -> str:
    lines = text.splitlines()
    sanitized_lines: list[str] = []
    for line in lines:
        if line.startswith("#EXT-X-ASSET:"):
            line = sanitize_asset_line(line)
        line = replace_urls_in_line(line)
        sanitized_lines.append(line)
    trailing = "\n" if text.endswith("\n") else ""
    return "\n".join(sanitized_lines) + trailing


def iter_m3u8_files(input_path: Path) -> tuple[Path, list[Path]]:
    if input_path.is_dir():
        return input_path, sorted(input_path.rglob("*.m3u8"))
    if input_path.is_file() and input_path.suffix == ".m3u8":
        return input_path.parent, [input_path]
    if zipfile.is_zipfile(input_path):
        temp_dir = tempfile.TemporaryDirectory()
        temp_path = Path(temp_dir.name)
        with zipfile.ZipFile(input_path) as archive:
            archive.extractall(temp_path)
        files = sorted(temp_path.rglob("*.m3u8"))
        return temp_path, files
    raise ValueError(f"Unsupported input path: {input_path}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Sanitize playlists by masking URLs and EXT-X-ASSET values."
    )
    parser.add_argument("input", type=Path, help="Directory, .m3u8 file, or zip.")
    parser.add_argument(
        "output_dir",
        type=Path,
        help="Directory to write sanitized playlists into.",
    )
    args = parser.parse_args()

    try:
        input_root, files = iter_m3u8_files(args.input)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    if not files:
        print("No .m3u8 files found.", file=sys.stderr)
        return 1

    args.output_dir.mkdir(parents=True, exist_ok=True)

    for file_path in files:
        rel_path = file_path.relative_to(input_root)
        output_path = args.output_dir / rel_path
        output_path.parent.mkdir(parents=True, exist_ok=True)
        original = file_path.read_text(errors="replace")
        sanitized = sanitize_text(original)
        output_path.write_text(sanitized)

    print(f"Sanitized {len(files)} playlist(s) into {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
