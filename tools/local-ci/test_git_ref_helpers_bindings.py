#!/usr/bin/env python3
"""Binding tests for local-ci git ref helper facades."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("git_ref_helpers_bindings.py")
git_ref_helpers_bindings = load_module_from_path(
    MODULE_PATH,
    module_name="git_ref_helpers_bindings",
    add_module_dir=True,
)


class FakeGitHelpers:
    def __init__(self) -> None:
        self.calls: list[tuple] = []

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


class GitRefHelpersBindingTests(unittest.TestCase):
    def test_ref_bindings_delegate_to_bound_module(self) -> None:
        fake = FakeGitHelpers()
        bindings = {"_git_helpers": fake}
        path = Path("repo")

        self.assertEqual(git_ref_helpers_bindings.current_branch(bindings), "branch")
        self.assertEqual(git_ref_helpers_bindings.current_sha(bindings), "sha")
        self.assertEqual(git_ref_helpers_bindings.git_root_for(bindings, path), path)
        self.assertEqual(git_ref_helpers_bindings.resolve_git_ref_sha(bindings, "HEAD"), "resolved")
        self.assertEqual(git_ref_helpers_bindings.short_sha(bindings, "abcdef"), "short")
        self.assertEqual(
            fake.calls,
            [
                ("current_branch",),
                ("current_sha",),
                ("git_root_for", path),
                ("resolve_git_ref_sha", "HEAD"),
                ("short_sha", "abcdef"),
            ],
        )

    def test_install_git_ref_helpers_wires_named_exports(self) -> None:
        fake = FakeGitHelpers()
        bindings = {"_git_helpers": fake}

        git_ref_helpers_bindings.install_git_ref_helpers(bindings, ("current_branch", "short_sha"))

        self.assertEqual(bindings["current_branch"](), "branch")
        self.assertEqual(bindings["short_sha"]("abcdef"), "short")
        self.assertEqual(bindings["current_branch"].__name__, "current_branch")
        self.assertEqual([call[0] for call in fake.calls], ["current_branch", "short_sha"])


if __name__ == "__main__":
    unittest.main()
