#!/usr/bin/env python3
"""No-network tests for desktop source request construction."""

from __future__ import annotations

import argparse
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_source_request_core.py")


class DesktopSourceRequestCoreTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_make_desktop_source_request_normalizes_defaults_and_prepare_command(self) -> None:
        request = self.mod.make_desktop_source_request(
            argparse.Namespace(
                source_mode="EXACT-SHA",
                branch=None,
                sha=None,
                prepare_command="  ./scripts/build-ui.sh  ",
                prepare_timeout=42,
            ),
            normalize_desktop_source_mode_fn=lambda value: str(value).lower(),
            current_branch_fn=lambda: "main",
            current_sha_fn=lambda: "b" * 40,
        )

        self.assertEqual(
            request,
            {
                "mode": "exact-sha",
                "branch": "main",
                "sha": "b" * 40,
                "prepare_command": "./scripts/build-ui.sh",
                "prepare_timeout_secs": 42.0,
            },
        )

    def test_make_desktop_source_request_uses_explicit_branch_sha_and_timeout_default(self) -> None:
        request = self.mod.make_desktop_source_request(
            argparse.Namespace(
                source_mode=None,
                branch="feature/source",
                sha="c" * 40,
                prepare_command=" ",
                prepare_timeout=None,
            ),
            normalize_desktop_source_mode_fn=lambda value: value or "live",
            current_branch_fn=lambda: "main",
            current_sha_fn=lambda: "b" * 40,
        )

        self.assertEqual(request["mode"], "live")
        self.assertEqual(request["branch"], "feature/source")
        self.assertEqual(request["sha"], "c" * 40)
        self.assertIsNone(request["prepare_command"])
        self.assertEqual(request["prepare_timeout_secs"], 900.0)


if __name__ == "__main__":
    unittest.main()
