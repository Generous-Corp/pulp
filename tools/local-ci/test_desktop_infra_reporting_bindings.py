#!/usr/bin/env python3
"""Tests for desktop reporting infrastructure facade bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
from pathlib import Path
import types
import unittest



def load_module():
    return load_local_ci_module("desktop_infra_reporting_bindings.py")


class DesktopInfraReportingBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_reporting_exports_match_wrappers(self) -> None:
        expected = (
            "clear_directory_contents",
            "copy_directory_contents",
            "slugify_token",
        )

        self.assertEqual(self.mod.DESKTOP_INFRA_REPORTING_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_reporting_wrappers_delegate_arguments(self) -> None:
        captured = {}

        def capture(name, result=None):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        reporting = types.SimpleNamespace(
            clear_directory_contents=capture("clear"),
            copy_directory_contents=capture("copy"),
            slugify_token=capture("slug", "demo-token"),
        )
        bindings = {"_reporting": reporting}

        self.mod.clear_directory_contents(bindings, Path("/tmp/a"))
        self.assertEqual(captured["clear"][0], (Path("/tmp/a"),))
        self.mod.copy_directory_contents(bindings, Path("/tmp/a"), Path("/tmp/b"))
        self.assertEqual(captured["copy"][0], (Path("/tmp/a"), Path("/tmp/b")))
        self.assertEqual(self.mod.slugify_token(bindings, "Demo Token", max_len=12), "demo-token")
        self.assertEqual(captured["slug"][0], ("Demo Token",))
        self.assertEqual(captured["slug"][1], {"max_len": 12})

if __name__ == "__main__":
    unittest.main()
