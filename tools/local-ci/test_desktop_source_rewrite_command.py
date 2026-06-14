#!/usr/bin/env python3
"""No-network tests for desktop source command rewrite helpers."""

from __future__ import annotations

import pathlib
import tempfile
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_source_rewrite_command.py")


class DesktopSourceRewriteCommandTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_command_path_rewrite_candidate_accepts_repo_local_paths_only(self) -> None:
        self.assertEqual(
            self.mod.command_path_rewrite_candidate(str(self.repo / "bin" / "app"), root=self.repo),
            self.repo / "bin" / "app",
        )
        self.assertEqual(
            self.mod.command_path_rewrite_candidate("./scripts/run-preview", root=self.repo),
            self.repo / "scripts" / "run-preview",
        )
        self.assertIsNone(self.mod.command_path_rewrite_candidate("/usr/bin/true", root=self.repo))
        self.assertIsNone(self.mod.command_path_rewrite_candidate("cmake", root=self.repo))

    def test_rewrite_launch_command_for_mapper_rewrites_first_repo_path_token(self) -> None:
        rewritten = self.mod.rewrite_launch_command_for_mapper(
            f"{self.repo}/bin/ui-preview --label 'UI Preview'",
            lambda rel: f"/prepared/{rel.as_posix()}",
            root=self.repo,
        )
        self.assertEqual(rewritten, "/prepared/bin/ui-preview --label 'UI Preview'")

        malformed = '"unterminated'
        self.assertEqual(
            self.mod.rewrite_launch_command_for_mapper(malformed, lambda rel: str(rel), root=self.repo),
            malformed,
        )

        windows = self.mod.rewrite_launch_command_for_mapper(
            r".\scripts\run-preview.exe --smoke",
            lambda rel: "C:\\pulp\\" + str(rel).replace("/", "\\"),
            root=self.repo,
            windows=True,
        )
        self.assertEqual(windows, r"C:\pulp\scripts\run-preview.exe --smoke")


if __name__ == "__main__":
    unittest.main()
