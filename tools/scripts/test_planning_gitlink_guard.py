#!/usr/bin/env python3
"""Unit tests for planning_gitlink_guard.py.

The guard's git access goes through gate_common.git_diff_names /
git_range_trailers, which are imported into the guard's namespace — so the
tests patch those two names directly and never touch a real repo.
"""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "planning_gitlink_guard", HERE / "planning_gitlink_guard.py")
guard = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(guard)


def run(changed, trailers, mode="report"):
    with mock.patch.object(guard, "git_diff_names", return_value=changed), \
         mock.patch.object(guard, "git_range_trailers", return_value=trailers):
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


if __name__ == "__main__":
    unittest.main()
