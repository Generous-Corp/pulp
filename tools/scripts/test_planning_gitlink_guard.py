#!/usr/bin/env python3
"""Unit tests for planning_gitlink_guard.py.

The trailer tests patch gate_common.git_diff_names / git_range_trailers (and
the pin-ancestry check) in the guard's namespace. The ancestry tests build real
throwaway repositories: a planning repo with a main branch and a side commit,
and a superproject that pins one of them.
"""

from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "planning_gitlink_guard", HERE / "planning_gitlink_guard.py")
guard = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(guard)


def run(changed, trailers, mode="report", ancestry=("ok", "verified")):
    with mock.patch.object(guard, "git_diff_names", return_value=changed), \
         mock.patch.object(guard, "git_range_trailers", return_value=trailers), \
         mock.patch.object(guard, "check_pin_ancestry", return_value=ancestry):
        return guard.main(["--base", "origin/main", "--mode", mode])


class PlanningGitlinkGuardTest(unittest.TestCase):
    def test_no_planning_change_passes(self):
        self.assertEqual(run(["core/view/src/x.cpp", "CMakeLists.txt"], {}), 0)

    def test_planning_bump_without_trailer_fails(self):
        self.assertEqual(run(["planning", "core/view/src/x.cpp"], {}), 1)

    def test_planning_bump_only_without_trailer_fails(self):
        self.assertEqual(run(["planning"], {}), 1)

    def test_planning_bump_with_trailer_passes(self):
        trailers = {"planning-bump": ['reason="pin newer planning snapshot"']}
        self.assertEqual(run(["planning"], trailers), 0)

    def test_hint_mode_never_fails_even_on_unauthorized_bump(self):
        self.assertEqual(run(["planning"], {}, mode="hint"), 0)

    def test_trailer_key_is_case_insensitive(self):
        # gate_common lowercases trailer keys; the guard looks up "planning-bump".
        self.assertEqual(run(["planning"], {"planning-bump": ["reason=x"]}), 0)

    def test_unrelated_trailer_does_not_authorize(self):
        self.assertEqual(run(["planning"], {"version-bump": ["minor"]}), 1)

    def test_authorized_bump_still_fails_an_ancestry_failure(self):
        trailers = {"planning-bump": ['reason="x"']}
        self.assertEqual(run(["planning"], trailers, ancestry=("rejected", "off main")), 1)
        self.assertEqual(run(["planning"], trailers, ancestry=("unverified", "no fetch")), 1)
        self.assertEqual(run(["planning"], trailers, mode="hint",
                             ancestry=("rejected", "off main")), 0)


def git(cwd, *args):
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid",
               GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
    return subprocess.run(["git", "-C", str(cwd), *args], check=True, capture_output=True,
                          text=True, env=env).stdout.strip()


class PlanningPinAncestryTest(unittest.TestCase):
    """End-to-end: the real guard against real repositories."""

    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="planning-guard-test-")
        root = Path(self.scratch.name)
        self.planning = root / "planning-remote"
        self.planning.mkdir()
        git(self.planning, "init", "--quiet", "-b", "main")
        git(self.planning, "commit", "--quiet", "--allow-empty", "-m", "p1")
        self.base_pin = git(self.planning, "rev-parse", "HEAD")
        git(self.planning, "commit", "--quiet", "--allow-empty", "-m", "p2")
        self.main_pin = git(self.planning, "rev-parse", "HEAD")
        git(self.planning, "checkout", "--quiet", "-b", "side", self.base_pin)
        git(self.planning, "commit", "--quiet", "--allow-empty", "-m", "side")
        self.side_pin = git(self.planning, "rev-parse", "HEAD")
        git(self.planning, "checkout", "--quiet", "main")

        self.repo = root / "superproject"
        self.repo.mkdir()
        git(self.repo, "init", "--quiet", "-b", "main")
        (self.repo / ".gitmodules").write_text(
            f'[submodule "planning"]\n\tpath = planning\n\turl = {self.planning}\n')
        git(self.repo, "add", ".gitmodules")
        git(self.repo, "update-index", "--add", "--cacheinfo",
            f"160000,{self.base_pin},planning")
        git(self.repo, "commit", "--quiet", "-m", "base")
        git(self.repo, "branch", "base")

    def tearDown(self):
        self.scratch.cleanup()

    def pin(self, sha, trailer=True):
        git(self.repo, "update-index", "--cacheinfo", f"160000,{sha},planning")
        message = "bump planning"
        if trailer:
            message += '\n\nPlanning-Bump: reason="test"'
        git(self.repo, "commit", "--quiet", "-m", message)

    def run_guard(self, *extra):
        env = dict(os.environ)
        env.pop("PULP_PLANNING_REMOTE", None)
        return subprocess.run(
            [sys.executable, str(HERE / "planning_gitlink_guard.py"), "--base", "base",
             "--head", "HEAD", *extra],
            cwd=self.repo, capture_output=True, text=True, env=env)

    def test_pin_on_planning_main_passes(self):
        self.pin(self.main_pin)
        result = self.run_guard()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"pin {self.main_pin[:12]} is on main", result.stdout)

    def test_pin_off_planning_main_fails(self):
        self.pin(self.side_pin)
        result = self.run_guard()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn(f"the new planning pin {self.side_pin} is not an ancestor of",
                      result.stderr)

    def test_pin_unknown_to_planning_fails(self):
        self.pin("1" * 40)
        result = self.run_guard()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("is not an ancestor of", result.stderr)

    def test_unreachable_planning_remote_fails_closed(self):
        self.pin(self.main_pin)
        missing = Path(self.scratch.name) / "no-such-remote"
        result = self.run_guard("--planning-remote", str(missing))
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("planning-gitlink: NOT VERIFIED", result.stderr)
        self.assertIn("cannot fetch", result.stderr)

    def test_ci_warn_only_passes_an_unreachable_remote_with_a_warning(self):
        self.pin(self.main_pin)
        missing = Path(self.scratch.name) / "no-such-remote"
        result = self.run_guard("--planning-remote", str(missing), "--ci-warn-only")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("::warning title=planning-gitlink NOT VERIFIED::", result.stdout)
        self.assertIn("cannot fetch", result.stdout)

    def test_ci_warn_only_still_fails_a_pin_verified_off_main(self):
        self.pin(self.side_pin)
        result = self.run_guard("--ci-warn-only")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertNotIn("::warning", result.stdout)
        self.assertIn("is not an ancestor of", result.stderr)

    def test_ci_warn_only_still_passes_a_pin_on_main(self):
        self.pin(self.main_pin)
        result = self.run_guard("--ci-warn-only")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("::warning", result.stdout)

    def test_unauthorized_bump_fails_before_any_fetch(self):
        self.pin(self.main_pin, trailer=False)
        result = self.run_guard("--planning-remote", "/nonexistent")
        self.assertEqual(result.returncode, 1)
        self.assertIn("no\n  `Planning-Bump:` trailer", result.stderr)


if __name__ == "__main__":
    unittest.main()
