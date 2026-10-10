"""Fetch the small official WebView2 SDK; C++ libraries come from pixi."""
from pathlib import Path
import urllib.request
import zipfile
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WEBVIEW_VERSION = "1.0.4191.47"

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
    if not (target / "build/native/include/WebView2.h").exists():
        target.mkdir(parents=True, exist_ok=True)
        package = target / "webview2.nupkg"
        url = f"https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/{WEBVIEW_VERSION}/microsoft.web.webview2.{WEBVIEW_VERSION}.nupkg"
        print(f"Downloading Microsoft WebView2 SDK {WEBVIEW_VERSION}", flush=True)
        urllib.request.urlretrieve(url, package)
        with zipfile.ZipFile(package) as archive:
            archive.extractall(target)
    print("WebView2 SDK ready. Build tools and JSON are managed by pixi.")
    setup_kenlm()
    from prepare_libime_lm import main as prepare_language_model
    prepare_language_model()

if __name__ == "__main__":
    main()
