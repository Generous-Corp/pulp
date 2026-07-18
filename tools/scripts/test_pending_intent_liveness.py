#!/usr/bin/env python3
"""Tests for pending_intent_liveness.py — the time-based backstop that catches
a positive version intent the version-at-land bot never applied.

Load-bearing behavior: a positive ``Version-Bump: <surface>=<level>`` intent
after the newest ``Version-Bump-Applied`` marker must ALARM once it is older
than the grace window, and must stay quiet when (a) it is younger than the
window, (b) it has since been applied (a newer marker exists), (c) there is no
intent at all (a no-intent fix/feat is the stranded-fix DETECTOR's job, not
this alarm's — they must not double-fire), or (d) the only intent lives on a
re-sync merge commit (``--no-merges`` scoping).
"""
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import pending_intent_liveness as pil  # noqa: E402


def _git(repo: Path, *args: str) -> None:
    subprocess.run(["git", "-C", str(repo), *args], check=True,
                   capture_output=True, text=True)


def _rev(repo: Path, ref: str = "HEAD") -> str:
    return subprocess.run(["git", "-C", str(repo), "rev-parse", ref],
                          capture_output=True, text=True, check=True).stdout.strip()


def _ct(repo: Path, ref: str = "HEAD") -> int:
    return int(subprocess.run(
        ["git", "-C", str(repo), "show", "-s", "--format=%ct", ref],
        capture_output=True, text=True, check=True).stdout)


class LivenessAlarmTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo = Path(tempfile.mkdtemp(prefix="pulp-liveness-"))
        self._n = 0
        cfg = HERE / "versioning.json"
        (self.repo / "tools/scripts").mkdir(parents=True)
        shutil.copy(cfg, self.repo / "tools/scripts/versioning.json")
        self.cfg = self.repo / "tools/scripts/versioning.json"
        _git(self.repo, "init", "-q", "-b", "main")
        _git(self.repo, "config", "user.email", "t@t")
        _git(self.repo, "config", "user.name", "t")
        _git(self.repo, "config", "commit.gpgsign", "false")
        (self.repo / "CMakeLists.txt").write_text("project(Pulp VERSION 0.1.0)\n")
        _git(self.repo, "add", "-A")
        _git(self.repo, "commit", "-q", "-m", "seed")

    def tearDown(self) -> None:
        shutil.rmtree(self.repo, ignore_errors=True)

    def _commit(self, msg: str, f: str = "core/runtime/src/x.cpp") -> str:
        self._n += 1
        p = self.repo / f
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(f"// {msg} {self._n}\nint x{self._n}(){{return {self._n};}}\n")
        _git(self.repo, "add", "-A")
        _git(self.repo, "commit", "-q", "-m", msg)
        return _rev(self.repo)

    def _marker(self, ver: str) -> str:
        self._n += 1
        (self.repo / f"marker{self._n}.txt").write_text(ver)
        _git(self.repo, "add", "-A")
        _git(self.repo, "commit", "-q", "-m",
             f"chore: bump versions\n\nVersion-Bump-Applied: sdk={ver}\n")
        return _rev(self.repo)

    def _assess(self, now_off_min: int, grace: int = 45) -> dict:
        started = self._intent_epoch
        return pil.assess(self.repo, self.cfg, "HEAD", grace_minutes=grace,
                          now_epoch=started + now_off_min * 60)

    def test_aged_intent_alarms(self) -> None:
        self._marker("0.1.0")
        self._commit("feat(core): new API\n\nVersion-Bump: sdk=minor\n")
        self._intent_epoch = _ct(self.repo)
        r = self._assess(now_off_min=60)
        self.assertTrue(r["overdue"], "aged pending intent must alarm")
        self.assertEqual(r["overdue"][0]["surface"], "sdk")

    def test_fresh_intent_is_quiet(self) -> None:
        self._marker("0.1.0")
        self._commit("feat(core): new API\n\nVersion-Bump: sdk=minor\n")
        self._intent_epoch = _ct(self.repo)
        r = self._assess(now_off_min=5)
        self.assertFalse(r["overdue"], "within-grace intent must not alarm")
        self.assertTrue(r["pending"], "within-grace intent should be reported pending")

    def test_applied_intent_is_quiet(self) -> None:
        self._marker("0.1.0")
        self._commit("feat(core): new API\n\nVersion-Bump: sdk=minor\n")
        self._intent_epoch = _ct(self.repo)
        self._marker("0.2.0")  # bot applied it — newer marker moves the base past it
        r = self._assess(now_off_min=60)
        self.assertFalse(r["overdue"], "an applied intent must not alarm")

    def test_no_intent_fixfeat_is_quiet(self) -> None:
        # A user-facing fix/feat with NO intent is the stranded-fix detector's
        # job; this alarm must stay quiet so the two don't double-fire.
        self._marker("0.1.0")
        d = self._commit("feat(core): change with no intent trailer")
        self._intent_epoch = _ct(self.repo, d)
        r = self._assess(now_off_min=60)
        self.assertFalse(r["overdue"], "no-intent fix/feat is not this alarm's concern")

    def test_merge_only_intent_is_quiet(self) -> None:
        # Intent that only exists on a re-sync MERGE commit is not an authored
        # release intent (--no-merges scoping), so it must not alarm.
        self._marker("0.1.0")
        base = _rev(self.repo)
        _git(self.repo, "checkout", "-q", "-b", "side")
        self._commit("chore(core): side work")
        _git(self.repo, "checkout", "-q", "main")
        _git(self.repo, "merge", "--no-ff", "-q", "side", "-m",
             "Merge side into main\n\nVersion-Bump: sdk=minor\n")
        self._intent_epoch = _ct(self.repo)
        r = self._assess(now_off_min=60)
        self.assertFalse(r["overdue"], "merge-commit-only intent must not alarm")

    def test_no_marker_yet_is_quiet(self) -> None:
        # Before the first apply there is no established "processed up to here"
        # line; the alarm stays quiet (the flip canary bootstraps the marker).
        self._commit("feat(core): new API\n\nVersion-Bump: sdk=minor\n")
        self._intent_epoch = _ct(self.repo)
        r = self._assess(now_off_min=600)
        self.assertIsNone(r["base"])
        self.assertFalse(r["overdue"])


if __name__ == "__main__":
    unittest.main()
