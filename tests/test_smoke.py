"""Smoke failure cleanup; no model download, compiler or IME registration."""
from pathlib import Path
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))


class SmokeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

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
