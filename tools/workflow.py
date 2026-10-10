"""One build tree per architecture, one current release and one verification entry."""
from pathlib import Path
import argparse
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'dist/Euphony-0.2.0'

def run(*arguments):
    subprocess.run(arguments, cwd=ROOT, check=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('build', 'test', 'package', 'verify'))
    action = parser.parse_args().action
    if action == 'build':
        for arch in ('x64', 'x86'):
            for operation in ('configure', 'build'):
                run(sys.executable, 'tools/build.py', operation, '--arch', arch)
    elif action == 'test':
        for arch in ('x64', 'x86'):
            run(sys.executable, 'tools/build.py', 'test', '--arch', arch)
    elif action == 'package':
        for arch in ('x64', 'x86'):
            run(sys.executable, 'tools/build.py', 'package', '--arch', arch)
        archive = ROOT / 'dist/Euphony-win64.zip'
        with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as output:
            for file in sorted(PACKAGE.rglob('*')):
                if file.is_file():
                    output.write(file, file.relative_to(PACKAGE.parent))
        print(f'Current release: {PACKAGE}\nArchive: {archive}')
    else:
        for arch in ('x64', 'x86'):
            run(sys.executable, 'tools/build.py', 'test', '--arch', arch)
        for tool in ('smoke', 'ui_test', 'tray_test'):
            run(sys.executable, f'tools/{tool}.py', '--bin-dir', str(PACKAGE))
        run('powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
            'tests/registration_process_tests.ps1', '-ToolsDirectory', str(PACKAGE / 'tools'))

if __name__ == '__main__':
    main()
