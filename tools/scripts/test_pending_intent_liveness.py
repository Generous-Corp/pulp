#!/usr/bin/env python3
"""Tests for pending_intent_liveness.py — the release liveness alarm.

Load-bearing behavior: the alarm fires when a release PR has been open past
the threshold, when a merged release PR is still pending its tag past the
grace window, or when release-please.yml has not succeeded within the
threshold (or ever), or when a merge after a surface's newest tag touches its
trigger paths without an explicitly non-releasing title and no release PR
exists for it past the threshold; and it stays quiet for a young release PR,
a fresh hand-off, a recent successful run, PRs that are not release PRs, a
`ci:`-style title, an untouched surface, and a surface with an open release
PR.
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import pending_intent_liveness as pil  # noqa: E402

NOW = datetime(2026, 10, 24, 18, 0, tzinfo=timezone.utc)


def ago(minutes: int) -> str:
    return (NOW - timedelta(minutes=minutes)).isoformat().replace("+00:00", "Z")


def open_pr(minutes: int, branch: str = "release-please--branches--main") -> dict:
    return {"number": 3, "title": "chore(main): release 1.11.0",
            "head": {"ref": branch}, "created_at": ago(minutes)}


def merged_pr(minutes: int) -> dict:
    return {"number": 4, "title": "chore(main): release plugin 0.492.0",
            "merged_at": ago(minutes)}


class Assess(unittest.TestCase):
    def kinds(self, result: dict, key: str = "overdue") -> list[str]:
        return [row["kind"] for row in result[key]]

    def test_healthy_state_is_quiet(self) -> None:
        result = pil.assess([open_pr(30)], [merged_pr(10)], ago(5), NOW)
        self.assertEqual(result["overdue"], [])
        self.assertEqual(self.kinds(result, "pending"), ["release-pr-overdue", "handoff-stuck"])

    def test_an_old_release_pr_alarms(self) -> None:
        result = pil.assess([open_pr(361)], [], ago(5), NOW)
        self.assertEqual(self.kinds(result), ["release-pr-overdue"])

    def test_a_merged_release_pr_left_pending_alarms(self) -> None:
        result = pil.assess([], [merged_pr(61)], ago(5), NOW)
        self.assertEqual(self.kinds(result), ["handoff-stuck"])

    def test_a_silent_release_please_alarms(self) -> None:
        self.assertEqual(self.kinds(pil.assess([], [], ago(400), NOW)), ["release-please-silent"])
        self.assertEqual(self.kinds(pil.assess([], [], None, NOW)), ["release-please-silent"])

    def test_non_release_prs_are_ignored(self) -> None:
        result = pil.assess([open_pr(9999, branch="fix/x")], [], ago(5), NOW)
        self.assertEqual(result, {"overdue": [], "pending": []})

    def test_thresholds_are_parameters(self) -> None:
        result = pil.assess([open_pr(100)], [merged_pr(20)], ago(5), NOW,
                            threshold_minutes=90, grace_minutes=15)
        self.assertEqual(self.kinds(result), ["release-pr-overdue", "handoff-stuck"])


class UnreleasedChanges(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name)
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.email", "t@example.com")
        self.git("config", "user.name", "t")
        versioning = {"surfaces": {
            "sdk": {"trigger_paths": ["core/**"],
                    "release_please": {"config": "rp/sdk.json", "manifest": "rp/sdk.m.json"}},
            "plugin": {"trigger_paths": [".claude-plugin/**"],
                       "release_please": {"config": "rp/plugin.json", "manifest": "rp/p.m.json"}},
        }}
        self.write("versioning.json", json.dumps(versioning))
        self.write("rp/sdk.json", json.dumps({"include-component-in-tag": False,
                                              "packages": {".": {}}}))
        self.write("rp/plugin.json", json.dumps({"packages": {".": {"component": "plugin"}}}))
        self.write("core/a.cpp", "0\n")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "base")
        self.git("tag", "v1.0.0")
        self.git("tag", "plugin-v0.1.0")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def git(self, *args: str) -> str:
        return subprocess.run(["git", "-C", str(self.repo), *args], check=True,
                              capture_output=True, text=True, encoding="utf-8").stdout

    def write(self, rel: str, text: str) -> None:
        path = self.repo / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def merge(self, title: str, path: str = "core/a.cpp") -> None:
        self.git("checkout", "-q", "-b", "topic")
        self.write(path, title + "\n")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "work")
        self.git("checkout", "-q", "main")
        self.git("merge", "-q", "--no-ff", "-m", f"Merge pull request #1 from topic\n\n{title}",
                 "topic")
        self.git("branch", "-q", "-D", "topic")

    def rows(self, open_surfaces=frozenset(), minutes_later: int = 400) -> list[dict]:
        now = datetime.now(timezone.utc) + timedelta(minutes=minutes_later)
        return pil.unreleased_changes(self.repo, self.repo / "versioning.json",
                                      set(open_surfaces), now, 360)

    def test_an_invisible_fix_with_no_release_pr_alarms(self) -> None:
        self.merge("Paint the knob on the snapped grid")
        rows = self.rows()
        self.assertEqual([r["kind"] for r in rows], ["unreleased-change"])
        self.assertIn("sdk since v1.0.0", rows[0]["title"])

    def test_quiet_cases(self) -> None:
        self.merge("ci: tidy the workflow")
        self.merge("fix(plugin): only plugin", path=".claude-plugin/x.json")
        self.assertEqual([r["title"].split(" ")[0] for r in self.rows()], ["plugin"])
        self.assertEqual(self.rows(open_surfaces={"plugin"}), [])
        self.assertEqual(self.rows(minutes_later=0), [])


if __name__ == "__main__":
    unittest.main()
