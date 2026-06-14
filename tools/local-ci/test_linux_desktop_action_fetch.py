#!/usr/bin/env python3
"""No-network tests for Linux desktop action remote artifact fetch helpers."""

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).resolve().with_name("linux_desktop_action_fetch.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxDesktopActionFetchTests(unittest.TestCase):
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
            "pid_path": self.root / "pid.txt",
            "window_id_path": self.root / "window-id.txt",
            "window_title_path": self.root / "window-title.txt",
        }

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_fetch_outputs_cleans_remote_bundle(self) -> None:
        fetched = []
        cleaned = []

        def fetch(_host, remote_path, local_path, **kwargs):
            fetched.append((remote_path, local_path.name, kwargs))
            local_path.parent.mkdir(parents=True, exist_ok=True)
            local_path.write_text("payload")
            return True

        self.mod.fetch_linux_remote_action_outputs(
            host="ubuntu-host",
            remote_bundle_copy_root="~/.local/state/pulp/desktop/bundle",
            remote_bundle_cleanup_expr='"$HOME/.local/state/pulp/desktop/bundle"',
            pulp_app_automation=True,
            capture_before=True,
            capture_ui_snapshot=True,
            fetch_ssh_artifact_fn=fetch,
            cleanup_remote_ssh_dir_fn=lambda host, expr: cleaned.append((host, expr)),
            **self.paths,
        )

        self.assertIn(("~/.local/state/pulp/desktop/bundle/screenshots/window.png", "window.png", {}), fetched)
        self.assertIn(("~/.local/state/pulp/desktop/bundle/screenshots/before.png", "before.png", {"optional": False}), fetched)
        self.assertIn(("~/.local/state/pulp/desktop/bundle/ui-tree.json", "ui-tree.json", {}), fetched)
        self.assertEqual(cleaned, [("ubuntu-host", '"$HOME/.local/state/pulp/desktop/bundle"')])


if __name__ == "__main__":
    unittest.main()
