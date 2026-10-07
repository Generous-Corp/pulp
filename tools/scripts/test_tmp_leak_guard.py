#!/usr/bin/env python3
"""Tests for tmp_leak_guard.py: a leak fails, a clean run passes, nothing stays."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tmp_leak_guard  # noqa: E402

GUARD = Path(tmp_leak_guard.__file__)


class TmpLeakGuardTest(unittest.TestCase):
    def setUp(self) -> None:
        # The guard makes its private directory under this test's own temp
        # directory, so the assertions can see every entry it left.
        self.outer = tempfile.TemporaryDirectory(prefix="pulp-tmp-leak-guard-test-")
        self.addCleanup(self.outer.cleanup)
        self.env = dict(os.environ, TMPDIR=self.outer.name)

    def guard(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(GUARD), *args], env=self.env,
                              text=True, capture_output=True, timeout=60)

    def python(self, code: str) -> list[str]:
        return ["--", sys.executable, "-c", code]

    def assert_guard_removed_its_directory(self) -> None:
        self.assertEqual(os.listdir(self.outer.name), [])

    def test_a_command_that_leaves_scratch_fails_and_names_it(self) -> None:
        result = self.guard(*self.python(
            "import tempfile; tempfile.mkdtemp(prefix='pulp-leaky-')"))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("left 1 entry", result.stderr)
        self.assertIn("pulp-leaky-", result.stderr)
        self.assert_guard_removed_its_directory()

    def test_a_command_that_cleans_up_passes(self) -> None:
        result = self.guard(*self.python(
            "import shutil, tempfile; shutil.rmtree(tempfile.mkdtemp(prefix='pulp-tidy-'))"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assert_guard_removed_its_directory()

    def test_the_command_sees_the_private_directory_not_the_shared_one(self) -> None:
        result = self.guard(*self.python(
            "import os, tempfile;"
            "assert os.environ['TMPDIR'] == os.environ['TMP'] == os.environ['TEMP'];"
            "assert os.path.basename(tempfile.gettempdir()).startswith('pulp-tmp-leak-guard-')"))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_failing_command_keeps_its_exit_code_and_still_reports(self) -> None:
        result = self.guard(*self.python(
            "import sys, tempfile; tempfile.mkdtemp(prefix='pulp-leaky-'); sys.exit(7)"))
        self.assertEqual(result.returncode, 7)
        self.assertIn("pulp-leaky-", result.stderr)
        self.assert_guard_removed_its_directory()

    def test_ignored_entries_are_not_leaks(self) -> None:
        result = self.guard("--ignore", "runtime-owned-*", *self.python(
            "import tempfile; tempfile.mkdtemp(prefix='runtime-owned-')"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_guard_removed_its_directory()

    def test_read_only_leftovers_are_still_removed(self) -> None:
        result = self.guard(*self.python(
            "import os, tempfile; d = tempfile.mkdtemp(prefix='pulp-ro-');"
            "open(os.path.join(d, 'f'), 'w').close(); os.chmod(d, 0o500)"))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assert_guard_removed_its_directory()

    def test_no_command_is_a_usage_error(self) -> None:
        result = self.guard("--")
        self.assertEqual(result.returncode, 2)
        self.assert_guard_removed_its_directory()


if __name__ == "__main__":
    unittest.main()
