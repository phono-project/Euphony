"""Build the official libime language model with KenLM's native binary tool."""
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request
import zstandard
import argparse
import hashlib
import json

ROOT = Path(__file__).resolve().parents[1]
SOURCE = json.loads((ROOT / "dependencies.json").read_text(encoding="utf-8"))["language_model"]
VERSION = SOURCE["version"]
URL = SOURCE["url"]

def main(source_only=False):
    output = ROOT / "data/zh_CN.lm"
    tool = ROOT / ".deps/vcpkg-installed/x64-windows-static/tools/kenlm/build_binary.exe"
    work = ROOT / ".deps/libime-lm"
    work.mkdir(parents=True, exist_ok=True)
    archive = work / f"lm_sc.arpa-{VERSION}.tar.zst"
    arpa = work / "lm_sc.arpa"
    if not archive.is_file():
        print(f"Downloading official libime LM {VERSION}…", flush=True)
        temporary = archive.with_suffix(".download")
        with urllib.request.urlopen(URL, timeout=60) as source, temporary.open("wb") as target:
            shutil.copyfileobj(source, target, 1024 * 1024)
        temporary.replace(archive)
    with archive.open("rb") as stream:
        actual = hashlib.file_digest(stream, "sha256").hexdigest()
    if actual != SOURCE["sha256"]:
        raise SystemExit("Language-model source checksum mismatch")
    if output.is_file() and not source_only:
        print("libime KenLM model ready.", flush=True)
        return
    if not arpa.is_file():
        with archive.open("rb") as source, zstandard.ZstdDecompressor().stream_reader(source) as stream:
            with tarfile.open(fileobj=stream, mode="r|") as members:
                found = False
                for member in members:
                    if member.isfile() and Path(member.name).name == "lm_sc.arpa":
                        with members.extractfile(member) as content, arpa.open("wb") as target:
                            shutil.copyfileobj(content, target, 1024 * 1024)
                        found = True
                        break
                if not found:
                    raise RuntimeError("The official archive does not contain lm_sc.arpa.")
    if source_only:
        print(f"libime ARPA source ready: {arpa}", flush=True)
        return
    if not tool.is_file():
        raise SystemExit("Run setup to install the Windows KenLM build tool first.")
    temporary = output.with_suffix(".lm.tmp")
    output.parent.mkdir(parents=True, exist_ok=True)
    print("Building the quantized trie used by libime…", flush=True)
    # These are libime's own data/CMakeLists.txt binary conversion options.
    subprocess.run([str(tool), "-s", "-a", "22", "-q", "4", "trie", str(arpa), str(temporary)], check=True)
    temporary.replace(output)
    print(f"libime KenLM model ready: {output}", flush=True)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-only", action="store_true")
    main(parser.parse_args().source_only)
