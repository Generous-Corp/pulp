#!/usr/bin/env python3
"""No-network tests for Windows desktop action artifact fetch helpers."""

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("windows_desktop_action_fetch.py")


class WindowsDesktopActionFetchTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)
        self.paths = {
            "screenshot_path": self.root / "screenshots" / "window.png",
            "before_screenshot_path": self.root / "screenshots" / "before.png",
            "ui_snapshot_path": self.root / "ui-tree.json",
            "log_path": self.root / "stdout.log",
            "err_path": self.root / "stderr.log",
        }
        self.request = {
            "outputs": {
                "stdout": r"C:\Agent\results\job-123\stdout.log",
                "stderr": r"C:\Agent\results\job-123\stderr.log",
                "screenshot": r"C:\Agent\results\job-123\screenshots\window.png",
                "before_screenshot": r"C:\Agent\results\job-123\screenshots\before.png",
                "ui_snapshot": r"C:\Agent\results\job-123\ui-tree.json",
            },
        }

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_fetch_outputs_writes_missing_optional_logs(self) -> None:
        fetched = []

        def fetch(_host, remote_path, local_path, **kwargs):
            fetched.append((remote_path, local_path.name, kwargs))
            local_path.parent.mkdir(parents=True, exist_ok=True)
            if local_path.suffix == ".log":
                return False
            if local_path.name == "ui-tree.json":
                local_path.write_text("{}")
            else:
                local_path.write_bytes(b"png")
            return True

        self.mod.fetch_windows_session_agent_outputs(
            host="win-host",
            request=self.request,
            capture_before=True,
            capture_ui_snapshot=True,
            windows_ssh_fetch_file_fn=fetch,
            **self.paths,
        )

        self.assertEqual(self.paths["log_path"].read_text(), "")
        self.assertEqual(self.paths["err_path"].read_text(), "")
        self.assertTrue(self.paths["screenshot_path"].exists())
        self.assertTrue(self.paths["before_screenshot_path"].exists())
        self.assertTrue(self.paths["ui_snapshot_path"].exists())
        self.assertEqual(fetched[0][2], {"optional": True, "timeout": 30})


if __name__ == "__main__":
    unittest.main()
