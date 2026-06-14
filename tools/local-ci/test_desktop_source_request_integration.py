#!/usr/bin/env python3
"""Facade-level desktop source request and manifest integration tests."""

from __future__ import annotations

import os
import pathlib
import tempfile
import unittest
from argparse import Namespace
from unittest import mock

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module(
        "local_ci.py",
        module_name="pulp_local_ci_desktop_source_request_integration",
        add_module_dir=True,
    )


class DesktopSourceRequestIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_desktop_source_request_manifest_and_command_edges(self) -> None:
        args = Namespace(
            source_mode="exact_sha",
            branch="feature/source",
            sha="abc123",
            prepare_command="  ./setup.sh --desktop  ",
            prepare_timeout=12,
        )
        request = self.mod.make_desktop_source_request(args)

        self.assertEqual(request["mode"], "exact-sha")
        self.assertEqual(request["prepare_command"], "./setup.sh --desktop")
        self.assertEqual(request["prepare_timeout_secs"], 12.0)
        self.assertEqual(
            self.mod.desktop_source_cache_key(request),
            self.mod.desktop_source_cache_key({**request, "mode": "live", "branch": "main"}),
        )
        self.assertNotEqual(
            self.mod.desktop_source_cache_key(request),
            self.mod.desktop_source_cache_key({**request, "prepare_command": "cmake --build build"}),
        )

        with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_HOME": str(self.root / "state")}, clear=True):
            source_root = self.mod.desktop_source_root("windows", request)
        self.assertEqual(source_root.parent, self.root / "state" / "desktop-source" / "windows")

        self.assertIsNone(self.mod._command_path_rewrite_candidate("/tmp/outside-tool"))
        self.assertIsNone(self.mod._command_path_rewrite_candidate("pulp-ui-preview"))
        self.assertEqual(
            self.mod._command_path_rewrite_candidate("./tools/local-ci/local_ci.py"),
            self.mod.ROOT / "tools" / "local-ci" / "local_ci.py",
        )
        self.assertIsNone(self.mod.rewrite_launch_command_for_source_root(None, self.root / "prepared"))
        self.assertEqual(
            self.mod.rewrite_launch_command_for_source_root('"unterminated', self.root / "prepared"),
            '"unterminated',
        )
        self.assertEqual(
            self.mod.rewrite_launch_command_for_posix_root("pulp-ui-preview --flag", "$HOME/source"),
            "pulp-ui-preview --flag",
        )

        local_command = f"'{self.mod.ROOT / 'tools' / 'local-ci' / 'local_ci.py'}' --json"
        rewritten_local = self.mod.rewrite_launch_command_for_source_root(local_command, self.root / "prepared source")
        rewritten_posix = self.mod.rewrite_launch_command_for_posix_root(local_command, "$HOME/prepared source")
        rewritten_windows = self.mod.rewrite_launch_command_for_windows_root(
            r".\tools\local-ci\local_ci.py --json",
            r"C:\Prepared Source",
        )
        self.assertIn(str(self.root / "prepared source" / "tools" / "local-ci" / "local_ci.py"), rewritten_local)
        self.assertIn("'$HOME/prepared source/tools/local-ci/local_ci.py'", rewritten_posix)
        self.assertIn(r'"C:\Prepared Source\tools\local-ci\local_ci.py" --json', rewritten_windows)

        commands = self.mod.split_windows_prepare_commands('echo "one;two"; cmake --build build\nctest -C Debug')
        self.assertEqual(commands, ['echo "one;two"', "cmake --build build", "ctest -C Debug"])
        self.mod.validate_windows_prepare_commands(['cmake -G "Visual Studio 17 2022"'])
        with self.assertRaisesRegex(ValueError, "single-quoted tokens"):
            self.mod.validate_windows_prepare_commands(["cmake -G 'Visual Studio 17 2022'"])

        manifest: dict = {}
        self.mod.attach_desktop_source_to_manifest(manifest, None)
        self.assertEqual(manifest, {})
        self.mod.attach_desktop_source_to_manifest(
            manifest,
            {
                "mode": "prepared",
                "branch": "feature/source",
                "sha": "abc123",
                "prepare_command": "./setup.sh",
                "prepare_timeout_secs": 12.0,
                "prepared_root": "/real/root",
                "prepared_root_display": "$STATE/root",
                "launch_cwd": "/real/root/examples",
                "launch_cwd_display": "$STATE/root/examples",
                "prepare_log": "prepare.log",
            },
        )
        self.assertEqual(manifest["source"]["mode"], "prepared")
        self.assertEqual(manifest["source"]["sha"], "abc123")
        self.assertEqual(manifest["source"]["prepared_root"], "$STATE/root")
        self.assertEqual(manifest["source"]["launch_cwd"], "$STATE/root/examples")
        self.assertEqual(manifest["artifacts"]["prepare_log"], "prepare.log")
        self.assertEqual(self.mod.slugify_token(" UI Preview / Smoke! "), "ui-preview-smoke")
        self.assertEqual(self.mod.slugify_token("!!!"), "run")
        self.assertEqual(len(self.mod.slugify_token("x" * 80, max_len=12)), 12)


if __name__ == "__main__":
    unittest.main()
