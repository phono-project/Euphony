"""Verification failure paths; no model download, compiler or IME registration."""
from pathlib import Path
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import build as build_tools
import download_models
import workflow


class VerificationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.content = b'{"fixed":true}'
        self.model = {"repository": "test/model", "revision": "fixed", "files": [
            {"path": "config.json", "size": len(self.content),
             "sha256": hashlib.sha256(self.content).hexdigest()}]}

    def test_required_tests_allow_additions_but_not_missing_tests(self):
        for arch in ("x64", "x86"):
            required = build_tools.required_tests(arch)
            build_tools.require_tests([*required, "new_test"], required)
            for actual in ([], sorted(required)[1:]):
                with self.subTest(arch=arch, actual=actual), self.assertRaisesRegex(RuntimeError, "Missing required"):
                    build_tools.require_tests(actual, required)

    def test_required_test_must_pass_in_report(self):
        required = build_tools.required_tests("x64")
        suite = ET.Element("testsuite")
        cases = {name: ET.SubElement(suite, "testcase", name=name, status="run")
                 for name in [*sorted(required), "new_test"]}
        report = self.root / "ctest.xml"
        ET.ElementTree(suite).write(report)
        build_tools.verify_test_report(report, required)
        for tag in ("skipped", "failure"):
            marker = ET.SubElement(cases["tsf_smoke"], tag)
            ET.ElementTree(suite).write(report)
            with self.subTest(tag=tag), self.assertRaisesRegex(RuntimeError, "tsf_smoke"):
                build_tools.verify_test_report(report, required)
            cases["tsf_smoke"].remove(marker)

    def test_ctest_failure_preserves_reports_and_exit_status(self):
        reports = self.root / "test-results"
        reports.mkdir()
        (reports / "ctest.xml").write_text("old success")
        listing = json.dumps({"tests": [{"name": name} for name in build_tools.required_tests("x86")]})
        def invoke(command, **kwargs):
            if "--show-only=json-v1" in command:
                self.assertFalse((reports / "ctest.xml").exists())
                return subprocess.CompletedProcess(command, 0, stdout=listing, stderr="")
            self.assertIn("--no-tests=error", command)
            (reports / "ctest.xml").write_text('<testsuite><testcase name="tsf_smoke"><failure/></testcase></testsuite>')
            (reports / "ctest.log").write_text("controlled failure")
            raise subprocess.CalledProcessError(8, command)
        with mock.patch.object(sys, "argv", ["build.py", "test", "--arch", "x86", "--build-dir", str(self.root)]), \
             mock.patch.object(build_tools, "developer_environment", return_value={}), \
             mock.patch.object(build_tools.subprocess, "run", side_effect=invoke):
            with self.assertRaises(subprocess.CalledProcessError) as failure:
                build_tools.main()
        self.assertEqual(failure.exception.returncode, 8)
        self.assertEqual((reports / "ctest.log").read_text(), "controlled failure")
        self.assertEqual((reports / "ctest-list.json").read_text(), listing)
        self.assertIn("failure", (reports / "ctest.xml").read_text())

    def test_ctest_discovery_failure_preserves_output_and_removes_old_reports(self):
        reports = self.root / "test-results"
        reports.mkdir()
        names = ("ctest.xml", "ctest.log", "ctest-list.json", "ctest-discovery.log")
        for name in names:
            (reports / name).write_text("old success")
        output = "partial discovery output\n"
        error = "CMake Error: unknown command\n"
        def invoke(command, **kwargs):
            self.assertIn("--show-only=json-v1", command)
            self.assertTrue(all(not (reports / name).exists() for name in names))
            self.assertEqual(kwargs["encoding"], "utf-8")
            self.assertEqual(kwargs["errors"], "replace")
            return subprocess.CompletedProcess(command, 8, stdout=output, stderr=error)
        with mock.patch.object(sys, "argv", ["build.py", "test", "--arch", "x86", "--build-dir", str(self.root)]), \
             mock.patch.object(build_tools, "developer_environment", return_value={}), \
             mock.patch.object(build_tools.subprocess, "run", side_effect=invoke) as run, \
             mock.patch.object(sys, "stdout", new_callable=io.StringIO) as stdout, \
             mock.patch.object(sys, "stderr", new_callable=io.StringIO) as stderr:
            with self.assertRaises(subprocess.CalledProcessError) as failure:
                build_tools.main()
        self.assertEqual(failure.exception.returncode, 8)
        self.assertEqual(failure.exception.stdout, output)
        self.assertEqual(failure.exception.stderr, error)
        run.assert_called_once()
        self.assertEqual(stdout.getvalue(), output)
        self.assertEqual(stderr.getvalue(), error)
        log = (reports / "ctest-discovery.log").read_text(encoding="utf-8")
        self.assertIn("Command: ctest --test-dir", log)
        self.assertIn("Return code: 8", log)
        self.assertIn(f"stdout:\n{output}", log)
        self.assertIn(f"stderr:\n{error}", log)
        self.assertNotIn("old success", log)
        for name in names[:-1]:
            self.assertFalse((reports / name).exists())

    def test_ctest_discovery_warnings_do_not_prevent_testing(self):
        reports = self.root / "test-results"
        names = [*sorted(build_tools.required_tests("x86")), "new_test"]
        listing = json.dumps({"tests": [{"name": name} for name in names]})
        warning = "CMake Warning: discovery warning\n"
        def invoke(command, **kwargs):
            if "--show-only=json-v1" in command:
                return subprocess.CompletedProcess(command, 0, stdout=listing, stderr=warning)
            self.assertEqual((reports / "ctest-list.json").read_text(), listing)
            self.assertIn("--no-tests=error", command)
            suite = ET.Element("testsuite")
            for name in names:
                ET.SubElement(suite, "testcase", name=name, status="run")
            ET.ElementTree(suite).write(reports / "ctest.xml")
            return subprocess.CompletedProcess(command, 0)
        with mock.patch.object(sys, "argv", ["build.py", "test", "--arch", "x86", "--build-dir", str(self.root)]), \
             mock.patch.object(build_tools, "developer_environment", return_value={}), \
             mock.patch.object(build_tools.subprocess, "run", side_effect=invoke) as run, \
             mock.patch.object(sys, "stderr", new_callable=io.StringIO) as stderr:
            build_tools.main()
        self.assertEqual(run.call_count, 2)
        self.assertEqual(stderr.getvalue(), warning)
        log = (reports / "ctest-discovery.log").read_text(encoding="utf-8")
        self.assertIn("Return code: 0", log)
        self.assertIn(listing, log)
        self.assertIn(warning, log)

    def test_existing_model_is_verified_without_network(self):
        path = self.root / "config.json"
        path.write_bytes(self.content)
        with mock.patch.object(download_models, "MODEL", self.model), \
             mock.patch.object(sys, "argv", ["download_models.py", "--output", str(self.root)]), \
             mock.patch.object(download_models.urllib.request, "urlopen") as fetch:
            download_models.main()
            fetch.assert_not_called()
            path.write_bytes(b'X' + self.content[1:])
            with self.assertRaisesRegex(RuntimeError, "SHA-256 mismatch"):
                download_models.main()
            fetch.assert_not_called()
            self.assertEqual(path.read_bytes(), b'X' + self.content[1:])

    def test_missing_model_file_is_rejected(self):
        with mock.patch.object(download_models, "MODEL", self.model):
            with self.assertRaisesRegex(RuntimeError, "Missing model file"):
                download_models.verify_model(self.root)

    def test_download_is_verified_before_becoming_a_model_file(self):
        with mock.patch.object(download_models, "MODEL", self.model), \
             mock.patch.object(sys, "argv", ["download_models.py", "--output", str(self.root)]), \
             mock.patch.object(download_models.urllib.request, "urlopen", return_value=io.BytesIO(b'X' + self.content[1:])):
            with self.assertRaisesRegex(RuntimeError, "SHA-256 mismatch"):
                download_models.main()
        self.assertFalse((self.root / "config.json").exists())
        with mock.patch.object(download_models, "MODEL", self.model), \
             mock.patch.object(sys, "argv", ["download_models.py", "--output", str(self.root)]), \
             mock.patch.object(download_models.urllib.request, "urlopen", return_value=io.BytesIO(self.content)):
            download_models.main()
        self.assertEqual((self.root / "config.json").read_bytes(), self.content)

    def test_release_rejects_wrong_explicit_model_before_smoke(self):
        (self.root / "config.json").write_bytes(b'X' + self.content[1:])
        with mock.patch.object(download_models, "MODEL", self.model), \
             mock.patch.object(sys, "argv", ["workflow.py", "release", "--model-dir", str(self.root)]), \
             mock.patch.object(workflow, "run") as run:
            with self.assertRaisesRegex(RuntimeError, "SHA-256 mismatch"):
                workflow.main()
            run.assert_not_called()

    @unittest.skipUnless(os.name == "nt", "Windows broker smoke")
    def test_smoke_failure_preserves_isolated_profile(self):
        import smoke
        for name in ("EuphonyBroker.exe", "phono_core.dll"):
            (self.root / name).touch()
        reports = self.root / "build/test-results"
        reports.mkdir(parents=True)
        (reports / "smoke.json").write_text('{"old":"success"}')
        process = mock.Mock()
        process.poll.return_value = None
        response = {"ok": True, "status": {"language_model_ready": False}}
        with mock.patch.object(smoke, "ROOT", self.root), \
             mock.patch.object(sys, "argv", ["smoke.py", "--no-model", "--bin-dir", str(self.root)]), \
             mock.patch.object(smoke, "user_sid", return_value="test-user"), \
             mock.patch.object(smoke.subprocess, "Popen", return_value=process), \
             mock.patch.object(smoke, "call", return_value=response):
            with self.assertRaises(AssertionError):
                smoke.main()
        failures = list(reports.glob("temporary-smoke/*/failure.json"))
        self.assertEqual(len(failures), 1)
        failure = json.loads(failures[0].read_text(encoding="utf-8"))
        self.assertIn("AssertionError", failure["error"])
        self.assertTrue((failures[0].parent / "config.json").is_file())
        self.assertFalse((reports / "smoke.json").exists())
        process.wait.assert_called_once_with(timeout=10)


if __name__ == "__main__":
    unittest.main()
