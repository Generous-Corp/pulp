#!/usr/bin/env python3
"""Tests for desktop git remote normalization bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("desktop_infra_git_remote_bindings.py")


class DesktopInfraGitRemoteBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_remote_exports_match_wrappers(self) -> None:
        expected = (
            "normalize_git_remote_for_http",
            "normalize_git_remote_for_clone",
        )

        self.assertEqual(self.mod.DESKTOP_INFRA_GIT_REMOTE_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_remote_wrappers_delegate_to_git_helpers(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        git_helpers = types.SimpleNamespace(
            normalize_git_remote_for_http=capture("http", "https://example/repo"),
            normalize_git_remote_for_clone=capture("clone", "git@example:repo.git"),
        )
        bindings = {"_git_helpers": git_helpers}

        self.assertEqual(self.mod.normalize_git_remote_for_http(bindings, "git@example:repo.git"), "https://example/repo")
        self.assertEqual(captured["http"][0], ("git@example:repo.git",))
        self.assertEqual(self.mod.normalize_git_remote_for_clone(bindings, "https://example/repo"), "git@example:repo.git")
        self.assertEqual(captured["clone"][0], ("https://example/repo",))

if __name__ == "__main__":
    unittest.main()
