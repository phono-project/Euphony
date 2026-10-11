"""Validate the Win32 tray menu, embedded icons and a real IPC shortcut save."""
from pathlib import Path
import argparse
import json
import os
import subprocess
import time
import uuid
import shutil
from smoke import call, user_sid, k, W, C
from version import PACKAGE

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, default=PACKAGE)
    parser.add_argument("--visible", action="store_true", help="Briefly add a real isolated Shell icon and verify retry / recreation")
    args = parser.parse_args()
    bin_dir = args.bin_dir.resolve()
    instance = "tray-test-" + uuid.uuid4().hex[:12]
    output = ROOT / "build/test-results/temporary-tray" / instance
    profile = output / "profile"
    profile.mkdir(parents=True)
    (profile / "config.json").write_text('{"model_enabled":false}', encoding="utf-8")
    report_path = output / "result.json"
    desktop = W.DWORD()
    k.ProcessIdToSessionId(os.getpid(), C.byref(desktop))
    pipe = rf"\\.\pipe\Euphony-v1-{user_sid()}-{desktop.value}-{instance}"
    env = {**os.environ, "EUPHONY_INSTANCE": instance, "EUPHONY_USER_DIR": str(profile), "EUPHONY_TRAY_TEST": str(report_path)}
    if args.visible:
        env["EUPHONY_TRAY_TEST_SHELL"] = "1"
    process = subprocess.Popen([str(bin_dir / "EuphonyBroker.exe")], env=env, creationflags=0x08000000)
    try:
        deadline = time.monotonic() + 40
        while not report_path.exists():
            if process.poll() is not None:
                raise RuntimeError("Broker exited before its native tray test completed")
            if time.monotonic() > deadline:
                raise RuntimeError("Native tray test timed out")
            time.sleep(.05)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        if not report.get("ok"):
            raise RuntimeError(report)
        # Load resources only: this also verifies a packaged x86 DLL from x64 Python.
        k.LoadLibraryExW.argtypes = [W.LPCWSTR, W.HANDLE, W.DWORD]
        k.LoadLibraryExW.restype = W.HMODULE
        k.FreeLibrary.argtypes = [W.HMODULE]
        user = C.WinDLL("user32", use_last_error=True)
        user.LoadImageW.argtypes = [W.HINSTANCE, C.c_void_p, W.UINT, C.c_int, C.c_int, W.UINT]
        user.LoadImageW.restype = W.HANDLE
        user.DestroyIcon.argtypes = [W.HANDLE]
        files = [bin_dir / name for name in ("EuphonyBroker.exe", "EuphonySettings.exe", "EuphonyTsf.dll")]
        x86 = bin_dir / "x86/EuphonyTsf.dll"
        if x86.exists():
            files.append(x86)
        for file in files:
            module = k.LoadLibraryExW(str(file), None, 0x22)
            if not module:
                raise C.WinError(C.get_last_error())
            try:
                for size in (16, 32, 256):
                    icon = user.LoadImageW(module, C.c_void_p(101), 1, size, size, 0)
                    if not icon:
                        raise RuntimeError(f"Missing or invalid {size}px icon in {file.name}")
                    user.DestroyIcon(icon)
            finally:
                k.FreeLibrary(module)
        report["program_icons"] = [str(file.relative_to(bin_dir)) for file in files]
        report["icon_sizes"] = [16, 32, 256]
        summary = ROOT / "build/test-results/tray.json"
        summary.parent.mkdir(parents=True, exist_ok=True)
        summary.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(json.dumps(report, ensure_ascii=False, indent=2))
    finally:
        if process.poll() is None:
            try:
                call(pipe, {"op": "shutdown"})
                process.wait(timeout=20)
            except (OSError, subprocess.TimeoutExpired):
                process.terminate()
                process.wait(timeout=5)
        shutil.rmtree(output)

if __name__ == "__main__":
    main()
