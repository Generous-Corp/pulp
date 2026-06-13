#!/usr/bin/env python3
"""Binding tests for local-ci git helper facades."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("git_helpers_bindings.py")
git_helpers_bindings = load_module_from_path(MODULE_PATH, module_name="git_helpers_bindings", add_module_dir=True)


class FakeGitHelpers:
    def __init__(self) -> None:
        self.calls: list[tuple] = []

    def now_iso(self):
        self.calls.append(("now_iso",))
        return "now"

    def current_branch(self):
        self.calls.append(("current_branch",))
        return "branch"

    def current_sha(self):
        self.calls.append(("current_sha",))
        return "sha"

    def git_root_for(self, path):
        self.calls.append(("git_root_for", path))
        return path

    def resolve_git_ref_sha(self, ref):
        self.calls.append(("resolve_git_ref_sha", ref))
        return "resolved"

    def short_sha(self, sha):
        self.calls.append(("short_sha", sha))
        return "short"


class GitHelpersBindingTests(unittest.TestCase):
    def test_git_helpers_bindings_delegate_to_bound_module(self) -> None:
        fake = FakeGitHelpers()
        bindings = {"_git_helpers": fake}
        path = Path("repo")

        self.assertEqual(git_helpers_bindings.now_iso(bindings), "now")
        self.assertEqual(git_helpers_bindings.current_branch(bindings), "branch")
        self.assertEqual(git_helpers_bindings.current_sha(bindings), "sha")
        self.assertEqual(git_helpers_bindings.git_root_for(bindings, path), path)
        self.assertEqual(git_helpers_bindings.resolve_git_ref_sha(bindings, "HEAD"), "resolved")
        self.assertEqual(git_helpers_bindings.short_sha(bindings, "abcdef"), "short")
        self.assertEqual(
            fake.calls,
            [
                ("now_iso",),
                ("current_branch",),
                ("current_sha",),
                ("git_root_for", path),
                ("resolve_git_ref_sha", "HEAD"),
                ("short_sha", "abcdef"),
            ],
        )

    def test_install_git_helpers_wires_named_exports(self) -> None:
        fake = FakeGitHelpers()
        bindings = {"_git_helpers": fake}

        git_helpers_bindings.install_git_helpers(bindings, ("now_iso", "short_sha"))

        self.assertEqual(bindings["now_iso"](), "now")
        self.assertEqual(bindings["short_sha"]("abcdef"), "short")
        self.assertEqual(bindings["now_iso"].__name__, "now_iso")
        self.assertEqual([call[0] for call in fake.calls], ["now_iso", "short_sha"])

    def test_install_git_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        git_helpers_bindings.future_git_helper = lambda _bindings: "future"

        git_helpers_bindings.install_git_helpers(bindings, ("future_git_helper",))

        self.assertEqual(bindings["future_git_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
