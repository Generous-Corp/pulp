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


class WiringTests(unittest.TestCase):
    def test_the_gate_measures_and_announces(self) -> None:
        workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
        test_step = workflow.split("      - name: Test (non-Windows)\n", 1)[1].split("\n      - name:", 1)[0]
        self.assertIn("PULP_A2T_HEADROOM_OUT: ${{ runner.temp }}/a2t-scope-headroom.json", test_step)
        step = workflow.split("      - name: Announce A2T scope-history headroom\n", 1)[1].split("\n      - name:", 1)[0]
        self.assertIn("steps.ctest.outcome != 'skipped'", step)
        self.assertIn("continue-on-error: true", step)
        self.assertIn('python3 tools/ci/a2t_headroom_annotation.py "$RUNNER_TEMP/a2t-scope-headroom.json"', step)

    def test_the_guard_test_writes_its_measurement(self) -> None:
        source = (ROOT / "tools/scripts/test_gpu_trace_overhead_acceptance.py").read_text(encoding="utf-8")
        guard = source.split("def test_scope_touching_history_keeps_headroom_under_its_limit", 1)[1]
        guard = guard.split("\n    def ", 1)[0]
        self.assertIn('os.environ.get("PULP_A2T_HEADROOM_OUT")', guard)
        # Written before the assertions, so a failing guard still reports its count.
        self.assertLess(guard.index("PULP_A2T_HEADROOM_OUT"), guard.index("assertLessEqual"))


if __name__ == "__main__":
    unittest.main()
