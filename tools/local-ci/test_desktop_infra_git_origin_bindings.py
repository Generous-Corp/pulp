#!/usr/bin/env python3
"""Tests for desktop git origin URL bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("desktop_infra_git_origin_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopInfraGitOriginBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_origin_exports_match_wrappers(self) -> None:
        expected = (
            "git_origin_http_url",
            "git_origin_clone_url",
        )

        self.assertEqual(self.mod.DESKTOP_INFRA_GIT_ORIGIN_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_origin_wrappers_bind_subprocess_runner(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        git_helpers = types.SimpleNamespace(
            git_origin_http_url=capture("origin_http", "https://origin/repo"),
            git_origin_clone_url=capture("origin_clone", "git@origin:repo.git"),
        )
        bindings = {
            "_git_helpers": git_helpers,
            "subprocess": types.SimpleNamespace(run=object()),
        }
        repo_root = Path("/repo")

        self.assertEqual(self.mod.git_origin_http_url(bindings, repo_root), "https://origin/repo")
        self.assertEqual(captured["origin_http"][0], (repo_root,))
        self.assertIs(captured["origin_http"][1]["run_fn"], bindings["subprocess"].run)
        self.assertEqual(self.mod.git_origin_clone_url(bindings, repo_root), "git@origin:repo.git")
        self.assertEqual(captured["origin_clone"][0], (repo_root,))
        self.assertIs(captured["origin_clone"][1]["run_fn"], bindings["subprocess"].run)

    def test_install_origin_helpers_wires_named_exports(self) -> None:
        git_helpers = types.SimpleNamespace(
            git_origin_http_url=lambda repo_root, *, run_fn: f"https:{repo_root}",
        )
        bindings = {
            "_git_helpers": git_helpers,
            "subprocess": types.SimpleNamespace(run=object()),
        }

        self.mod.install_desktop_infra_git_origin_helpers(bindings, ("git_origin_http_url",))

        self.assertEqual(bindings["git_origin_http_url"](Path("/repo")), "https:/repo")


if __name__ == "__main__":
    unittest.main()
