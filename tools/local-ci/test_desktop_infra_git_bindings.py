#!/usr/bin/env python3
"""Tests for desktop git infrastructure facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("desktop_infra_git_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopInfraGitBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_git_exports_match_wrappers(self) -> None:
        expected = (
            "normalize_git_remote_for_http",
            "normalize_git_remote_for_clone",
            "git_origin_http_url",
            "git_origin_clone_url",
            "run_git",
        )

        self.assertEqual(self.mod.DESKTOP_INFRA_GIT_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_git_wrappers_bind_subprocess_runner(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        git_helpers = types.SimpleNamespace(
            normalize_git_remote_for_http=capture("http", "https://example/repo"),
            normalize_git_remote_for_clone=capture("clone", "git@example:repo.git"),
            git_origin_http_url=capture("origin_http", "https://origin/repo"),
            git_origin_clone_url=capture("origin_clone", "git@origin:repo.git"),
            run_git=capture("run_git", types.SimpleNamespace(returncode=0)),
        )
        bindings = {
            "_git_helpers": git_helpers,
            "subprocess": types.SimpleNamespace(run=object()),
        }
        repo_root = Path("/repo")

        self.assertEqual(self.mod.normalize_git_remote_for_http(bindings, "git@example:repo.git"), "https://example/repo")
        self.assertEqual(captured["http"][0], ("git@example:repo.git",))
        self.assertEqual(self.mod.normalize_git_remote_for_clone(bindings, "https://example/repo"), "git@example:repo.git")
        self.assertEqual(captured["clone"][0], ("https://example/repo",))
        self.assertEqual(self.mod.git_origin_http_url(bindings, repo_root), "https://origin/repo")
        self.assertIs(captured["origin_http"][1]["run_fn"], bindings["subprocess"].run)
        self.assertEqual(self.mod.git_origin_clone_url(bindings, repo_root), "git@origin:repo.git")
        self.assertIs(captured["origin_clone"][1]["run_fn"], bindings["subprocess"].run)
        self.assertEqual(self.mod.run_git(bindings, ["status"], cwd=repo_root, check=False).returncode, 0)
        self.assertEqual(captured["run_git"][0], (["status"],))
        self.assertEqual(captured["run_git"][1]["cwd"], repo_root)
        self.assertEqual(captured["run_git"][1]["check"], False)
        self.assertIs(captured["run_git"][1]["run_fn"], bindings["subprocess"].run)

    def test_install_desktop_infra_git_helpers_wires_named_exports(self) -> None:
        git_helpers = types.SimpleNamespace(
            normalize_git_remote_for_http=lambda remote_url: f"https:{remote_url}",
        )
        bindings = {
            "_git_helpers": git_helpers,
            "subprocess": types.SimpleNamespace(run=object()),
        }

        self.mod.install_desktop_infra_git_helpers(bindings, ("normalize_git_remote_for_http",))

        self.assertEqual(bindings["normalize_git_remote_for_http"]("example/repo"), "https:example/repo")


if __name__ == "__main__":
    unittest.main()
