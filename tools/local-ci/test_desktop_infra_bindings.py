#!/usr/bin/env python3
"""Tests for desktop infrastructure facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("desktop_infra_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopInfraBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, *, git_helpers=None, reporting=None, io_utils=None):
        return {
            "_git_helpers": git_helpers or types.SimpleNamespace(),
            "_reporting": reporting or types.SimpleNamespace(),
            "_io_utils": io_utils or types.SimpleNamespace(),
            "subprocess": types.SimpleNamespace(run=object()),
        }

    def test_desktop_infra_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.DESKTOP_INFRA_GIT_EXPORTS,
            *self.mod.DESKTOP_INFRA_REPORTING_EXPORTS,
            *self.mod.DESKTOP_INFRA_WAIT_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_INFRA_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

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
        bindings = self._bindings(git_helpers=git_helpers)
        repo_root = Path("/repo")

        self.assertEqual(self.mod.normalize_git_remote_for_http(bindings, "git@example:repo.git"), "https://example/repo")
        self.assertEqual(captured["http"][0], ("git@example:repo.git",))
        self.assertEqual(self.mod.normalize_git_remote_for_clone(bindings, "https://example/repo"), "git@example:repo.git")
        self.assertEqual(captured["clone"][0], ("https://example/repo",))
        self.assertEqual(self.mod.git_origin_http_url(bindings, repo_root), "https://origin/repo")
        self.assertEqual(captured["origin_http"][0], (repo_root,))
        self.assertIs(captured["origin_http"][1]["run_fn"], bindings["subprocess"].run)
        self.assertEqual(self.mod.git_origin_clone_url(bindings, repo_root), "git@origin:repo.git")
        self.assertIs(captured["origin_clone"][1]["run_fn"], bindings["subprocess"].run)
        self.assertEqual(self.mod.run_git(bindings, ["status"], cwd=repo_root, check=False).returncode, 0)
        self.assertEqual(captured["run_git"][0], (["status"],))
        self.assertEqual(captured["run_git"][1]["cwd"], repo_root)
        self.assertEqual(captured["run_git"][1]["check"], False)
        self.assertIs(captured["run_git"][1]["run_fn"], bindings["subprocess"].run)

    def test_reporting_and_io_wrappers_delegate_arguments(self) -> None:
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
        io_utils = types.SimpleNamespace(wait_for_path=capture("wait", Path("/tmp/file")))
        bindings = self._bindings(reporting=reporting, io_utils=io_utils)

        self.mod.clear_directory_contents(bindings, Path("/tmp/a"))
        self.assertEqual(captured["clear"][0], (Path("/tmp/a"),))
        self.mod.copy_directory_contents(bindings, Path("/tmp/a"), Path("/tmp/b"))
        self.assertEqual(captured["copy"][0], (Path("/tmp/a"), Path("/tmp/b")))
        self.assertEqual(self.mod.slugify_token(bindings, "Demo Token", max_len=12), "demo-token")
        self.assertEqual(captured["slug"][0], ("Demo Token",))
        self.assertEqual(captured["slug"][1], {"max_len": 12})
        self.assertEqual(self.mod.wait_for_path(bindings, Path("/tmp/file"), 3.0), Path("/tmp/file"))
        self.assertEqual(captured["wait"][0], (Path("/tmp/file"), 3.0))

    def test_install_desktop_infra_helpers_wires_named_exports(self) -> None:
        git_helpers = types.SimpleNamespace(
            normalize_git_remote_for_http=lambda remote_url: f"https:{remote_url}",
        )
        reporting = types.SimpleNamespace(
            slugify_token=lambda value, *, max_len=48: value[:max_len].lower(),
        )
        io_utils = types.SimpleNamespace(wait_for_path=lambda path, timeout_secs: path)
        bindings = self._bindings(git_helpers=git_helpers, reporting=reporting, io_utils=io_utils)

        self.mod.install_desktop_infra_helpers(
            bindings,
            ("normalize_git_remote_for_http", "slugify_token", "wait_for_path"),
        )

        self.assertEqual(bindings["normalize_git_remote_for_http"]("example/repo"), "https:example/repo")
        self.assertEqual(bindings["slugify_token"]("Demo Token", max_len=4), "demo")
        self.assertEqual(bindings["wait_for_path"](Path("/tmp/file"), 3.0), Path("/tmp/file"))

    def test_install_desktop_infra_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_desktop_infra_helper = lambda _bindings: "future"

        self.mod.install_desktop_infra_helpers(bindings, ("future_desktop_infra_helper",))

        self.assertEqual(bindings["future_desktop_infra_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
