#!/usr/bin/env python3
"""No-network tests for Linux desktop action metadata helpers."""

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).resolve().with_name("linux_desktop_action_metadata.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxDesktopActionMetadataTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_read_pid_ignores_missing_or_invalid_values(self) -> None:
        pid_path = self.root / "pid.txt"
        self.assertIsNone(self.mod.read_linux_pid_file(pid_path))
        pid_path.write_text("not-a-pid")
        self.assertIsNone(self.mod.read_linux_pid_file(pid_path))
        pid_path.write_text("4242")
        self.assertEqual(self.mod.read_linux_pid_file(pid_path), 4242)

    def test_attach_window_metadata_uses_available_files(self) -> None:
        window_id_path = self.root / "window-id.txt"
        window_title_path = self.root / "window-title.txt"
        manifest: dict = {}
        self.mod.attach_linux_window_metadata(manifest, window_id_path=window_id_path, window_title_path=window_title_path)
        self.assertEqual(manifest, {})

        window_id_path.write_text("0x123")
        window_title_path.write_text("UI Preview")
        self.mod.attach_linux_window_metadata(manifest, window_id_path=window_id_path, window_title_path=window_title_path)
        self.assertEqual(manifest["window"], {"window_id": "0x123", "title": "UI Preview"})


if __name__ == "__main__":
    unittest.main()
