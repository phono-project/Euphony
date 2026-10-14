"""Exercise broker IPC and dictionary fallback; optionally require real inference."""
from pathlib import Path
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import os
import subprocess
import tempfile
import time
import uuid
import shutil
import sys
import traceback
from version import PACKAGE

sys.stdout.reconfigure(encoding="utf-8")

ROOT = Path(__file__).resolve().parents[1]
k = C.WinDLL("kernel32", use_last_error=True)
a = C.WinDLL("advapi32", use_last_error=True)
k.CreateFileW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, W.HANDLE]
k.CreateFileW.restype = W.HANDLE
k.ReadFile.argtypes = [W.HANDLE, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.c_void_p]
k.WriteFile.argtypes = k.ReadFile.argtypes
k.CloseHandle.argtypes = [W.HANDLE]
k.GetCurrentProcess.restype = W.HANDLE
a.OpenProcessToken.argtypes = [W.HANDLE, W.DWORD, C.POINTER(W.HANDLE)]
a.GetTokenInformation.argtypes = [W.HANDLE, C.c_int, C.c_void_p, W.DWORD, C.POINTER(W.DWORD)]
a.ConvertSidToStringSidW.argtypes = [C.c_void_p, C.POINTER(W.LPWSTR)]
k.LocalFree.argtypes = [C.c_void_p]
k.ProcessIdToSessionId.argtypes = [W.DWORD, C.POINTER(W.DWORD)]

def user_sid():
    token = W.HANDLE()
    if not a.OpenProcessToken(k.GetCurrentProcess(), 8, C.byref(token)):
        raise C.WinError(C.get_last_error())
    try:
        size = W.DWORD()
        a.GetTokenInformation(token, 1, None, 0, C.byref(size))
        data = C.create_string_buffer(size.value)
        if not a.GetTokenInformation(token, 1, data, size, C.byref(size)):
            raise C.WinError(C.get_last_error())
        pointer = C.cast(data, C.POINTER(C.c_void_p))[0]
        formatted = W.LPWSTR()
        if not a.ConvertSidToStringSidW(pointer, C.byref(formatted)):
            raise C.WinError(C.get_last_error())
        try:
            return formatted.value
        finally:
            k.LocalFree(C.cast(formatted, C.c_void_p))
    finally:
        k.CloseHandle(token)

