#!/usr/bin/env python3
"""Tests for tools/ci/a2t_headroom_annotation.py and its gate wiring."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import a2t_headroom_annotation as ann  # noqa: E402

LIMIT, BUDGET = 512, 384


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(HERE / "a2t_headroom_annotation.py"), *args],
                          capture_output=True, text=True, timeout=60)


class ClassifyTests(unittest.TestCase):
    def test_thresholds(self) -> None:
        early = int(ann.EARLY_WARNING_RATIO * LIMIT + 0.999)  # 308: first count at or past 60%
        cases = {0: "ok", 268: "ok", early - 1: "ok", early: "early", BUDGET: "early",
                 BUDGET + 1: "over", LIMIT: "over"}
        for count, expected in cases.items():
            with self.subTest(count=count):
                self.assertEqual(ann.classify(count, LIMIT, BUDGET), expected)

    def test_early_warning_comes_well_before_the_guard(self) -> None:
        # At the measured 10.4 revisions a day, 60% leaves about a week and a half.
        self.assertLess(ann.EARLY_WARNING_RATIO * LIMIT, BUDGET - 70)


class CliTests(unittest.TestCase):
    def measure(self, count: int) -> subprocess.CompletedProcess:
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp, "m.json")
            p.write_text(json.dumps({"count": count, "limit": LIMIT, "headroom": LIMIT - count,
                                     "budget": BUDGET}), encoding="utf-8")
            return run(str(p))

    def test_below_the_early_mark_is_a_notice_with_the_numbers(self) -> None:
        proc = self.measure(268)
        self.assertEqual(proc.returncode, 0)
        self.assertTrue(proc.stdout.startswith(f"::notice title={ann.TITLE}::268 "), proc.stdout)

    def test_early_mark_warns_and_names_the_repin(self) -> None:
        proc = self.measure(320)
        self.assertEqual(proc.returncode, 0)
        self.assertTrue(proc.stdout.startswith(f"::warning title={ann.TITLE}::320 "), proc.stdout)
        self.assertIn("64 revisions from now", proc.stdout)
        self.assertIn("A2T_SCOPE_HISTORY_BASE", proc.stdout)
        self.assertIn("never raise A2T_SCOPE_HISTORY_LIMIT", proc.stdout)

    def test_past_the_guard_says_the_gate_is_failing(self) -> None:
        proc = self.measure(390)
        self.assertIn("::warning", proc.stdout)
        self.assertIn("failing now", proc.stdout)

    def test_a_missing_or_broken_measurement_is_loud_not_silent(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            broken = Path(tmp, "broken.json")
            broken.write_text('{"count": 3}', encoding="utf-8")
            for path in (str(Path(tmp, "absent.json")), str(broken)):
                with self.subTest(path=path):
                    proc = run(path)
                    self.assertEqual(proc.returncode, 0)
                    self.assertIn(f"::warning title={ann.TITLE}::A2T scope-history headroom was not measured",
                                  proc.stdout)


class FakeGitHub:
    """Records gh api calls and serves issues from an in-memory list."""

    def __init__(self, issues: list[dict] | None = None) -> None:
        self.issues = issues or []
        self.calls: list[tuple[list[str], str | None]] = []

    def __call__(self, args: list[str], stdin: str | None = None) -> object:
        self.calls.append((args, stdin))
        if "/search/issues" in args:
            return {"items": [dict(i) for i in self.issues]}
        if args[:2] == ["-X", "POST"] and args[2].endswith("/issues"):
            fields = json.loads(stdin or "{}")
            self.issues.append({"number": 900 + len(self.issues), "state": "open", **fields})
            return {}
        if args[:2] == ["-X", "PATCH"]:
            number = int(args[2].rsplit("/", 1)[1])
            issue = next(i for i in self.issues if i["number"] == number)
            if stdin:
                issue.update(json.loads(stdin))
            if "state=closed" in args:
                issue["state"] = "closed"
            return {}
        return {}

    def mutations(self) -> list[list[str]]:
        return [a for a, _ in self.calls if "/search/issues" not in a]


def measurement_at_share_of_budget(share: float) -> dict:
    count = int(BUDGET * share)
    return {"count": count, "limit": LIMIT, "headroom": LIMIT - count, "budget": BUDGET}


class TrackingIssueTests(unittest.TestCase):
    """The issue fires when headroom drops below 20% of the guard's budget."""

    def test_the_issue_threshold_is_headroom_below_a_fifth_of_the_budget(self) -> None:
        self.assertAlmostEqual(ann.EARLY_WARNING_RATIO * LIMIT, 0.80 * BUDGET, places=6)

    def test_at_79_percent_of_the_budget_no_issue_is_opened(self) -> None:
        gh = FakeGitHub()
        action = ann.reconcile_issue(measurement_at_share_of_budget(0.79), "o/r", gh)
        self.assertEqual(action, "none")
        self.assertEqual(gh.mutations(), [])

    def test_at_81_percent_one_issue_opens_then_updates_in_place(self) -> None:
        gh = FakeGitHub()
        first = ann.reconcile_issue(measurement_at_share_of_budget(0.81), "o/r", gh)
        self.assertEqual(first, "opened")
        self.assertEqual(len(gh.issues), 1)
        issue = gh.issues[0]
        count = int(BUDGET * 0.81)
        self.assertEqual(issue["title"], f"{ann.TITLE}: {count}/{LIMIT} scope-touching revisions; "
                                         "re-pin A2T_SCOPE_HISTORY_BASE")
        self.assertIn("A2T_SCOPE_HISTORY_BASE", issue["body"])
        self.assertIn("never raise A2T_SCOPE_HISTORY_LIMIT", issue["body"])
        self.assertIn(ann.ISSUE_MARKER, issue["body"])
        # A later push at a higher count updates the same issue, never a second one.
        second = ann.reconcile_issue(measurement_at_share_of_budget(0.9), "o/r", gh)
        self.assertEqual(second, f"updated #{issue['number']}")
        self.assertEqual(len(gh.issues), 1)
        self.assertIn(f"{int(BUDGET * 0.9)}/{LIMIT}", gh.issues[0]["title"])

    def test_after_a_repin_the_open_issue_closes(self) -> None:
        gh = FakeGitHub()
        ann.reconcile_issue(measurement_at_share_of_budget(0.81), "o/r", gh)
        repinned = {"count": 128, "limit": LIMIT, "headroom": LIMIT - 128, "budget": BUDGET}
        action = ann.reconcile_issue(repinned, "o/r", gh)
        self.assertEqual(action, f"closed #{gh.issues[0]['number']}")
        self.assertEqual(gh.issues[0]["state"], "closed")
        # And stays closed: a healthy push does nothing further.
        before = len(gh.mutations())
        self.assertEqual(ann.reconcile_issue(repinned, "o/r", gh), "none")
        self.assertEqual(len(gh.mutations()), before)

    def test_an_issue_without_the_marker_is_never_touched(self) -> None:
        stranger = {"number": 5, "state": "open", "title": f"{ann.TITLE} discussion", "body": "hand-written"}
        gh = FakeGitHub([stranger])
        repinned = {"count": 128, "limit": LIMIT, "headroom": LIMIT - 128, "budget": BUDGET}
        self.assertEqual(ann.reconcile_issue(repinned, "o/r", gh), "none")
        self.assertEqual(gh.mutations(), [])

    def test_an_unreadable_measurement_never_opens_or_closes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = run("--reconcile-issue", "--repo", "o/r", str(Path(tmp, "absent.json")))
            self.assertEqual(proc.returncode, 0)
            self.assertIn("was not measured", proc.stdout)
            self.assertNotIn("tracking issue:", proc.stdout)


