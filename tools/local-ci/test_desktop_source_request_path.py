#!/usr/bin/env python3
"""No-network tests for desktop source cache path helpers."""

from __future__ import annotations

import pathlib
import tempfile
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_source_request_path.py")


class DesktopSourceRequestPathTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_cache_key_uses_sha_and_prepare_command_only(self) -> None:
        request = {
            "mode": "exact-sha",
            "branch": "main",
            "sha": "a" * 40,
            "prepare_command": "./build.sh",
        }
        same_material = {**request, "mode": "live", "branch": "other"}
        changed_prepare = {**request, "prepare_command": "cmake --build build"}

        self.assertEqual(self.mod.desktop_source_cache_key(request), self.mod.desktop_source_cache_key(same_material))
        self.assertNotEqual(
            self.mod.desktop_source_cache_key(request),
            self.mod.desktop_source_cache_key(changed_prepare),
        )

    def test_desktop_source_root_uses_target_cache_key_under_state_dir(self) -> None:
        request = {"sha": "a" * 40, "prepare_command": None}
        source_root = self.mod.desktop_source_root("mac", request, state_dir_fn=lambda: self.root / "state")

        self.assertEqual(source_root.parent, self.root / "state" / "desktop-source" / "mac")
        self.assertEqual(len(source_root.name), 12)


if __name__ == "__main__":
    unittest.main()
