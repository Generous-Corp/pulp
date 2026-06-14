#!/usr/bin/env python3
"""Binding tests for local-ci time helper facades."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("git_time_helpers_bindings.py")
git_time_helpers_bindings = load_module_from_path(
    MODULE_PATH,
    module_name="git_time_helpers_bindings",
    add_module_dir=True,
)


class FakeGitHelpers:
    def __init__(self) -> None:
        self.calls: list[tuple] = []

    def now_iso(self):
        self.calls.append(("now_iso",))
        return "now"


class GitTimeHelpersBindingTests(unittest.TestCase):
    def test_time_binding_delegates_to_bound_module(self) -> None:
        fake = FakeGitHelpers()
        bindings = {"_git_helpers": fake}

        self.assertEqual(git_time_helpers_bindings.now_iso(bindings), "now")
        self.assertEqual(fake.calls, [("now_iso",)])

    def test_install_git_time_helpers_wires_named_exports(self) -> None:
        fake = FakeGitHelpers()
        bindings = {"_git_helpers": fake}

        git_time_helpers_bindings.install_git_time_helpers(bindings)

        self.assertEqual(bindings["now_iso"](), "now")
        self.assertEqual(bindings["now_iso"].__name__, "now_iso")


if __name__ == "__main__":
    unittest.main()
