#!/usr/bin/env python3
"""Tests for tools/ci/checkout_location_guard.py."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import checkout_location_guard as guard  # noqa: E402

REPO_ROOT = HERE.parent.parent


def no_ccache(_argv):
    return None


class TempCheckoutTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.env = {"TMPDIR": str(self.tmp)}

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_control_the_repository_is_not_a_temporary_checkout(self) -> None:
        # Every "allowed" case below depends on this being true; if the test
        # tree itself sat in a temp dir they would pass for the wrong reason.
        self.assertIsNone(guard.temp_root_containing(REPO_ROOT, self.env))

    def test_checkout_under_tmpdir_is_refused(self) -> None:
        checkout = self.tmp / "wt"
        checkout.mkdir()
        status, messages = guard.evaluate(checkout, self.env, "ctx", no_ccache)
        self.assertEqual(status, guard.REFUSED_EXIT)
        self.assertIn("refusing to build a Pulp checkout in a temporary directory", messages[0])
        self.assertIn(guard.ALLOW_ENV, messages[0])

    def test_checkout_under_fixed_tmp_is_refused_even_without_tmpdir(self) -> None:
        with tempfile.TemporaryDirectory(dir="/tmp") as raw:
            status, _ = guard.evaluate(Path(raw), {}, "ctx", no_ccache)
        self.assertEqual(status, guard.REFUSED_EXIT)

    def test_refusal_names_the_declared_worktrees_root(self) -> None:
        env = {**self.env, "PULP_WORKTREES_ROOT": "/Volumes/Somewhere/agent-worktrees"}
        _, messages = guard.evaluate(self.tmp, env, "ctx", no_ccache)
        self.assertIn("/Volumes/Somewhere/agent-worktrees", messages[0])
        self.assertIn("PULP_WORKTREES_ROOT", messages[0])

    def test_non_temporary_checkout_is_allowed(self) -> None:
        status, messages = guard.evaluate(REPO_ROOT, self.env, "ctx", no_ccache)
        self.assertEqual((status, messages), (0, []))

    def test_override_allows_with_a_note(self) -> None:
        env = {**self.env, guard.ALLOW_ENV: "1"}
        status, messages = guard.evaluate(self.tmp, env, "ctx", no_ccache)
        self.assertEqual(status, 0)
        self.assertIn(guard.ALLOW_ENV, messages[0])

    def test_github_actions_jobs_are_exempt(self) -> None:
        env = {**self.env, "GITHUB_ACTIONS": "true"}
        self.assertEqual(guard.evaluate(self.tmp, env, "ctx", no_ccache), (0, []))

    def test_a_root_tmpdir_does_not_refuse_everything(self) -> None:
        self.assertEqual(guard.evaluate(REPO_ROOT, {"TMPDIR": "/"}, "ctx", no_ccache)[0], 0)

    def test_cli_exit_status_is_the_refusal(self) -> None:
        env = {k: v for k, v in os.environ.items() if k not in ("GITHUB_ACTIONS", guard.ALLOW_ENV)}
        env["TMPDIR"] = str(self.tmp)
        result = subprocess.run(
            [sys.executable, str(HERE / "checkout_location_guard.py"), str(self.tmp)],
            env=env, capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, guard.REFUSED_EXIT, result.stderr)
        self.assertIn("temporary directory", result.stderr)


class CcacheBaseDirTests(unittest.TestCase):
    def test_outside_base_dir_warns(self) -> None:
        with tempfile.TemporaryDirectory() as other:
            status, messages = guard.evaluate(
                REPO_ROOT, {}, "ctx", lambda _argv: other
            )
        self.assertEqual(status, 0)
        self.assertEqual(len(messages), 1)
        self.assertIn("outside ccache base_dir", messages[0])

    def test_inside_base_dir_is_silent(self) -> None:
        base = str(REPO_ROOT.parent)
        self.assertEqual(guard.evaluate(REPO_ROOT, {}, "ctx", lambda _argv: base), (0, []))

    def test_unset_base_dir_is_silent(self) -> None:
        self.assertEqual(guard.evaluate(REPO_ROOT, {}, "ctx", lambda _argv: ""), (0, []))


@unittest.skipIf(shutil.which("cmake") is None or sys.platform == "win32", "needs cmake on POSIX")
class ConfigureHookTests(unittest.TestCase):
    """The root CMake configure applies the same refusal."""

    def _configure(self, source: Path, **extra: str) -> subprocess.CompletedProcess:
        env = {k: v for k, v in os.environ.items()
               if k not in ("GITHUB_ACTIONS", guard.ALLOW_ENV)}
        env["TMPDIR"] = tempfile.gettempdir()
        env.update(extra)
        return subprocess.run(
            ["cmake", "-P", str(source / "tools" / "cmake" / "PulpCheckoutLocation.cmake")],
            cwd=source, env=env, capture_output=True, text=True, check=False, timeout=60,
        )

    def _copy(self, root: Path) -> Path:
        (root / "tools" / "ci").mkdir(parents=True)
        (root / "tools" / "cmake").mkdir(parents=True)
        shutil.copy2(HERE / "checkout_location_guard.py", root / "tools" / "ci")
        shutil.copy2(REPO_ROOT / "tools" / "cmake" / "PulpCheckoutLocation.cmake",
                     root / "tools" / "cmake")
        return root

    def test_control_the_real_checkout_configures(self) -> None:
        result = self._configure(REPO_ROOT)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_temporary_checkout_fails_configure(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            result = self._configure(self._copy(Path(raw)))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("temporary directory", result.stderr)

    def test_override_lets_a_temporary_checkout_configure(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            result = self._configure(self._copy(Path(raw)), **{guard.ALLOW_ENV: "1"})
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