def call(pipe_name, request):
    handle = k.CreateFileW(pipe_name, 0xC0000000, 0, None, 3, 0, None)
    if handle == C.c_void_p(-1).value:
        raise C.WinError(C.get_last_error())
    try:
        data = json.dumps({"version": 1, **request}, ensure_ascii=False).encode("utf-8")
        frame = len(data).to_bytes(4, "little") + data
        written = W.DWORD()
        if not k.WriteFile(handle, frame, len(frame), C.byref(written), None):
            raise C.WinError(C.get_last_error())
        def read(count):
            result = bytearray()
            while len(result) < count:
                data = C.create_string_buffer(count - len(result))
                size = W.DWORD()
                if not k.ReadFile(handle, data, len(data), C.byref(size), None) or not size.value:
                    raise C.WinError(C.get_last_error())
                result.extend(data.raw[:size.value])
            return bytes(result)
        size = int.from_bytes(read(4), "little")
        if not 0 < size <= 1024 * 1024:
            raise RuntimeError("Invalid IPC response length")
        response = json.loads(read(size))
        ack = b'{"ack":true}'
        frame = len(ack).to_bytes(4, "little") + ack
        k.WriteFile(handle, frame, len(frame), C.byref(written), None)
        return response
    finally:
        k.CloseHandle(handle)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, default=PACKAGE,
        help="Directory containing the built or packaged Euphony executables")
    parser.add_argument("--no-model", action="store_true", help="Check isolated IPC and dictionary fallback only")
    parser.add_argument("--model-dir", type=Path, default=os.environ.get("PHONO_TEST_MODEL_DIR"),
        help="Explicit downloaded Phono model directory; required unless --no-model")
    args = parser.parse_args()
    if not args.no_model and (not args.model_dir or not (args.model_dir / "config.json").is_file()):
        raise SystemExit("A model with config.json is required; pass --model-dir or PHONO_TEST_MODEL_DIR.")
    bin_dir = args.bin_dir.resolve()
    broker = bin_dir / "EuphonyBroker.exe"
    core = bin_dir / "phono_core.dll"
    if not broker.is_file() or not core.is_file():
        raise SystemExit("Build or package Euphony first: broker and phono_core.dll are required.")
    instance = "smoke-" + uuid.uuid4().hex[:12]
    desktop = W.DWORD()
    k.ProcessIdToSessionId(os.getpid(), C.byref(desktop))
    pipe = rf"\\.\pipe\Euphony-v1-{user_sid()}-{desktop.value}-{instance}"
    profile = ROOT / "build/test-results/temporary-smoke" / instance
    profile.mkdir(parents=True)
    (ROOT / "build/test-results/smoke.json").unlink(missing_ok=True)
    (profile / "config.json").write_text('{"model_enabled":false}', encoding="utf-8")
    env = {**os.environ, "EUPHONY_INSTANCE": instance, "EUPHONY_USER_DIR": str(profile)}
    process = subprocess.Popen([str(broker)], env=env, creationflags=0x08000000)
    passed = False
    seq = 0
    def request(op, session="smoke", **fields):
        nonlocal seq
        seq += 1
        return call(pipe, {"op": op, "session": session, "seq": seq, **fields})
    def type_text(text, session="smoke"):
        result = None
        for char in text:
            result = request("key", session, key=char)
            assert result.get("eaten"), result
        return result
    try:
        last_error = None
        for _ in range(1500):
            if process.poll() is not None:
                raise RuntimeError(f"Broker failed to start ({process.returncode})")
            try:
                loaded = call(pipe, {"op": "get_config"})
                break
            except OSError as error:
                last_error = error
                time.sleep(0.02)
        else:
            raise RuntimeError(f"Broker startup timed out: {last_error}")
        assert loaded["ok"], loaded
        assert loaded["status"]["language_model_ready"], loaded["status"]
        assert loaded["config"]["prefer_unigram_single_syllable"] is True
        assert loaded["config"]["page_size"] == 5 and loaded["config"]["candidate_layout"] == "vertical"
        configured = call(pipe, {"op": "set_config", "config": {"model_enabled": False}})
        assert configured["ok"], configured
        unigram = type_text("ni", session="unigram-offline")
        assert unigram["candidate_source"] == "unigram" and not unigram["pending"], unigram
        assert unigram["candidates"][0]["text"] == "你", "KenLM unigram did not recover the common single character"
        assert all(not item["model"] for item in unigram["candidates"]), unigram
        request("close", session="unigram-offline")
        retained = type_text("ni", session="config-round")
        changed = call(pipe, {"op": "set_config", "config": {"page_size": 4, "candidate_layout": "horizontal"}})
        assert changed["ok"], changed
        retained = request("poll", session="config-round")
        assert retained["preedit"] == "ni" and retained["page_size"] == 5 and retained["candidate_layout"] == "vertical", "Config reload changed active composition"
        request("key", session="config-round", key="Space")
        updated = type_text("hao", session="config-round")
        assert updated["page_size"] == 4, "New composition did not receive saved configuration"
        assert updated["candidate_layout"] == "horizontal", "New composition did not receive candidate layout"
        request("close", session="config-round")
        call(pipe, {"op": "set_config", "config": {"page_size": 5, "candidate_layout": "vertical"}})
        type_text("n", session="idle-config")
        request("key", session="idle-config", key="Escape")
        call(pipe, {"op": "set_config", "config": {"page_size": 3}})
        updated = type_text("n", session="idle-config")
        assert updated["page_size"] == 3, "Idle session delayed saved configuration by a composition"
        request("close", session="idle-config")
        call(pipe, {"op": "set_config", "config": {"page_size": 5}})
        result = type_text("nihao")
        assert result["candidates"], "Official libime dictionary returned no candidates"
        result = request("key", key="Space")
        assert result.get("commit") or result.get("preedit"), result
        request("reset")
        type_text("test", session="one")
        second = type_text("ni", session="two")
        assert "test" not in second.get("preedit", ""), "Session state leaked"
        sensitive = request("key", session="password", key="n", sensitive=True)
        assert not sensitive.get("pending"), "Sensitive input triggered inference"
        bad = call(pipe, {"op": "set_config", "config": {"page_size": 0}})
        assert not bad["ok"], "Invalid configuration accepted"
        good = call(pipe, {"op": "get_config"})
        assert good["config"]["page_size"] == 5, "Rejected configuration changed state"
        if args.no_model:
            call(pipe, {"op": "shutdown"})
            process.wait(timeout=10)
            assert process.returncode == 0
            print(json.dumps({"ipc": "passed", "dictionary_fallback": "passed", "session_isolation": "passed",
                "config_validation": "passed", "bin_directory": str(bin_dir)}, indent=2))
            passed = True
            return
        configured = call(pipe, {"op": "set_config", "config": {
            "model_enabled": True,
            "model_directory": str(args.model_dir.resolve()),
            "core_library": str(core)}})
        assert configured["ok"], configured
        def await_models(session):
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                response = request("poll", session=session)
                if not response.get("pending"):
                    found = [item for item in response.get("candidates", []) if item.get("model")]
                    assert found, f"No deep-model candidates: {response.get('error', '')}"
                    return response, found
                time.sleep(0.03)
            raise RuntimeError("Deep-model candidate timeout")
        unigram = type_text("ni", session="unigram-model-enabled")
        assert unigram["candidate_source"] == "unigram" and not unigram["pending"], unigram
        request("close", session="unigram-model-enabled")
        for char in "ni":
            contextual = request("key", session="contextual-single", key=char,
                context_valid=True, surrounding="今天我们正在测试中文输入")
        contextual, _ = await_models("contextual-single")
        assert contextual["candidate_source"] == "phono", contextual
        request("close", session="contextual-single")
        changed = call(pipe, {"op": "set_config", "config": {"prefer_unigram_single_syllable": False}})
        assert changed["ok"], changed
        type_text("ni", session="single-option-disabled")
        disabled, _ = await_models("single-option-disabled")
        assert disabled["candidate_source"] == "phono", disabled
        request("close", session="single-option-disabled")
        changed = call(pipe, {"op": "set_config", "config": {"prefer_unigram_single_syllable": True}})
        assert changed["ok"], changed
        result = type_text("nihao", session="model")
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            result = request("poll", session="model")
            if not result.get("pending"):
                break
            time.sleep(0.03)
        models = [candidate for candidate in result.get("candidates", []) if candidate.get("model")]
        assert models, f"W8A8 produced no model candidates: {result.get('error', '')}"
        committed = request("key", session="model", key="Space")
        assert committed.get("commit"), "Model candidate did not commit"
        repeated = request("poll", session="model")
        assert not repeated.get("commit"), "IPC poll repeated a commit"
        call(pipe, {"op": "shutdown"})
        process.wait(timeout=10)
        assert process.returncode == 0
        process = subprocess.Popen([str(broker)], env=env, creationflags=0x08000000)
        for _ in range(300):
            try:
                restored = call(pipe, {"op": "get_config"})
                break
            except OSError:
                time.sleep(0.02)
        assert restored["config"]["model_enabled"], "Configuration did not persist"
        assert restored["config"]["prefer_unigram_single_syllable"] is True
        report = {"ipc": "passed", "libime_dictionary": "passed", "session_isolation": "passed", "live_config_composition": "passed",
            "five_candidate_default": "passed", "candidate_layout_composition_snapshot": "passed",
            "sensitive_input": "passed", "config_validation_and_persistence": "passed",
            "kenlm_official_model": "passed", "single_syllable_unigram": "passed",
            "contextual_single_syllable_deep_model": "passed", "unigram_option_disabled": "passed",
            "w8a8_dynamic_core": "passed", "model_candidates": [item["text"] for item in models],
            "profile": str(profile), "bin_directory": str(bin_dir)}
        report_directory = ROOT / "build/test-results"
        report_directory.mkdir(parents=True, exist_ok=True)
        report_name = "smoke.json"
        (report_directory / report_name).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(json.dumps(report, ensure_ascii=False, indent=2))
        passed = True
    except Exception:
        (profile / "failure.json").write_text(json.dumps({"result": "failure",
            "error": traceback.format_exc(), "bin_directory": str(bin_dir),
            "profile": str(profile)}, ensure_ascii=False, indent=2), encoding="utf-8")
        raise
    finally:
        try:
            if process.poll() is None:
                try:
                    call(pipe, {"op": "shutdown"})
                    process.wait(timeout=10)
                except (OSError, subprocess.TimeoutExpired):
                    process.terminate()
                    process.wait(timeout=5)
        except (OSError, subprocess.TimeoutExpired) as error:
            if passed:
                raise
            print(f"Broker cleanup failed: {error}", file=sys.stderr)
        if passed:
            shutil.rmtree(profile)

if __name__ == "__main__":
    main()
