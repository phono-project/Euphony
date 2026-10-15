"""Exercise real TSF registration and removal on a clean, elevated Windows host.

This changes machine registrations. CI runs it on its disposable Windows runner;
it refuses to run when Euphony is already registered or installed.
"""
from pathlib import Path
import ctypes
import json
import os
import subprocess
import sys
import tempfile
import winreg

from version import ROOT, VERSION, PACKAGE, PACKAGE_NAME

CLSID = r"Software\Classes\CLSID\{1E7BDC4A-9862-43C6-AA16-8A40D96614E1}"
TIP = r"Software\Microsoft\CTF\TIP\{1E7BDC4A-9862-43C6-AA16-8A40D96614E1}"
UNINSTALL = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\Euphony_is1"
VIEWS = (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY)


def value(hive, path, name="", view=winreg.KEY_WOW64_64KEY):
    try:
        with winreg.OpenKey(hive, path, 0, winreg.KEY_READ | view) as key:
            return winreg.QueryValueEx(key, name)[0]
    except FileNotFoundError:
        return None


def exists(hive, path, view=winreg.KEY_WOW64_64KEY):
    try:
        with winreg.OpenKey(hive, path, 0, winreg.KEY_READ | view):
            return True
    except FileNotFoundError:
        return False


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    require(ctypes.windll.shell32.IsUserAnAdmin(), "Installer tests require an elevated Windows host.")
    require(value(winreg.HKEY_CURRENT_USER, r"Software\Euphony", "InstallDir") is None and
            value(winreg.HKEY_CURRENT_USER, r"Software\Euphony", "X86DllPath") is None and
            not exists(winreg.HKEY_LOCAL_MACHINE, UNINSTALL) and
            not any(exists(winreg.HKEY_LOCAL_MACHINE, path, view)
                    for path in (CLSID, TIP) for view in VIEWS),
            "Use a clean host: Euphony is already installed or registered.")
    installer = ROOT / "dist" / f"{PACKAGE_NAME}-setup.exe"
    require(installer.is_file(), f"Build the installer first: {installer}")
    reports = ROOT / "build/test-results/installer"
    reports.mkdir(parents=True, exist_ok=True)
    result = reports / "result.json"
    result.unlink(missing_ok=True)
    profile = Path(os.environ["LOCALAPPDATA"]) / "Euphony"
    profile.mkdir(parents=True, exist_ok=True)
    created = []
    snapshots = {}
    for name, content in (("config.json", '{"model_enabled":false,"page_size":7}'),
                          ("learning.json", '{"installer_test":"retained"}')):
        path = profile / name
        if not path.exists():
            path.write_text(content, encoding="utf-8")
            created.append(path)
        snapshots[path] = path.read_bytes()
    with tempfile.TemporaryDirectory(prefix="installer-", dir=reports) as temporary:
        install = Path(temporary) / "Euphony 安装 test"
        uninstaller = install / "unins000.exe"

        def execute(executable, log, *arguments):
            subprocess.run([str(executable), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
                            f"/LOG={reports / log}", *arguments], check=True, timeout=180)

        def installed():
            require((install / "VERSION").read_text().strip() == VERSION, "Installed version mismatch")
            for source in PACKAGE.rglob("*"):
                if source.is_file():
                    target = install / source.relative_to(PACKAGE)
                    require(target.is_file() and target.read_bytes() == source.read_bytes(),
                            f"Installed payload differs: {target}")
            require(value(winreg.HKEY_CURRENT_USER, r"Software\Euphony", "InstallDir") == str(install),
                    "Broker discovery path mismatch")
            require(value(winreg.HKEY_CURRENT_USER, r"Software\Euphony", "X86DllPath") == str(install / "x86/EuphonyTsf.dll"),
                    "x86 recovery path mismatch")
            for view, relative in zip(VIEWS, ("EuphonyTsf.dll", "x86/EuphonyTsf.dll")):
                require(value(winreg.HKEY_LOCAL_MACHINE, CLSID + r"\InprocServer32", view=view) == str(install / relative),
                        f"TSF registration mismatch for view {view}")
                require(exists(winreg.HKEY_LOCAL_MACHINE, TIP, view), f"Missing TSF profile for view {view}")
            require(value(winreg.HKEY_LOCAL_MACHINE, UNINSTALL, "DisplayVersion") == VERSION,
                    "Missing Windows uninstall entry")
            for path, content in snapshots.items():
                require(path.read_bytes() == content, f"User data changed: {path}")

        try:
            execute(installer, "install.log", f"/DIR={install}")
            installed()
            # Install over the same directory to exercise Inno's upgrade/uninstall log.
            execute(installer, "upgrade.log", f"/DIR={install}")
            installed()
            subprocess.run([sys.executable, str(ROOT / "tools/smoke.py"),
                            "--bin-dir", str(install), "--no-model"], cwd=ROOT, check=True, timeout=180)
            execute(uninstaller, "uninstall.log")
            require(not any(exists(winreg.HKEY_LOCAL_MACHINE, path, view)
                            for path in (CLSID, TIP) for view in VIEWS), "TSF registration survived removal")
            require(not exists(winreg.HKEY_LOCAL_MACHINE, UNINSTALL), "Windows uninstall entry survived removal")
            require(value(winreg.HKEY_CURRENT_USER, r"Software\Euphony", "InstallDir") is None and
                    value(winreg.HKEY_CURRENT_USER, r"Software\Euphony", "X86DllPath") is None,
                    "Broker discovery paths survived removal")
            require(not any((install / name).exists() for name in
                            ("EuphonyBroker.exe", "EuphonySettings.exe", "EuphonyTsf.dll", "x86/EuphonyTsf.dll")),
                    "Installed binaries survived removal")
            for path, content in snapshots.items():
                require(path.read_bytes() == content, f"Uninstall changed user data: {path}")
            result.write_text(json.dumps({"install": "passed", "upgrade": "passed",
                "installed_smoke": "passed", "uninstall": "passed", "user_data": "retained"}, indent=2), encoding="utf-8")
        finally:
            if exists(winreg.HKEY_LOCAL_MACHINE, UNINSTALL) and uninstaller.is_file():
                execute(uninstaller, "cleanup.log")
            for path in created:
                # Never remove data changed by another process during a local test.
                if path.is_file() and path.read_bytes() == snapshots[path]:
                    path.unlink()
    print(result.read_text(encoding="utf-8"))


if __name__ == "__main__":
    main()
