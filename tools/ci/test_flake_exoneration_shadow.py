#!/usr/bin/env python3
"""Tests for tools/ci/flake_exoneration_shadow.py (shadow OCCURS_ON_OTHER_CLS verdict).

All GitHub reads are stubbed. What must hold:
- a failing test is would_exonerate only when it failed on >= 2 OTHER heads in
  the window AND passes on main's tip, judged by its required gate job;
- a test that also fails on main's tip is never exonerable (the base-red case
  stays with the base-red detector);
- main evidence that was not READ never exonerates: no observed tip, a gate
  that did not execute the suite, an undownloadable failing-test list, and a
  red gate that named no test are all "unknown";
- a deterministic test (the `pr-fast` static-contract label, or a drift check
  drift-fast names) is never exonerated, however many heads it failed on;
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
import io
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import base_poison_detector as bpd  # noqa: E402
import flake_exoneration_shadow as fes  # noqa: E402

NOW = dt.datetime(2026, 9, 27, 12, 0, tzinfo=dt.timezone.utc)


def run(rid: int, head: str, hours_ago: float, conclusion: str = "failure") -> dict:
    return {"id": rid, "head_sha": head, "conclusion": conclusion, "status": "completed",
            "created_at": (NOW - dt.timedelta(hours=hours_ago)).isoformat().replace("+00:00", "Z")}


GREEN = fes.MainEvidence("9", "success")
UNKNOWN = fes.MainEvidence("9", "unknown", reason="main's failing-test list could not be read")


def red(*failing: str) -> fes.MainEvidence:
    return fes.MainEvidence("9", "failure", frozenset(failing))


class DecideTests(unittest.TestCase):
    def test_two_other_heads_and_green_main_exonerates(self) -> None:
        r = fes.decide(["Flaky: a"], {"Flaky: a": {"h1", "h2"}}, GREEN)
        self.assertTrue(r["tests"]["Flaky: a"]["would_exonerate"])
        self.assertEqual(r["tests"]["Flaky: a"]["reason"], "OCCURS_ON_OTHER_CLS")
        self.assertTrue(r["tests"]["Flaky: a"]["main_evidence_read"])
        self.assertTrue(r["exonerated_only"])

    def test_one_other_head_is_not_enough(self) -> None:
        r = fes.decide(["Flaky: a"], {"Flaky: a": {"h1"}}, GREEN)
        self.assertFalse(r["tests"]["Flaky: a"]["would_exonerate"])
        self.assertIn("only 1 other head", r["tests"]["Flaky: a"]["reason"])

    def test_failing_on_main_is_never_exonerated(self) -> None:
        r = fes.decide(["Red: b"], {"Red: b": {"h1", "h2", "h3"}}, red("Red: b"))
        self.assertFalse(r["tests"]["Red: b"]["would_exonerate"])
        self.assertEqual(r["tests"]["Red: b"]["reason"], "fails on main too")

    def test_main_failing_elsewhere_still_counts_as_passing_this_test(self) -> None:
        r = fes.decide(["Flaky: a"], {"Flaky: a": {"h1", "h2"}}, red("Other: z"))
        self.assertTrue(r["tests"]["Flaky: a"]["would_exonerate"])

    def test_unread_main_never_exonerates(self) -> None:
        r = fes.decide(["Flaky: a"], {"Flaky: a": {"h1", "h2"}}, UNKNOWN)
        self.assertFalse(r["tests"]["Flaky: a"]["would_exonerate"])
        self.assertFalse(r["tests"]["Flaky: a"]["main_evidence_read"])
        self.assertIn("main result unknown", r["tests"]["Flaky: a"]["reason"])

    def test_deterministic_test_is_never_exonerated(self) -> None:
        r = fes.decide(["script-test-inputs-drift"], {"script-test-inputs-drift": {"h1", "h2", "h3", "h4"}},
                       GREEN, deterministic={"script-test-inputs-drift"})
        v = r["tests"]["script-test-inputs-drift"]
        self.assertFalse(v["would_exonerate"])
        self.assertTrue(v["deterministic"])
        self.assertIn("deterministic", v["reason"])
        self.assertFalse(r["exonerated_only"])

    def test_mixed_failures_are_not_exonerated_only_and_unique_cause_counts(self) -> None:
        r = fes.decide(["Flaky: a", "New: c"], {"Flaky: a": {"h1", "h2"}}, GREEN)
        self.assertFalse(r["exonerated_only"])
        self.assertEqual(r["unique_cause"], 1)


def obs(conclusion: str, execution: str = bpd.EXECUTED) -> bpd.SuiteObservation:
    return bpd.SuiteObservation(run_id="77", lane="main", conclusion=conclusion, execution=execution)


class MainEvidenceTests(unittest.TestCase):
    def evidence(self, observed, failing=None) -> fes.MainEvidence:
        return fes.main_evidence("O/R", observe_fn=lambda repo: observed,
                                 failing_fn=lambda repo, rid: failing)

    def test_green_gate_is_read(self) -> None:
        e = self.evidence(obs("success"))
        self.assertEqual((e.conclusion, e.read, e.run_id), ("success", True, "77"))

    def test_red_gate_with_a_read_list_is_read(self) -> None:
        e = self.evidence(obs("failure"), ("Red: b",))
        self.assertEqual((e.conclusion, e.failing), ("failure", frozenset({"Red: b"})))

    def test_red_gate_with_an_unreadable_list_is_unknown(self) -> None:
        e = self.evidence(obs("failure"), None)
        self.assertEqual(e.conclusion, "unknown")
        self.assertIn("could not be read", e.reason)

    def test_red_gate_that_named_no_test_is_unknown(self) -> None:
        self.assertEqual(self.evidence(obs("failure"), ()).conclusion, "unknown")

    def test_gate_that_did_not_execute_the_suite_is_unknown(self) -> None:
        # A receipt-reuse green reports success without running anything, and a
        # red build never reached ctest; a stale artifact must not stand in.
        self.assertEqual(self.evidence(obs("success", execution=bpd.NOT_EXECUTED)).conclusion, "unknown")
        self.assertEqual(self.evidence(obs("failure", execution=bpd.BUILT_BUT_UNTESTED), ("Red: b",)).conclusion,
                         "unknown")

    def test_no_observed_tip_is_unknown(self) -> None:
        self.assertEqual(self.evidence(None).conclusion, "unknown")


class ReadFailingTestsTests(unittest.TestCase):
    def patch(self, listing, blob):
        saved = (bpd.gh, bpd.gh_bytes)
        bpd.gh = lambda path, jq=None: listing
        bpd.gh_bytes = lambda path: blob
        self.addCleanup(lambda: setattr(bpd, "gh", saved[0]) or setattr(bpd, "gh_bytes", saved[1]))

    @staticmethod
    def zipped(members: dict[str, str]) -> bytes:
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as z:
            for name, text in members.items():
                z.writestr(name, text)
        return buf.getvalue()

    def test_names_come_from_last_tests_failed(self) -> None:
        self.patch("42", self.zipped({bpd.LAST_TESTS_FAILED_MEMBER: "12:Red: b\n"}))
        self.assertEqual(fes.read_failing_tests("O/R", "1"), ("Red: b",))

    def test_every_unreadable_shape_is_none_not_empty(self) -> None:
        for listing, blob in (("", None), (None, None), ("null", None), ("42", None), ("42", b"not a zip"),
                              ("42", self.zipped({"other.log": "x"}))):
            with self.subTest(listing=listing, blob=blob):
                self.patch(listing, blob)
                self.assertIsNone(fes.read_failing_tests("O/R", "1"))


class DeterminismTests(unittest.TestCase):
    def test_pr_fast_label_and_drift_fast_names_are_deterministic(self) -> None:
        labels = {"lint-a": {"pr-fast"}, "census-b": set(), "Flaky: c": {"ui"}}
        self.assertEqual(fes.deterministic_tests(labels, {"census-b"}), {"lint-a", "census-b"})

    def test_junit_labels_are_read_per_failing_case(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp, "j.xml")
            p.write_text('<testsuite>'
                         '<testcase name="lint-a"><properties><property name="cmake_labels" value="pr-fast;quality"/>'
                         '</properties><failure message="x"/></testcase>'
                         '<testcase name="ok"><properties><property name="cmake_labels" value="pr-fast"/></properties>'
                         '</testcase></testsuite>', encoding="utf-8")
            self.assertEqual(fes.junit_failure_labels(p), {"lint-a": {"pr-fast", "quality"}})

    def test_the_checked_in_drift_fast_list_is_read(self) -> None:
        self.assertIn("script-test-inputs-drift", fes.drift_fast_tests())


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
