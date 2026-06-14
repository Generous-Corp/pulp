#!/usr/bin/env python3
"""No-network tests for local-ci desktop source preparation helpers."""

from __future__ import annotations

import argparse
import pathlib
import tempfile
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("source_prep.py")


class SourcePrepTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def request(self, **overrides) -> dict:
        request = {
            "mode": "exact-sha",
            "branch": "feature/source",
            "sha": "a" * 40,
            "prepare_command": None,
            "prepare_timeout_secs": 120.0,
        }
        request.update(overrides)
        return request

    def test_request_cache_key_and_command_rewrites(self) -> None:
        args = argparse.Namespace(
            source_mode="EXACT-SHA",
            branch=None,
            sha=None,
            prepare_command="  ./scripts/build-ui.sh  ",
            prepare_timeout=42,
        )

        request = self.mod.make_desktop_source_request(
            args,
            normalize_desktop_source_mode_fn=lambda value: str(value).lower(),
            current_branch_fn=lambda: "main",
            current_sha_fn=lambda: "b" * 40,
        )

        self.assertEqual(request["mode"], "exact-sha")
        self.assertEqual(request["branch"], "main")
        self.assertEqual(request["sha"], "b" * 40)
        self.assertEqual(request["prepare_command"], "./scripts/build-ui.sh")
        self.assertEqual(request["prepare_timeout_secs"], 42.0)

        same_material = {**request, "mode": "live", "branch": "other"}
        changed_prepare = {**request, "prepare_command": "cmake --build build"}
        self.assertEqual(self.mod.desktop_source_cache_key(request), self.mod.desktop_source_cache_key(same_material))
        self.assertNotEqual(
            self.mod.desktop_source_cache_key(request),
            self.mod.desktop_source_cache_key(changed_prepare),
        )

        state = self.root / "state"
        source_root = self.mod.desktop_source_root("mac", request, state_dir_fn=lambda: state)
        self.assertEqual(source_root.parent, state / "desktop-source" / "mac")

        prepared = self.root / "prepared"
        rewritten = self.mod.rewrite_launch_command_for_source_root(
            f"{self.repo}/bin/ui-preview --label 'UI Preview'",
            prepared,
            root=self.repo,
        )
        self.assertEqual(rewritten, f"{prepared}/bin/ui-preview --label 'UI Preview'")

        posix = self.mod.rewrite_launch_command_for_posix_root(
            "./scripts/run-preview --smoke",
            "/home/dev/pulp",
            root=self.repo,
        )
        self.assertEqual(posix, "/home/dev/pulp/scripts/run-preview --smoke")

        windows = self.mod.rewrite_launch_command_for_windows_root(
            r".\scripts\run-preview.exe --smoke",
            r"C:\pulp",
            root=self.repo,
            windows_path_join_fn=lambda *parts: "\\".join(parts),
        )
        self.assertEqual(windows, r"C:\pulp\scripts\run-preview.exe --smoke")

        malformed = '"unterminated'
        self.assertEqual(
            self.mod.rewrite_launch_command_for_source_root(malformed, prepared, root=self.repo),
            malformed,
        )
        self.assertEqual(
            self.mod.rewrite_launch_command_for_source_root("/usr/bin/true --flag", prepared, root=self.repo),
            "/usr/bin/true --flag",
        )

    def test_windows_prepare_validation_and_manifest_attachment(self) -> None:
        commands = self.mod.split_windows_prepare_commands('cmake -G "Visual Studio; 17"; echo ok\nninja')
        self.assertEqual(commands, ['cmake -G "Visual Studio; 17"', "echo ok", "ninja"])

        with self.assertRaisesRegex(ValueError, "Use double quotes"):
            self.mod.validate_windows_prepare_commands(["cmake -G 'Ninja'"])

        manifest: dict = {}
        self.mod.attach_desktop_source_to_manifest(
            manifest,
            {
                "mode": "exact-sha",
                "branch": "feature/source",
                "sha": "c" * 40,
                "prepare_command": "cmake --build build",
                "prepare_timeout_secs": 120.0,
                "prepared_root": "/actual/root",
                "prepared_root_display": "~/display/root",
                "launch_cwd": "/actual/cwd",
                "launch_cwd_display": "~/display/cwd",
                "prepare_log": "prepare.log",
            },
        )

        self.assertEqual(manifest["source"]["prepared_root"], "~/display/root")
        self.assertEqual(manifest["source"]["launch_cwd"], "~/display/cwd")
        self.assertEqual(manifest["artifacts"]["prepare_log"], "prepare.log")

        unchanged = {"status": "pass"}
        self.mod.attach_desktop_source_to_manifest(unchanged, None)
        self.assertEqual(unchanged, {"status": "pass"})

if __name__ == "__main__":
    unittest.main()
