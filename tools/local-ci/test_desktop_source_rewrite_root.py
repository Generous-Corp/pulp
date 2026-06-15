#!/usr/bin/env python3
"""No-network tests for desktop source-root command rewrites."""

from __future__ import annotations

import pathlib
import tempfile
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_source_rewrite_root.py")


class DesktopSourceRewriteRootTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_rewrite_launch_command_for_local_posix_and_windows_roots(self) -> None:
        prepared = self.root / "prepared"
        self.assertEqual(
            self.mod.rewrite_launch_command_for_source_root(
                f"{self.repo}/bin/ui-preview --smoke",
                prepared,
                root=self.repo,
            ),
            f"{prepared}/bin/ui-preview --smoke",
        )
        self.assertEqual(
            self.mod.rewrite_launch_command_for_posix_root("./scripts/run-preview --smoke", "/home/dev/pulp", root=self.repo),
            "/home/dev/pulp/scripts/run-preview --smoke",
        )
        self.assertEqual(
            self.mod.rewrite_launch_command_for_windows_root(
                r".\scripts\run-preview.exe --smoke",
                r"C:\pulp",
                root=self.repo,
                windows_path_join_fn=lambda *parts: "\\".join(parts),
            ),
            r"C:\pulp\scripts\run-preview.exe --smoke",
        )


if __name__ == "__main__":
    unittest.main()
