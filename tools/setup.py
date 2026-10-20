"""Prepare the official WebView2 SDK and KenLM dependencies."""
from pathlib import Path
import urllib.request
import zipfile
import os
import shutil
import subprocess
import hashlib
import json

ROOT = Path(__file__).resolve().parents[1]
DEPENDENCIES = json.loads((ROOT / "dependencies.json").read_text(encoding="utf-8"))
WEBVIEW = DEPENDENCIES["webview2"]

def setup_kenlm():
    installed = ROOT / ".deps/vcpkg-installed"
    if (installed / "x64-windows-static/share/kenlm/kenlmConfig.cmake").exists():
        return
    vcpkg = shutil.which("vcpkg")
    if not vcpkg and os.environ.get("VCPKG_ROOT"):
        candidate = Path(os.environ["VCPKG_ROOT"]) / "vcpkg.exe"
        if candidate.exists():
            vcpkg = str(candidate)
    if not vcpkg:
        raise SystemExit("KenLM has no pixi win-64 package. Install vcpkg and add it to PATH, or set VCPKG_ROOT.")
    subprocess.run([vcpkg, "install", "--triplet", "x64-windows-static",
        "--x-install-root=" + str(installed),
        "--x-buildtrees-root=" + str(ROOT / ".deps/vcpkg-buildtrees"),
        "--x-packages-root=" + str(ROOT / ".deps/vcpkg-packages"),
        "--downloads-root=" + str(ROOT / ".deps/vcpkg-downloads"),
        "--disable-metrics"], cwd=ROOT, check=True)

def main():
    target = ROOT / ".deps" / "webview2"
    target.mkdir(parents=True, exist_ok=True)
    package = target / "webview2.nupkg"
    if not package.is_file():
        print(f"Downloading Microsoft WebView2 SDK {WEBVIEW['version']}", flush=True)
        urllib.request.urlretrieve(WEBVIEW["url"], package)
    with package.open("rb") as stream:
        actual = hashlib.file_digest(stream, "sha256").hexdigest()
    if actual != WEBVIEW["sha256"]:
        raise SystemExit("WebView2 SDK checksum mismatch; remove the cached package and retry.")
    if not (target / "build/native/include/WebView2.h").exists():
        with zipfile.ZipFile(package) as archive:
            archive.extractall(target)
    print("WebView2 SDK ready. Build tools and JSON are managed by pixi.")
    setup_kenlm()
    from prepare_libime_lm import main as prepare_language_model
    prepare_language_model()

if __name__ == "__main__":
    main()
