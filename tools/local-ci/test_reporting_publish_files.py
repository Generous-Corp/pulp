#!/usr/bin/env python3
"""Tests for desktop publish filesystem helpers."""

from __future__ import annotations

import pathlib
import tempfile
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("reporting_publish_files.py", add_module_dir=True)


class ReportingPublishFilesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_directory_copy_and_clear_helpers_preserve_git_directory(self) -> None:
        src = self.root / "src"
        dest = self.root / "dest"
        (src / "nested").mkdir(parents=True)
        (src / "file.txt").write_text("file")
        (src / "nested" / "child.txt").write_text("child")
        (dest / ".git").mkdir(parents=True)
        (dest / "old.txt").write_text("old")

        self.mod.copy_directory_contents(src, dest)

        self.assertEqual((dest / "file.txt").read_text(), "file")
        self.assertEqual((dest / "nested" / "child.txt").read_text(), "child")

        self.mod.clear_directory_contents(dest)

        self.assertTrue((dest / ".git").is_dir())
        self.assertFalse((dest / "file.txt").exists())
        self.assertFalse((dest / "nested").exists())
        self.assertFalse((dest / "old.txt").exists())


if __name__ == "__main__":
    unittest.main()
