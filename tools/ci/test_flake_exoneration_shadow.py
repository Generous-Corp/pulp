#!/usr/bin/env python3
"""Tests for tools/ci/flake_exoneration_shadow.py (shadow OCCURS_ON_OTHER_CLS verdict).

All GitHub reads are stubbed. What must hold:
- a failing test is would_exonerate only when it failed on >= 2 OTHER heads in
  the window AND passes on main's latest tree;
- a test that also fails on main's latest run is never exonerable (the
  base-red case stays with the base-red detector);
- an unknown main result never exonerates;
- our own head's runs and runs outside the window are not "other heads";
- `exonerated_only` is true only when every failing test qualifies, and
  `unique_cause` counts tests seen nowhere else (the control);
- the CLI exits 0 with an annotation on a verdict, 0 with no annotation when
  nothing failed, and 2 when the JUnit is unreadable.

Run:
    python3 tools/ci/test_flake_exoneration_shadow.py
"""
from __future__ import annotations

import datetime as dt
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import flake_exoneration_shadow as fes  # noqa: E402

NOW = dt.datetime(2026, 9, 27, 12, 0, tzinfo=dt.timezone.utc)


def run(rid: int, head: str, hours_ago: float, conclusion: str = "failure") -> dict:
    return {"id": rid, "head_sha": head, "conclusion": conclusion, "status": "completed",
            "created_at": (NOW - dt.timedelta(hours=hours_ago)).isoformat().replace("+00:00", "Z")}


class DecideTests(unittest.TestCase):
    def test_two_other_heads_and_green_main_exonerates(self) -> None:
        r = fes.decide(["Flaky: a"], {"Flaky: a": {"h1", "h2"}}, "success", set())
        self.assertTrue(r["tests"]["Flaky: a"]["would_exonerate"])
        self.assertEqual(r["tests"]["Flaky: a"]["reason"], "OCCURS_ON_OTHER_CLS")
        self.assertTrue(r["exonerated_only"])

    def test_one_other_head_is_not_enough(self) -> None:
        r = fes.decide(["Flaky: a"], {"Flaky: a": {"h1"}}, "success", set())
        self.assertFalse(r["tests"]["Flaky: a"]["would_exonerate"])
        self.assertIn("only 1 other head", r["tests"]["Flaky: a"]["reason"])

    def test_failing_on_main_is_never_exonerated(self) -> None:
        r = fes.decide(["Red: b"], {"Red: b": {"h1", "h2", "h3"}}, "failure", {"Red: b"})
        self.assertFalse(r["tests"]["Red: b"]["would_exonerate"])
        self.assertEqual(r["tests"]["Red: b"]["reason"], "fails on main too")

    def test_main_failing_elsewhere_still_counts_as_passing_this_test(self) -> None:
        r = fes.decide(["Flaky: a"], {"Flaky: a": {"h1", "h2"}}, "failure", {"Other: z"})
        self.assertTrue(r["tests"]["Flaky: a"]["would_exonerate"])

    def test_unknown_main_never_exonerates(self) -> None:
        r = fes.decide(["Flaky: a"], {"Flaky: a": {"h1", "h2"}}, "unknown", set())
        self.assertFalse(r["tests"]["Flaky: a"]["would_exonerate"])
        self.assertEqual(r["tests"]["Flaky: a"]["reason"], "main result unknown")

    def test_mixed_failures_are_not_exonerated_only_and_unique_cause_counts(self) -> None:
        r = fes.decide(["Flaky: a", "New: c"], {"Flaky: a": {"h1", "h2"}}, "success", set())
        self.assertFalse(r["exonerated_only"])
        self.assertEqual(r["unique_cause"], 1)


class WindowTests(unittest.TestCase):
    def test_own_head_and_old_runs_are_excluded(self) -> None:
        runs = {"pull_request": [run(1, "mine", 1), run(2, "h1", 2), run(3, "h2", 30)],
                "merge_group": [run(4, "h3", 5)]}
        out = fes.recent_failed_runs("O/R", 24, "mine", now=NOW, runs_fn=lambda repo, ev, n: runs[ev])
        self.assertEqual(sorted(r["id"] for r in out), [2, 4])

    def test_failures_by_head_reads_only_failed_runs(self) -> None:
        runs = [run(2, "h1", 2), run(4, "h3", 5), run(5, "h4", 6, conclusion="success")]
        table = fes.failures_by_head("O/R", runs, failing_fn=lambda repo, rid: {"2": ("Flaky: a",), "4": ("Flaky: a", "Other: b"), "5": ("Never",)}[rid])
        self.assertEqual(table, {"Flaky: a": {"h1", "h3"}, "Other: b": {"h3"}})

    def test_main_latest_result_uses_the_newest_merge_group(self) -> None:
        groups = [run(10, "m1", 8, "success"), run(11, "m2", 1, "failure")]
        rid, concl, failing = fes.main_latest_result("O/R", runs_fn=lambda repo, ev, n: groups,
                                                      failing_fn=lambda repo, rid: ("Red: b",))
        self.assertEqual((rid, concl, failing), ("11", "failure", {"Red: b"}))


class CliTests(unittest.TestCase):
    def test_unreadable_junit_exits_2(self) -> None:
        proc = subprocess.run([sys.executable, str(HERE / "flake_exoneration_shadow.py"), "--repo", "O/R",
                               "--junit", "/nonexistent/ctest.junit.xml", "--head", "x"],
                              capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 2)
        self.assertNotIn("::notice", proc.stdout)

    def test_no_failures_exits_0_without_annotation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp, "j.xml")
            p.write_text('<testsuite><testcase name="ok"/></testsuite>', encoding="utf-8")
            proc = subprocess.run([sys.executable, str(HERE / "flake_exoneration_shadow.py"), "--repo", "O/R",
                                   "--junit", str(p), "--head", "x"], capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertNotIn("::notice", proc.stdout)
        self.assertIn("nothing to decide", proc.stdout)


if __name__ == "__main__":
    unittest.main()
