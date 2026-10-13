"""Use pixi packages and the installed Microsoft C++/Windows SDK toolchain."""
from pathlib import Path
import argparse
import json
import os
import shutil
import subprocess
import xml.etree.ElementTree as ET
from version import PACKAGE
from download_core import SDK

ROOT = Path(__file__).resolve().parents[1]

def required_tests(arch):
    return {"tsf_policy", "tsf_smoke"} | ({"common", "engine"} if arch == "x64" else set())

def require_tests(names, required):
    missing = required - set(names)
    if missing:
        raise RuntimeError(f"Missing required CTest tests: {', '.join(sorted(missing))}")

def verify_test_report(report, required):
    cases = ET.parse(report).getroot().findall(".//testcase")
    require_tests([case.get("name") for case in cases], required)
    for case in cases:
        if case.get("name") in required and (
            case.get("status") in ("notrun", "disabled") or
            any(case.find(tag) is not None for tag in ("skipped", "failure", "error"))
        ):
            raise RuntimeError(f"Required CTest did not pass: {case.get('name')}")

def developer_environment(arch):
    env = os.environ.copy()
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    if not vswhere.exists():
        raise SystemExit("Install Microsoft Visual Studio C++ Desktop workload with Windows SDK.")
    installations = json.loads(subprocess.check_output([str(vswhere), "-all", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-format", "json"], encoding="utf-8-sig"))
    if not installations:
        raise SystemExit("Microsoft C++ Desktop workload not found.")
    script = Path(installations[0]["installationPath"]) / "Common7/Tools/VsDevCmd.bat"
    # Only a fixed toolchain command runs in cmd; user file content is never interpolated.
    command = f'"{script}" -no_logo -arch={arch} -host_arch=x64 >nul && set'
    output = subprocess.check_output('cmd.exe /d /s /c "' + command + '"', env=env, encoding="utf-8", errors="replace")
    for line in output.splitlines():
        if "=" in line and not line.startswith("="):
            key, value = line.split("=", 1)
            env[key] = value
    # Keep pixi's CMake, Ninja and Python ahead of optional Visual Studio copies.
    pixi_prefix = Path(os.environ.get("CONDA_PREFIX", ROOT / ".pixi/envs/default"))
    env["PATH"] = str(pixi_prefix / "Library/bin") + os.pathsep + env.get("PATH", "")
    return env

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=["configure", "build", "test", "package"])
    parser.add_argument("--arch", choices=["x64", "x86"], default="x64")
    parser.add_argument("--target", default=None)
    parser.add_argument("--core-sdk", type=Path, default=os.environ.get("PHONO_CORE_SDK_DIR", SDK))
    parser.add_argument("--build-dir", type=Path, default=None,
        help="Use a separate build directory when an installed development DLL is in use")
    parser.add_argument("--install-dir", type=Path, default=PACKAGE,
        help="Package into a new directory when the previous TSF DLL is still loaded by applications")
    args = parser.parse_args()
    build = args.build_dir.resolve() if args.build_dir else ROOT / "build" / args.arch
    env = developer_environment(args.arch)
    def run(command, **kwargs):
        return subprocess.run(command, cwd=ROOT, env=env, check=True, **kwargs)
    if args.action == "configure":
        run(["cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", f"-DEUPHONY_TSF_ONLY={'ON' if args.arch == 'x86' else 'OFF'}", f"-DPHONO_CORE_SDK_DIR={args.core_sdk.resolve()}"])
    elif args.action == "build":
        command = ["cmake", "--build", str(build), "--parallel", "4"]
        if args.target:
            command += ["--target", args.target]
        run(command)
    elif args.action == "test":
        # Both TSF architectures use the same x64 inference broker, without
        # installing registry entries just to run an isolated integration test.
        env["EUPHONY_BROKER_PATH"] = os.environ.get("EUPHONY_BROKER_PATH", str(
            build / "bin/EuphonyBroker.exe" if args.arch == "x64" else ROOT / "build/x64/bin/EuphonyBroker.exe"))
        reports = build / "test-results"
        reports.mkdir(parents=True, exist_ok=True)
        report = reports / "ctest.xml"
        for name in ("ctest.xml", "ctest.log", "ctest-list.json"):
            (reports / name).unlink(missing_ok=True)
        listing = run(["ctest", "--test-dir", str(build), "--show-only=json-v1"], capture_output=True, text=True)
        (reports / "ctest-list.json").write_text(listing.stdout, encoding="utf-8")
        required = required_tests(args.arch)
        require_tests([test["name"] for test in json.loads(listing.stdout)["tests"]], required)
        run(["ctest", "--test-dir", str(build), "--output-on-failure", "--no-tests=error",
             "--output-junit", str(report), "--output-log", str(reports / "ctest.log")])
        verify_test_report(report, required)
    elif args.action == "package":
        run(["cmake", "--install", str(build), "--prefix", str(args.install_dir.resolve())])

if __name__ == "__main__":
    main()
