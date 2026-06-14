#!/usr/bin/env python3
"""No-network tests for Windows desktop source prepare-command helpers."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_source_request_windows.py")


class DesktopSourceRequestWindowsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_split_windows_prepare_commands_respects_quotes(self) -> None:
        self.assertEqual(
            self.mod.split_windows_prepare_commands('cmake -G "Visual Studio; 17"; echo ok\nninja'),
            ['cmake -G "Visual Studio; 17"', "echo ok", "ninja"],
        )

    def test_validate_windows_prepare_commands_rejects_single_quoted_tokens(self) -> None:
        with self.assertRaisesRegex(ValueError, "Use double quotes"):
            self.mod.validate_windows_prepare_commands(["cmake -G 'Ninja'"])

        self.mod.validate_windows_prepare_commands(['cmake -G "Ninja"', "echo ok"])


if __name__ == "__main__":
    unittest.main()
