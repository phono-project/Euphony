"""Shared local and CI build, test and clean package entry points."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

from version import ROOT, VERSION, PACKAGE, PACKAGE_NAME

def run(*arguments):
    subprocess.run(arguments, cwd=ROOT, check=True)

def package():
    stage = (ROOT / "dist/.package-stage").resolve()
    stage.relative_to(ROOT.resolve())
    if stage.exists():
        shutil.rmtree(stage)
    for arch in ("x64", "x86"):
        run(sys.executable, "tools/build.py", "package", "--arch", arch, "--install-dir", str(stage))
    required = ("EuphonyBroker.exe", "EuphonySettings.exe", "EuphonyTsf.dll", "x86/EuphonyTsf.dll",
        "phono_core.dll", "WebView2Loader.dll", "settings.html", "data/libime.tsv", "data/zh_CN.lm",
        "tools/manage.ps1", "tools/register.ps1", "tools/unregister.ps1", "tools/version.ps1", "VERSION")
    for name in required:
        if not (stage / name).is_file():
            raise RuntimeError(f"Package is missing {name}")
    if (stage / "VERSION").read_text(encoding="utf-8").strip() != VERSION:
        raise RuntimeError("Package version mismatch")
    if PACKAGE.exists():
        PACKAGE.resolve().relative_to(ROOT.resolve())
        shutil.rmtree(PACKAGE)
    stage.rename(PACKAGE)
    archive = ROOT / "dist" / f"{PACKAGE_NAME}.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as output:
        for file in sorted(PACKAGE.rglob("*")):
            if file.is_file():
                output.write(file, file.relative_to(PACKAGE.parent))
    print(f"Package: {PACKAGE}\nArchive: {archive}")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("build", "test", "package", "verify", "desktop", "release"))
    parser.add_argument("--core-sdk", type=Path, default=os.environ.get("PHONO_CORE_SDK_DIR"))
    parser.add_argument("--model-dir", type=Path, default=os.environ.get("PHONO_TEST_MODEL_DIR"))
    args = parser.parse_args()
    if args.action == "build":
        for arch in ("x64", "x86"):
            options = ["--core-sdk", str(args.core_sdk)] if args.core_sdk else []
            run(sys.executable, "tools/build.py", "configure", "--arch", arch, *options)
            run(sys.executable, "tools/build.py", "build", "--arch", arch)
    elif args.action in ("test", "verify"):
        for arch in ("x64", "x86"):
            run(sys.executable, "tools/build.py", "test", "--arch", arch)
        run(sys.executable, "tools/smoke.py", "--bin-dir", str(ROOT / "build/x64/bin"), "--no-model")
        run("powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
            "tests/registration_process_tests.ps1", "-ToolsDirectory", str(ROOT / "tools"))
    elif args.action == "package":
        package()
    elif args.action == "desktop":
        for tool in ("ui_test", "tray_test"):
            run(sys.executable, f"tools/{tool}.py", "--bin-dir", str(PACKAGE))
    elif args.action == "release":
        if not args.model_dir or not (args.model_dir / "config.json").is_file():
            raise SystemExit("Release validation requires --model-dir or PHONO_TEST_MODEL_DIR; missing models fail.")
        archive = ROOT / "dist" / f"{PACKAGE_NAME}.zip"
        with tempfile.TemporaryDirectory(prefix="euphony-release-") as temporary:
            with zipfile.ZipFile(archive) as source:
                source.extractall(temporary)
            unpacked = Path(temporary) / PACKAGE_NAME
            run(sys.executable, "tools/smoke.py", "--bin-dir", str(unpacked), "--model-dir", str(args.model_dir.resolve()))

if __name__ == '__main__':
    main()
