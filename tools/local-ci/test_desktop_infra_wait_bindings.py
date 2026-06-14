#!/usr/bin/env python3
"""Tests for desktop wait infrastructure facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("desktop_infra_wait_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopInfraWaitBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_wait_exports_match_wrappers(self) -> None:
        expected = ("wait_for_path",)

        self.assertEqual(self.mod.DESKTOP_INFRA_WAIT_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_wait_wrapper_delegates_arguments(self) -> None:
        captured = {}

        def wait_for_path(*args, **kwargs):
            captured["wait"] = (args, kwargs)
            return Path("/tmp/file")

        bindings = {"_io_utils": types.SimpleNamespace(wait_for_path=wait_for_path)}

        self.assertEqual(self.mod.wait_for_path(bindings, Path("/tmp/file"), 3.0), Path("/tmp/file"))
        self.assertEqual(captured["wait"][0], (Path("/tmp/file"), 3.0))

if __name__ == "__main__":
    unittest.main()
