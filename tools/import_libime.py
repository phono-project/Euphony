#!/usr/bin/env python3
"""Import libime's published plain-text dictionary; no libime runtime required.

Usage: python tools/import_libime.py --download --output data/libime.tsv
       python tools/import_libime.py dict_sc.txt --output data/libime.tsv
The upstream archive is streamed in memory and only dict_sc.txt is read.
"""
import argparse
import io
import json
import math
import re
import tarfile
import urllib.request
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = json.loads((ROOT / "dependencies.json").read_text(encoding="utf-8"))["dictionary"]
URL = SOURCE["url"]
FIELD = re.compile(r'"(?:[^"\\]|\\.)*"|\S+')


def source_lines(args):
    if not args.download:
        if not args.source:
            raise SystemExit("Specify a libime text export or --download")
        return Path(args.source).read_text(encoding="utf-8").splitlines()
    cached = ROOT / ".deps" / ("dict-" + SOURCE["version"] + ".tar.zst")
    cached.parent.mkdir(parents=True, exist_ok=True)
    if not cached.is_file():
        urllib.request.urlretrieve(URL, cached)
    archive = cached.read_bytes()
    if hashlib.sha256(archive).hexdigest() != SOURCE["sha256"]:
        raise SystemExit("Dictionary source checksum mismatch")
    try:
        from compression import zstd
        unpacked = zstd.decompress(archive)
    except ImportError:
        import zstandard
        unpacked = zstandard.ZstdDecompressor().decompress(archive, max_output_size=128 * 1024 * 1024)
    with tarfile.open(fileobj=io.BytesIO(unpacked)) as tar:
        member = next(m for m in tar.getmembers() if m.name == "dict_sc.txt" and m.isfile())
        return tar.extractfile(member).read().decode("utf-8").splitlines()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?")
    parser.add_argument("--download", action="store_true")
    parser.add_argument("--output", type=Path, default=Path("data/libime.tsv"))
    args = parser.parse_args()
    entries = {}
    skipped = 0
    for line in source_lines(args):
        fields = FIELD.findall(line)
        if len(fields) not in (2, 3):
            skipped += bool(line.strip())
            continue
        try:
            text = json.loads(fields[0]) if fields[0].startswith('"') else fields[0]
            pinyin = fields[1].lower()
            weight = float(fields[2]) if len(fields) == 3 else 0.0
        except (ValueError, json.JSONDecodeError):
            skipped += 1
            continue
        if not re.fullmatch("[a-z]+(?:'[a-z]+)*", pinyin) or not text or not math.isfinite(weight) or any(c in text for c in "\n\r\t"):
            skipped += 1
            continue
        key = (pinyin, text)
        entries[key] = max(entries.get(key, -math.inf), weight)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as output:
        output.write(f"# Euphony dictionary: libime dict-{SOURCE['version']}, dictionary data under LGPL-2.1-or-later.\n")
        output.write("# Source: " + URL + "\n")
        for (pinyin, text), weight in sorted(entries.items()):
            if any(c.isspace() or c in '\\"' for c in text):
                text = json.dumps(text, ensure_ascii=False)
            output.write(f"{text}\t{pinyin}\t{weight:.17g}\n")
    print(f"Imported {len(entries):,} entries to {args.output}; skipped {skipped:,} unsupported records.")


if __name__ == "__main__":
    main()
