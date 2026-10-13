"""Download the pinned W8A8 release-test model into this repository's cache."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
MODEL = json.loads((ROOT / "dependencies.json").read_text(encoding="utf-8"))["model"]

def check_file(path, spec):
    if not path.is_file():
        raise RuntimeError(f"Missing model file: {path}")
    if path.stat().st_size != spec["size"]:
        raise RuntimeError(f"Model size mismatch: {path}; expected {spec['size']}, found {path.stat().st_size}")
    with path.open("rb") as stream:
        actual = hashlib.file_digest(stream, "sha256").hexdigest()
    if actual != spec["sha256"]:
        raise RuntimeError(f"Model SHA-256 mismatch: {path}; expected {spec['sha256']}, found {actual}")

def verify_model(directory):
    for spec in MODEL["files"]:
        check_file(directory / spec["path"], spec)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / ".cache/models" / MODEL["repository"].split("/")[1])
    args = parser.parse_args()
    base = "https://huggingface.co/" + MODEL["repository"]
    args.output.mkdir(parents=True, exist_ok=True)
    for spec in MODEL["files"]:
        name = spec["path"]
        target = args.output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.is_file():
            check_file(target, spec)
            continue
        temporary = target.with_suffix(target.suffix + ".download")
        with urllib.request.urlopen(base + "/resolve/" + MODEL["revision"] + "/" + name, timeout=120) as source, temporary.open("wb") as output:
            shutil.copyfileobj(source, output)
        check_file(temporary, spec)
        temporary.replace(target)
    print(args.output.resolve())

if __name__ == "__main__":
    main()
