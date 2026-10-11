"""Fetch the pinned Windows core SDK and verify its published asset checksum."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / "dependencies.json").read_text(encoding="utf-8"))["core"]
SDK = ROOT / ".deps" / f"phono-core-{LOCK['version']}-windows-x64"

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=SDK)
    args = parser.parse_args()
    cache = ROOT / ".deps/core-downloads"
    cache.mkdir(parents=True, exist_ok=True)
    base = f"https://github.com/{LOCK['repository']}/releases/download/v{LOCK['version']}/"
    def fetch(name):
        path = cache / name
        headers = {"User-Agent": "Euphony-build"}
        if os.environ.get("GITHUB_TOKEN"):
            headers["Authorization"] = "Bearer " + os.environ["GITHUB_TOKEN"]
        request = urllib.request.Request(base + name, headers=headers)
        with urllib.request.urlopen(request, timeout=120) as source, path.open("wb") as target:
            import shutil
            shutil.copyfileobj(source, target)
        return path
    sums = fetch(LOCK["checksums"])
    expected = None
    for line in sums.read_text(encoding="utf-8").splitlines():
        value, name = line.split(maxsplit=1)
        if name.lstrip("*") == LOCK["asset"]:
            expected = value.lower()
    if expected is None:
        raise SystemExit(f"Release checksum manifest does not contain {LOCK['asset']}")
    archive = cache / LOCK["asset"]
    cached_checksum = None
    if archive.exists():
        with archive.open("rb") as stream:
            cached_checksum = hashlib.file_digest(stream, "sha256").hexdigest()
    if cached_checksum != expected:
        archive = fetch(LOCK["asset"])
    with archive.open("rb") as stream:
        actual = hashlib.file_digest(stream, "sha256").hexdigest()
    if actual != expected:
        raise SystemExit("Core SDK checksum mismatch")
    args.output.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as source:
        source.extractall(args.output)
    if (args.output / "share/phono-core/VERSION").read_text(encoding="utf-8").strip() != LOCK["version"]:
        raise SystemExit("Core SDK version mismatch")
    receipt = {**LOCK, "sha256": actual}
    (args.output / "core-dependency.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(args.output.resolve())

if __name__ == "__main__":
    main()
