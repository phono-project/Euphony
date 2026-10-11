"""Read the application version from the existing pixi workspace manifest."""
from pathlib import Path
import re
import tomllib

ROOT = Path(__file__).resolve().parents[1]
VERSION = tomllib.loads((ROOT / "pixi.toml").read_text(encoding="utf-8"))["workspace"]["version"]
if not re.fullmatch(r"\d+\.\d+\.\d+", VERSION):
    raise ValueError("Euphony version must use X.Y.Z")
PACKAGE_NAME = f"Euphony-{VERSION}-windows-x64"
PACKAGE = ROOT / "dist" / PACKAGE_NAME

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag")
    args = parser.parse_args()
    if args.tag and args.tag != "v" + VERSION:
        raise SystemExit(f"Release tag {args.tag} does not match v{VERSION}")
    print(VERSION)
