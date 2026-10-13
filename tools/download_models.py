"""Download the pinned W8A8 release-test model into this repository's cache."""
import argparse
import json
from pathlib import Path
import shutil
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
MODEL = json.loads((ROOT / "dependencies.json").read_text(encoding="utf-8"))["model"]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / ".cache/models" / MODEL["repository"].split("/")[1])
    args = parser.parse_args()
    base = "https://huggingface.co/" + MODEL["repository"]
    with urllib.request.urlopen("https://huggingface.co/api/models/" + MODEL["repository"] + "/revision/" + MODEL["revision"], timeout=60) as response:
        metadata = json.load(response)
    args.output.mkdir(parents=True, exist_ok=True)
    for item in metadata["siblings"]:
        name = item["rfilename"]
        if name.startswith(".") or name.lower().endswith((".md", ".png", ".jpg")):
            continue
        target = args.output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.is_file():
            continue
        temporary = target.with_suffix(target.suffix + ".download")
        with urllib.request.urlopen(base + "/resolve/" + MODEL["revision"] + "/" + name, timeout=120) as source, temporary.open("wb") as output:
            shutil.copyfileobj(source, output)
        temporary.replace(target)
    if not (args.output / "config.json").is_file():
        raise SystemExit("Downloaded model is missing config.json")
    print(args.output.resolve())

if __name__ == "__main__":
    main()
