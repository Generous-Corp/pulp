#!/usr/bin/env python3
"""No-network tests for desktop source manifest attachment."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_source_request_manifest.py")


class DesktopSourceRequestManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_attach_desktop_source_to_manifest_prefers_display_paths_and_prepare_log(self) -> None:
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

    def test_attach_desktop_source_to_manifest_ignores_missing_context(self) -> None:
        manifest = {"status": "pass"}
        self.mod.attach_desktop_source_to_manifest(manifest, None)
        self.assertEqual(manifest, {"status": "pass"})


if __name__ == "__main__":
    unittest.main()
