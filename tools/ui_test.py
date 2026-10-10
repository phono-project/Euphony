"""Run the real WebView2 host invisibly in an isolated development profile."""
from pathlib import Path
import argparse
import json
import os
import subprocess
import time
import uuid
import shutil
from smoke import call, user_sid, k, W, C

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, default=ROOT / "dist/Euphony-0.2.0")
    args = parser.parse_args()
    bin_dir = args.bin_dir.resolve()
    output = ROOT / "build/test-results/settings"
    output.mkdir(parents=True, exist_ok=True)
    instance = "ui-test-" + uuid.uuid4().hex[:12]
    desktop = W.DWORD()
    k.ProcessIdToSessionId(os.getpid(), C.byref(desktop))
    pipe = rf"\\.\pipe\Euphony-v1-{user_sid()}-{desktop.value}-{instance}"
    env = {**os.environ, "EUPHONY_USER_DIR": str(output / "profile"), "EUPHONY_INSTANCE": instance,
        "EUPHONY_BROKER_PATH": str(bin_dir / "EuphonyBroker.exe")}
    try:
        result = subprocess.run([str(bin_dir / "EuphonySettings.exe"), "--self-test", str(output)], env=env, timeout=45)
        report = json.loads((output / "result.json").read_text(encoding="utf-8"))
        print(json.dumps(report, ensure_ascii=False, indent=2))
        if result.returncode or not report.get("ok"):
            raise SystemExit("WebView2 self-test failed")
    finally:
        try:
            call(pipe, {"op": "shutdown"})
        except OSError:
            pass
        # Only the disposable WebView profile is removed; reports/screenshots remain.
        shutil.rmtree(output / 'profile', ignore_errors=True)

if __name__ == "__main__":
    main()
