#!/usr/bin/env python3
"""Fixture tests for apply_intent_bump.py — the merge-time half of the
intent-trailer version-bump model. Mirrors the temp-repo fixture used by
the other version-bump gate tests (gate_test_support.GateFixtureTestCase).

Runs standalone (`python3 tools/scripts/test_apply_intent_bump.py`) or as
part of the aggregate suite.
"""
from __future__ import annotations

import json
import subprocess
import unittest
from pathlib import Path

from gate_test_support import GateFixtureTestCase, _git

SCRIPT = Path(__file__).resolve().parent / "apply_intent_bump.py"


def _run_apply(repo: Path, head: str = "HEAD") -> tuple[int, str, str]:
    p = subprocess.run(
        ["python3", str(SCRIPT),
         "--head", head, "--base", "origin/main",
         "--config", str(repo / "tools/scripts/versioning.json"),
         "--repo-root", str(repo),
         "--print-edited"],
        cwd=repo, capture_output=True, text=True,
    )
    return p.returncode, p.stdout, p.stderr


def _cmake_version(repo: Path) -> str:
    txt = (repo / "CMakeLists.txt").read_text()
    import re
    return re.search(r"VERSION (\d+\.\d+\.\d+)", txt).group(1)


def _plugin_version(repo: Path) -> str:
    return json.loads((repo / ".claude-plugin/plugin.json").read_text())["version"]


class ApplyIntentBumpTests(GateFixtureTestCase):
    def _commit_with_message(self, message: str) -> None:
        # An intent-trailer PR touches NO version files; simulate a real merged
        # commit by making a trivial source change carrying the trailer.
        (self.tmp / "core/runtime/src/foo.cpp").write_text("int foo() { return 2; }\n")
        _git(self.tmp, "add", "-A")
        _git(self.tmp, "commit", "-q", "-m", message)

    def test_sdk_minor_intent_bumps_cmake(self) -> None:
        self.assertEqual(_cmake_version(self.tmp), "0.1.0")
        self._commit_with_message(
            "feat(core): add a thing\n\nVersion-Bump: sdk=minor reason=\"new API\"\n"
        )
        rc, out, err = _run_apply(self.tmp)
        self.assertEqual(rc, 0, err)
        self.assertEqual(_cmake_version(self.tmp), "0.2.0")
        self.assertIn("CMakeLists.txt", out)

    def test_plugin_patch_intent_bumps_plugin_json(self) -> None:
        self.assertEqual(_plugin_version(self.tmp), "0.1.0")
        self._commit_with_message(
            "fix(plugin): tweak\n\nVersion-Bump: plugin=patch\n"
        )
        rc, out, err = _run_apply(self.tmp)
        self.assertEqual(rc, 0, err)
        self.assertEqual(_plugin_version(self.tmp), "0.1.1")

    def test_no_trailer_is_a_noop(self) -> None:
        self._commit_with_message("docs: nothing to release\n")
        rc, out, err = _run_apply(self.tmp)
        self.assertEqual(rc, 0, err)
        self.assertEqual(_cmake_version(self.tmp), "0.1.0")
        self.assertEqual(out.strip(), "")

    def test_skip_trailer_is_a_noop(self) -> None:
        self._commit_with_message(
            "chore: infra\n\nVersion-Bump: skip reason=\"no release owed\"\n"
        )
        rc, out, err = _run_apply(self.tmp)
        self.assertEqual(rc, 0, err)
        self.assertEqual(_cmake_version(self.tmp), "0.1.0")

    def test_unknown_surface_is_skipped_not_fatal(self) -> None:
        self._commit_with_message(
            "feat: x\n\nVersion-Bump: bogus=minor\n"
        )
        rc, out, err = _run_apply(self.tmp)
        self.assertEqual(rc, 0, err)
        self.assertEqual(_cmake_version(self.tmp), "0.1.0")
        self.assertIn("unknown surface", err)

    def test_idempotent_rerun_does_not_double_bump(self) -> None:
        self._commit_with_message(
            "feat(core): add a thing\n\nVersion-Bump: sdk=minor\n"
        )
        rc, _, err = _run_apply(self.tmp)
        self.assertEqual(rc, 0, err)
        self.assertEqual(_cmake_version(self.tmp), "0.2.0")
        # The trailer commit is the workflow's --head. Commit the applied bump,
        # then REPLAY the workflow against that same trailer commit: the surface
        # is already past origin/main, so it must NOT bump again.
        trailer_sha = subprocess.run(
            ["git", "-C", str(self.tmp), "rev-parse", "HEAD"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
        _git(self.tmp, "commit", "-aqm", "chore: bump versions")
        rc2, out2, err2 = _run_apply(self.tmp, head=trailer_sha)
        self.assertEqual(rc2, 0, err2)
        self.assertEqual(_cmake_version(self.tmp), "0.2.0")
        self.assertIn("already bumped", err2)

    def test_two_surfaces_in_one_commit(self) -> None:
        self._commit_with_message(
            "feat: spanning change\n\n"
            "Version-Bump: sdk=major\nVersion-Bump: plugin=minor\n"
        )
        rc, out, err = _run_apply(self.tmp)
        self.assertEqual(rc, 0, err)
        self.assertEqual(_cmake_version(self.tmp), "1.0.0")
        self.assertEqual(_plugin_version(self.tmp), "0.2.0")


if __name__ == "__main__":
    unittest.main()
