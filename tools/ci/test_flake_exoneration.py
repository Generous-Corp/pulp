#!/usr/bin/env python3
"""Tests for tools/ci/flake_exoneration.py (the merge-group flake-exoneration verdict).

All GitHub reads are stubbed. What must hold:
- a failure seen on no other pull request (a real failure) is never excused:
  the negative control;
- a failure seen on 2 or more OTHER pull requests' head runs, passing on main's
  tip, is excused; 1 other pull request is not enough;
- corroboration counts pull requests, not runs or heads; it never counts a pull
  request in this group, a merge-group run, or a run whose base is not main;
- a test the change touches (named in a changed file, or a declared input or
  command path changed) is never excused, and an unread change scope touches
  everything;
- a test that did not pass in main's tip report (new, or red there) is never
  excused, and unread main evidence excuses nothing;
- a build failure is never excused: a "not run" test that ctest did not skip on
  purpose, or a failing test whose output carries a compiler/CMake error;
- the run must be clean otherwise: ctest exit 8, and every test ctest listed as
  failed is in the JUnit report;
- `gate` in enforce mode runs the tests --stop-on-failure skipped and judges the
  union; it returns 0 and writes a record only when that union is excused;
- shadow is the default and the kill switch: any mode other than `enforce`
  (unset included) fails as before and writes no record.

Run:
    python3 tools/ci/test_flake_exoneration.py
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import flake_exoneration as fx  # noqa: E402

NOW = dt.datetime(2026, 11, 9, 12, 0, tzinfo=dt.timezone.utc)
FLAKY = "Mixer meter settles under load"
REAL = "Mixer gain law is unity at 0 dB"


def case(name: str, status: str = "run", labels: str = "", out: str = "", skip: str = "") -> str:
    props = f'<property name="cmake_labels" value="{labels}"/>' if labels else ""
    body = ""
    if status == "fail":
        body = '<failure message="Failed"/>'
    elif status == "notrun":
        body = f'<skipped message="{skip}"/>'
    return (f'<testcase name="{name}" status="{status}">{body}<properties>{props}</properties>'
            f"<system-out>{out}</system-out></testcase>")


def junit(*cases: str) -> str:
    return '<?xml version="1.0"?><testsuite>' + "".join(cases) + "</testsuite>"


def write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def report(*cases: str) -> fx.RunReport:
    with tempfile.TemporaryDirectory() as d:
        return fx.read_junit([write(Path(d) / "j.xml", junit(*cases))])


MAIN_GREEN = fx.MainPasses("900", frozenset({FLAKY, REAL, "other"}))
SCOPE = fx.Scope(frozenset({11000}), frozenset({"core/audio/src/mixer.cpp"}))


def evidence(rep: fx.RunReport, by_pr: dict | None = None, *, main=MAIN_GREEN, scope=SCOPE,
             touched=None, det=None, exit_code=8, last_failed=None) -> fx.Evidence:
    return fx.Evidence(rep, exit_code, tuple(rep.failing) if last_failed is None else last_failed,
                       by_pr or {}, main, scope, touched or {}, det or set())


class VerdictTests(unittest.TestCase):
    def test_negative_control_a_real_failure_is_never_excused(self) -> None:
        rep = report(case(REAL, "fail"), case("other"))
        r = fx.verdict(evidence(rep, {}))
        self.assertFalse(r["excused"])
        self.assertIn("0 other pull request", r["tests"][REAL]["reason"])

    def test_flake_seen_on_two_other_prs_is_excused(self) -> None:
        rep = report(case(FLAKY, "fail"))
        r = fx.verdict(evidence(rep, {FLAKY: {11001, 11002}}))
        self.assertTrue(r["excused"], r)
        self.assertEqual(r["tests"][FLAKY]["reason"], "OCCURS_ON_OTHER_CLS")
        self.assertEqual(r["tests"][FLAKY]["other_prs"], [11001, 11002])

    def test_one_other_pr_is_not_enough(self) -> None:
        rep = report(case(FLAKY, "fail"))
        r = fx.verdict(evidence(rep, {FLAKY: {11001}}))
        self.assertFalse(r["excused"])
        self.assertIn("1 other pull request", r["tests"][FLAKY]["reason"])

    def test_one_real_failure_beside_a_flake_fails_the_run(self) -> None:
        rep = report(case(FLAKY, "fail"), case(REAL, "fail"))
        r = fx.verdict(evidence(rep, {FLAKY: {11001, 11002}}))
        self.assertTrue(r["tests"][FLAKY]["excused"])
        self.assertFalse(r["excused"])

    def test_a_touched_test_is_never_excused(self) -> None:
        rep = report(case(FLAKY, "fail"))
        r = fx.verdict(evidence(rep, {FLAKY: {11001, 11002, 11003}},
                                touched={FLAKY: "named in changed file test/test_mixer.cpp"}))
        self.assertFalse(r["excused"])
        self.assertIn("touched by this change", r["tests"][FLAKY]["reason"])

    def test_a_test_new_to_this_change_is_never_excused(self) -> None:
        rep = report(case("brand new test", "fail"))
        r = fx.verdict(evidence(rep, {"brand new test": {11001, 11002}}))
        self.assertFalse(r["excused"])
        self.assertIn("did not pass on main", r["tests"]["brand new test"]["reason"])

    def test_unread_main_excuses_nothing(self) -> None:
        rep = report(case(FLAKY, "fail"))
        main = fx.MainPasses("900", frozenset(), "main's JUnit report could not be read")
        r = fx.verdict(evidence(rep, {FLAKY: {11001, 11002}}, main=main))
        self.assertFalse(r["excused"])

    def test_unread_scope_excuses_nothing(self) -> None:
        rep = report(case(FLAKY, "fail"))
        scope = fx.Scope(frozenset(), frozenset(), "compare truncated its commit list")
        r = fx.verdict(evidence(rep, {FLAKY: {11001, 11002}}, scope=scope))
        self.assertFalse(r["excused"])

    def test_deterministic_test_is_never_excused(self) -> None:
        rep = report(case(FLAKY, "fail", labels="pr-fast"))
        r = fx.verdict(evidence(rep, {FLAKY: {11001, 11002}}, det={FLAKY}))
        self.assertFalse(r["excused"])

    def test_missing_executable_is_a_build_failure(self) -> None:
        rep = report(case(FLAKY, "fail"), case("pulp-test-mixer", "notrun", skip="Unable to find executable"))
        self.assertIn("pulp-test-mixer", rep.build_failures)
        r = fx.verdict(evidence(rep, {FLAKY: {11001, 11002}}))
        self.assertFalse(r["excused"])
        self.assertTrue(any("build failure" in x for x in r["run_reasons"]))

    def test_deliberate_skips_are_not_build_failures(self) -> None:
        rep = report(case(FLAKY, "fail"), case("a", "notrun", skip="SKIP_RETURN_CODE=77"),
                     case("b", "notrun", skip="SKIP_REGULAR_EXPRESSION_MATCHED"))
        self.assertEqual(rep.build_failures, {})
        self.assertTrue(fx.verdict(evidence(rep, {FLAKY: {11001, 11002}}))["excused"])

    def test_compile_error_output_is_a_build_failure(self) -> None:
        rep = report(case(FLAKY, "fail", out="main.cpp:3:1: error: expected ';'"))
        r = fx.verdict(evidence(rep, {FLAKY: {11001, 11002}}))
        self.assertFalse(r["excused"])
        self.assertIn("build failure", r["tests"][FLAKY]["reason"])

    def test_ctest_exit_other_than_tests_failed_is_never_excused(self) -> None:
        rep = report(case(FLAKY, "fail"))
        self.assertFalse(fx.verdict(evidence(rep, {FLAKY: {11001, 11002}}, exit_code=1))["excused"])

    def test_a_failure_ctest_saw_but_junit_did_not_is_never_excused(self) -> None:
        rep = report(case(FLAKY, "fail"))
        r = fx.verdict(evidence(rep, {FLAKY: {11001, 11002}}, last_failed=(FLAKY, "crashed before junit")))
        self.assertFalse(r["excused"])

    def test_no_failing_test_is_never_excused(self) -> None:
        self.assertFalse(fx.verdict(evidence(report(case("ok")), {}))["excused"])


class ScopeAndTouchTests(unittest.TestCase):
    def test_compare_payload_yields_prs_and_files(self) -> None:
        s = fx.scope_from_compare({"total_commits": 2, "commits": [
            {"commit": {"message": "Merge pull request #11001 from x/y\n\nbody"}},
            {"commit": {"message": "fix: mixer"}}],
            "files": [{"filename": "a.cpp"}, {"filename": "b.cpp", "previous_filename": "old.cpp"}]})
        self.assertTrue(s.read)
        self.assertEqual(s.prs, {11001})
        self.assertEqual(s.files, {"a.cpp", "b.cpp", "old.cpp"})

    def test_compare_records_each_group_prs_head(self) -> None:
        s = fx.scope_from_compare({"total_commits": 1, "files": [], "commits": [
            {"commit": {"message": "Merge pull request #11001 from x/y"},
             "parents": [{"sha": "base"}, {"sha": "prhead"}]}]})
        self.assertEqual(s.pr_heads, {"prhead"})

    def test_truncated_compare_is_unread(self) -> None:
        self.assertFalse(fx.scope_from_compare({"total_commits": 300, "commits": [{}] * 250, "files": []}).read)
        self.assertFalse(fx.scope_from_compare({"total_commits": 1, "commits": [{}],
                                                "files": [{"filename": f"{i}"} for i in range(300)]}).read)
        self.assertFalse(fx.scope_from_compare(None).read)

    def test_name_in_changed_file_and_changed_input_touch_a_test(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            write(root / "test/test_mixer.cpp", f'TEST_CASE("{FLAKY}", "[mixer]") {{}}')
            write(root / "tools/ci/check.py", "pass")
            scope = fx.Scope(frozenset(), frozenset({"test/test_mixer.cpp", "tools/ci/check.py"}))
            t = fx.touched_tests([FLAKY, "check-selftest", "untouched"], scope, root,
                                 {"check-selftest": ["tools/ci"]}, {})
            self.assertIn("test/test_mixer.cpp", t[FLAKY])
            self.assertIn("tools/ci", t["check-selftest"])
            self.assertNotIn("untouched", t)

    def test_unread_scope_touches_everything(self) -> None:
        t = fx.touched_tests([FLAKY], fx.Scope(frozenset(), frozenset(), "compare unreadable"), Path("."), {}, {})
        self.assertIn(FLAKY, t)


class CorroborationTests(unittest.TestCase):
    def run_(self, rid: int, pr: int | None, base: str = "main", conclusion: str = "failure",
             hours_ago: float = 1) -> dict:
        prs = [] if pr is None else [{"number": pr, "base": {"ref": base}}]
        return {"id": rid, "conclusion": conclusion, "pull_requests": prs,
                "created_at": (NOW - dt.timedelta(hours=hours_ago)).isoformat().replace("+00:00", "Z")}

    def test_counts_distinct_other_prs_only(self) -> None:
        runs = [self.run_(1, 11001), self.run_(2, 11001), self.run_(3, 11002), self.run_(4, 11000),
                self.run_(5, 11003, base="release"), self.run_(6, None), self.run_(7, 11004, conclusion="success")]
        table = fx.failures_by_pr("o/r", runs, {11000}, [FLAKY], failing_fn=lambda repo, rid: [FLAKY, "x"],
                                  lookup=lambda sha: None, min_other_prs=99)
        self.assertEqual(table[FLAKY], {11001, 11002})
        self.assertNotIn("x", table)

    def test_a_dependent_corroborating_pr_does_not_count(self) -> None:
        runs = [self.run_(1, 11001), self.run_(2, 11002), self.run_(3, 11003)]
        table = fx.failures_by_pr("o/r", runs, set(), [FLAKY], failing_fn=lambda repo, rid: [FLAKY],
                                  lookup=lambda sha: None, independent=lambda r, pr, t: pr != 11002,
                                  min_other_prs=99)
        self.assertEqual(table[FLAKY], {11001, 11003})

    def test_reading_stops_once_corroborated(self) -> None:
        reads = []
        runs = [self.run_(i, 11000 + i) for i in range(1, 6)]
        fx.failures_by_pr("o/r", runs, set(), [FLAKY], lookup=lambda sha: None,
                          failing_fn=lambda repo, rid: reads.append(rid) or [FLAKY])
        self.assertEqual(reads, ["1", "2"])

    def test_only_a_full_suite_run_corroborates(self) -> None:
        ran = [{"name": "macos", "steps": [{"name": fx.FULL_SUITE_STEP, "conclusion": "failure"}]}]
        skipped = [{"name": "macos", "steps": [{"name": fx.FULL_SUITE_STEP, "conclusion": "skipped"}]},
                   {"name": "Linux (x64)", "steps": [{"name": fx.FULL_SUITE_STEP, "conclusion": "failure"}]}]
        self.assertTrue(fx.full_suite_ran(ran))
        self.assertFalse(fx.full_suite_ran(skipped))
        blob = junit(case("a", "fail"), case("b")).encode()
        self.assertEqual(fx.failing_names_from_junit_bytes(blob), ("a",))

    def test_a_run_without_its_pr_list_is_resolved_from_its_head_commit(self) -> None:
        run = dict(self.run_(1, None), head_sha="h1")
        pulls = {"h1": [{"number": 11005, "base": {"ref": "main"}}]}
        self.assertEqual(fx.run_pr(run, pulls.get), 11005)
        stacked = {"h1": [{"number": 1, "base": {"ref": "main"}}, {"number": 2, "base": {"ref": "main"}}]}
        self.assertIsNone(fx.run_pr(run, stacked.get))
        self.assertIsNone(fx.run_pr(run, {"h1": [{"number": 3, "base": {"ref": "feature/x"}}]}.get))

    def test_window_stops_at_older_runs(self) -> None:
        pages = {1: [self.run_(1, 1, hours_ago=2), self.run_(2, 2, hours_ago=30)], 2: [self.run_(3, 3)]}
        runs = fx.recent_pr_runs("o/r", 24, now=NOW, page_fn=lambda p: pages.get(p, []))
        self.assertEqual([r["id"] for r in runs], [1])


class GateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.build = self.root / "build"
        self.junit = write(self.build / "ctest.junit.xml", junit(case("other"), case(FLAKY, "fail")))
        write(self.build / "Testing/Temporary/LastTestsFailed.log", f"2:{FLAKY}\n")
        self.selected = write(self.root / "selected.json", json.dumps(
            {"tests": [{"name": n, "command": []} for n in ("other", FLAKY, "later-1", "later-2")]}))
        self.summaries: list[str] = []

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def args(self, mode: str | None = "enforce", ctest_exit: int = 8) -> argparse.Namespace:
        return fx.build_parser().parse_args([
            "gate", "--repo", "o/r", "--merge-sha", "abc", "--ctest-exit", str(ctest_exit),
            "--build-dir", str(self.build), "--source-root", str(self.root), "--junit", str(self.junit),
            "--selected-json", str(self.selected), "--label-exclude", "slow",
            "--work-dir", str(self.root / "work"), "--record-out", str(self.root / "rec/flake-excusal.json"),
            "--run-id", "42"] + ([] if mode is None else ["--mode", mode]))

    def collect(self, by_pr):
        return lambda a, rep, sel: {"scope": SCOPE, "by_pr": by_pr,
                                    "main": fx.MainPasses("900", frozenset({FLAKY, "other", "later-1", "later-2"})),
                                    "script_inputs": {}, "commands": {}}

    def remainder_run(self, cases: list[str], rc: int = 0, failed: str = ""):
        calls = []

        def run(names, a, workdir):
            calls.append(list(names))
            write(workdir / "remainder.junit.xml", junit(*cases))
            if failed:
                write(self.build / "Testing/Temporary/LastTestsFailed.log", failed)
            return rc
        return run, calls

    def gate(self, a, by_pr, run_remainder=None) -> int:
        return fx.cmd_gate(a, collect=self.collect(by_pr), run_remainder=run_remainder,
                           write_summary=self.summaries.append)

    def test_enforce_runs_the_skipped_tests_then_excuses(self) -> None:
        run, calls = self.remainder_run([case("later-1"), case("later-2")])
        rc = self.gate(self.args(), {FLAKY: {11001, 11002}}, run)
        self.assertEqual(rc, 0)
        self.assertEqual(calls, [["later-1", "later-2"]])
        rec = json.loads((self.root / "rec/flake-excusal.json").read_text(encoding="utf-8"))
        self.assertEqual(rec["schema"], fx.SCHEMA)
        self.assertEqual(rec["run_id"], "42")
        self.assertEqual([t["name"] for t in rec["tests"]], [FLAKY])
        merged = fx.read_junit([self.junit])
        self.assertEqual(merged.ran, {"other", FLAKY, "later-1", "later-2"})
        self.assertIn("Excused", self.summaries[-1])

    def test_a_real_failure_in_the_skipped_tests_fails_the_run(self) -> None:
        run, _ = self.remainder_run([case("later-1", "fail"), case("later-2")], rc=8, failed="3:later-1\n")
        rc = self.gate(self.args(), {FLAKY: {11001, 11002}}, run)
        self.assertEqual(rc, 8)
        self.assertFalse((self.root / "rec/flake-excusal.json").exists())
        failed = (self.build / "Testing/Temporary/LastTestsFailed.log").read_text(encoding="utf-8")
        self.assertIn(FLAKY, failed)
        self.assertIn("later-1", failed)

    def test_skipped_tests_that_still_did_not_run_fail_the_run(self) -> None:
        run, _ = self.remainder_run([case("later-1")])
        self.assertEqual(self.gate(self.args(), {FLAKY: {11001, 11002}}, run), 8)

    def test_remainder_ctest_error_fails_the_run(self) -> None:
        run, _ = self.remainder_run([case("later-1"), case("later-2")], rc=1)
        self.assertEqual(self.gate(self.args(), {FLAKY: {11001, 11002}}, run), 8)

    def test_not_excused_fails_as_before_without_running_more(self) -> None:
        run, calls = self.remainder_run([])
        self.assertEqual(self.gate(self.args(), {FLAKY: {11001}}, run), 8)
        self.assertEqual(calls, [])
        self.assertFalse((self.root / "rec/flake-excusal.json").exists())

    def test_kill_switch_shadow_fails_as_before(self) -> None:
        run, calls = self.remainder_run([case("later-1"), case("later-2")])
        for mode in ("shadow", "off", "SHADOW", ""):
            self.assertEqual(self.gate(self.args(mode=mode), {FLAKY: {11001, 11002}}, run), 8)
        self.assertEqual(calls, [])
        self.assertFalse((self.root / "rec/flake-excusal.json").exists())
        self.assertIn("shadow", self.summaries[-1])

    def test_mode_resolution(self) -> None:
        self.assertEqual(fx.resolve_mode(None), fx.MODE_SHADOW)
        self.assertEqual(fx.resolve_mode(""), fx.MODE_SHADOW)
        self.assertEqual(fx.resolve_mode("Enforce"), fx.MODE_ENFORCE)
        self.assertEqual(fx.resolve_mode("shadow"), fx.MODE_SHADOW)
        self.assertEqual(fx.resolve_mode("anything-else"), fx.MODE_SHADOW)

    def test_passing_run_is_untouched(self) -> None:
        self.assertEqual(self.gate(self.args(ctest_exit=0), {}), 0)
        self.assertEqual(self.summaries, [])

    def test_missing_last_tests_failed_is_unread(self) -> None:
        (self.build / "Testing/Temporary/LastTestsFailed.log").unlink()
        self.assertIsNone(fx.read_last_failed(self.build))
        run, _ = self.remainder_run([case("later-1"), case("later-2")])
        self.assertEqual(self.gate(self.args(), {FLAKY: {11001, 11002}}, run), 8)

    def test_remainder_keeps_the_excused_output_log(self) -> None:
        log = write(self.build / "Testing/Temporary/LastTest.log", "first run output")
        a = self.args()
        work = self.root / "work"
        work.mkdir(parents=True, exist_ok=True)
        real = fx._run_ctest_remainder
        fx._run_ctest_remainder = lambda names, a, w: (log.write_text("remainder", encoding="utf-8"), 0)[1]
        try:
            self.assertEqual(fx.run_ctest_remainder(["later-1"], a, work), 0)
        finally:
            fx._run_ctest_remainder = real
        self.assertEqual(log.read_text(encoding="utf-8"), "first run output")
        self.assertEqual(log.with_name("LastTest.remainder.log").read_text(encoding="utf-8"), "remainder")

    def test_cli_error_fails_as_before(self) -> None:
        argv = ["flake_exoneration.py"] + [x for x in [
            "gate", "--repo", "o/r", "--merge-sha", "abc", "--ctest-exit", "8",
            "--build-dir", str(self.build), "--source-root", str(self.root),
            "--junit", str(self.root / "missing.xml"), "--selected-json", str(self.selected),
            "--label-exclude", "slow", "--record-out", str(self.root / "r.json")]]
        self.assertEqual(fx.main(argv), 8)


if __name__ == "__main__":
    unittest.main()
