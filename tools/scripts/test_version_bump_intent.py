#!/usr/bin/env python3
"""Fixture tests for the intent-trailer model in version_bump_check.py.

Two surfaces:

  * ``--accept-intent-trailers`` — the PR-side gate accepts a
    ``Version-Bump: <surface>=<level>`` trailer in lieu of an actual file
    bump (what replaces ``--require-bump-for-fix-feat`` when no PR carries a
    bump marker), and

  * ``classify-unreleased-range`` with ``PULP_ACCEPT_INTENT_TRAILERS=1`` —
    the auto-release stranded-fix detector treats a pending authored intent
    as COVERED (not "consumers are stuck"), but only for intent on the PR's
    own non-merge commits.

Runs standalone or via the aggregate suite (`test_gates.py`).
"""
from __future__ import annotations

import os
import subprocess
import unittest

from gate_test_support import GateFixtureTestCase, VBC, _run, _git


def _feat_core_commit_with_intent(f, subject_and_trailer: str) -> None:
    """A user-facing core change that declares intent but edits NO version
    file — the intent-trailer PR shape."""
    f.write("core/runtime/src/foo.cpp", "int foo() { return 42; }\n")
    _git(f.root, "add", "--", "core/runtime/src/foo.cpp")
    _git(f.root, "commit", "-q", "-m", subject_and_trailer)


class AcceptIntentTrailersGateTests(GateFixtureTestCase):
    """The PR-side gate: intent trailer stands in for a file bump."""

    def _run_report(self, extra: list[str]) -> tuple[int, str]:
        return _run(
            ["python3", str(VBC), "--base", "origin/main",
             "--config", str(self.tmp / "tools/scripts/versioning.json"),
             "--mode=report", *extra],
            cwd=self.tmp,
        )

    def test_intent_trailer_accepted_in_lieu_of_bump(self) -> None:
        _feat_core_commit_with_intent(
            self.f, "feat(core): new API\n\nVersion-Bump: sdk=minor\n")
        rc, out = self._run_report(["--accept-intent-trailers"])
        self.assertEqual(rc, 0, out)
        self.assertIn("intent declared", out)

    def test_without_flag_same_change_requires_a_bump(self) -> None:
        # The killer: identical commit, no --accept-intent-trailers → the
        # touched surface still demands an actual file bump.
        _feat_core_commit_with_intent(
            self.f, "feat(core): new API\n\nVersion-Bump: sdk=minor\n")
        rc, out = self._run_report([])
        self.assertEqual(rc, 1, out)
        self.assertIn("bump required", out)

    def test_touched_surface_with_no_intent_still_fails(self) -> None:
        # "declare an intent OR skip" — a touched surface with neither is a
        # hard fail even with the flag on.
        self.f.write("core/runtime/src/foo.cpp", "int foo(){return 7;}\n")
        _git(self.f.root, "add", "--", "core/runtime/src/foo.cpp")
        _git(self.f.root, "commit", "-q", "-m", "feat(core): thing, no intent")
        rc, out = self._run_report(["--accept-intent-trailers"])
        self.assertEqual(rc, 1, out)


class StrandedDetectorIntentPendingTests(GateFixtureTestCase):
    """The post-merge stranded-fix detector: pending intent is not stranded."""

    def _classify(self, *, accept_intent: bool) -> tuple[int, str]:
        env = os.environ.copy()
        if accept_intent:
            env["PULP_ACCEPT_INTENT_TRAILERS"] = "1"
        else:
            env.pop("PULP_ACCEPT_INTENT_TRAILERS", None)
        p = subprocess.run(
            ["python3", str(VBC), "classify-unreleased-range",
             "origin/main", "HEAD", "0", "0",
             str(self.tmp / "tools/scripts/versioning.json")],
            cwd=self.tmp, capture_output=True, text=True, env=env,
        )
        return p.returncode, p.stdout + p.stderr

    def test_intent_pending_is_not_stranded_when_model_live(self) -> None:
        _feat_core_commit_with_intent(
            self.f, "feat(core): new API\n\nVersion-Bump: sdk=minor\n")
        # Model OFF (today): a fix/feat with no bump IS stranded (exit 0 = found).
        rc_off, _ = self._classify(accept_intent=False)
        self.assertEqual(rc_off, 0)
        # Model ON: pending intent → not stranded (exit 1 = nothing to warn).
        rc_on, out_on = self._classify(accept_intent=True)
        self.assertEqual(rc_on, 1, out_on)

    def test_stray_intent_on_merge_commit_still_strands(self) -> None:
        # The scoping guard reaches the auto-release path too: an intent that
        # only lives on a re-sync MERGE commit is NOT a real pending bump, so
        # even with the model live the fix must still be flagged as stranded.
        base = subprocess.run(
            ["git", "-C", str(self.tmp), "rev-parse", "HEAD"],
            capture_output=True, text=True, check=True).stdout.strip()
        # main: a user-facing fix, NO intent on its own commit.
        self.f.write("core/runtime/src/foo.cpp", "int foo(){return 1;}\n")
        _git(self.tmp, "add", "--", "core/runtime/src/foo.cpp")
        _git(self.tmp, "commit", "-q", "-m", "feat(core): user-facing change")
        # side branch → real merge carrying the stray intent trailer.
        _git(self.tmp, "checkout", "-q", "-b", "side", base)
        self.f.write("core/runtime/src/bar.cpp", "int bar(){return 2;}\n")
        _git(self.tmp, "add", "--", "core/runtime/src/bar.cpp")
        _git(self.tmp, "commit", "-q", "-m", "chore(core): add bar")
        _git(self.tmp, "checkout", "-q", "main")
        _git(self.tmp, "merge", "--no-ff", "-q", "side", "-m",
             "Merge side into main\n\nVersion-Bump: sdk=minor\n")
        rc_on, out_on = self._classify(accept_intent=True)
        self.assertEqual(rc_on, 0,
                         "stray merge-commit intent wrongly suppressed a strand")


if __name__ == "__main__":
    unittest.main()