class WiringTests(unittest.TestCase):
    def test_the_gate_measures_and_announces(self) -> None:
        workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
        test_step = workflow.split("      - name: Test (non-Windows)\n", 1)[1].split("\n      - name:", 1)[0]
        self.assertIn("PULP_A2T_HEADROOM_OUT: ${{ runner.temp }}/a2t-scope-headroom.json", test_step)
        step = workflow.split("      - name: Announce A2T scope-history headroom\n", 1)[1].split("\n      - name:", 1)[0]
        self.assertIn("steps.ctest.outcome != 'skipped'", step)
        self.assertIn("continue-on-error: true", step)
        self.assertIn('python3 tools/ci/a2t_headroom_annotation.py "$RUNNER_TEMP/a2t-scope-headroom.json"', step)

    def test_a_push_to_main_keeps_the_tracking_issue(self) -> None:
        workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
        upload = workflow.split("      - name: Upload A2T scope-history headroom\n", 1)[1]
        upload = upload.split("\n      - name:", 1)[0]
        self.assertIn("github.event_name == 'push' && github.ref == 'refs/heads/main'", upload)
        self.assertIn("runner.os == 'Linux'", upload)
        self.assertIn("name: a2t-scope-headroom", upload)
        job = workflow.split("\n  a2t-headroom-issue:\n", 1)[1].split("\n  ci-cause:\n", 1)[0]
        self.assertIn("needs: [build]", job)
        self.assertIn("github.event_name == 'push' && github.ref == 'refs/heads/main'", job)
        self.assertIn("issues: write", job)
        self.assertIn("continue-on-error: true", job)
        self.assertIn("--reconcile-issue", job)
        self.assertIn('--repo "$GITHUB_REPOSITORY"', job)

    def test_the_guard_test_writes_its_measurement(self) -> None:
        source = (ROOT / "tools/scripts/test_gpu_trace_overhead_acceptance.py").read_text(encoding="utf-8")
        guard = source.split("def test_scope_touching_history_keeps_headroom_under_its_limit", 1)[1]
        guard = guard.split("\n    def ", 1)[0]
        self.assertIn('os.environ.get("PULP_A2T_HEADROOM_OUT")', guard)
        # Written before the assertions, so a failing guard still reports its count.
        self.assertLess(guard.index("PULP_A2T_HEADROOM_OUT"), guard.index("assertLessEqual"))


if __name__ == "__main__":
    unittest.main()
