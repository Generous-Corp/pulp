#!/usr/bin/env python3
"""Tests for tools/scripts/reuse_policy_replay.py and its collector, reuse_replay_collect.py.

What must hold:
- every named scenario fixture scores to its stated verdict, and all nine
  scenarios from the design plus the inert-drift rules are present;
- a group test that FAILED (after until-pass:2) and that a policy skips is a
  false skip, and a policy with one is UNSAFE and makes `score` exit 1;
- a skipped fail-then-pass test is a flake-skip, never a false skip;
- a pair lacking what a policy needs runs everything: it lowers coverage and
  is never counted as a skip;
- the job-log parser takes the final outcome and counts attempts across the
  retry lines ctest prints without a counter, and reads the receipt verdict
  from the rendered notice, never from the step script that echoes it;
- registered seams without a decision function refuse to score;
- `collect` exits 1 when it finds no merge groups or no PR-head pairs.

Run:
    python3 tools/scripts/test_reuse_policy_replay.py
"""
from __future__ import annotations

import io
import json
import os
import subprocess
import urllib.error
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import reuse_policy_replay as rpr  # noqa: E402
import reuse_replay_collect as rrc
from spawn_closure import SpawnIndex  # noqa: E402

SCENARIOS = HERE / "fixtures" / "reuse_policy_replay"
TOOL = HERE / "reuse_policy_replay.py"

DESIGN_SCENARIOS = {
    "env-read selftest", "generated-list drift", "host-shaped gate", "masked verdict",
    "flake on one VM", "base-red poison", "iOS PostBuild crash", "unpinned tool", "stale build dir",
    "affected-selection misses",
}


def group(**kw) -> dict:
    run = {"run_id": "g1", "run_kind": "merge_group", "pr": 1, "head_sha": "h1", "group_sha": "m1",
           "checkout_sha": "m1", "checkout_parents": ["b2", "h1"], "base_sha": "b2", "merge_tree": "t2",
           "runner_image": None, "created_at": "2026-09-29T12:00:00Z",
           "ctest": {"ran": True, "complete": True, "failed": 0}, "build_failed": False, "observed_decision": None}
    run.update(kw)
    return run


def head(**kw) -> dict:
    run = {"run_id": "p1", "run_kind": "pr_head", "pr": 1, "head_sha": "h1", "group_sha": None,
           "checkout_sha": "r1", "checkout_parents": ["b1", "h1"], "base_sha": "b1", "merge_tree": "t1",
           "runner_image": None, "created_at": "2026-09-29T11:00:00Z",
           "ctest": {"complete": True, "full_suite": True, "failed": 0}, "receipt_issued": True,
           "required_contexts_green": True}
    run.update(kw)
    return run


def pair(drift=("docs/guides/versioning.md",), stacked=False) -> dict:
    return {"pr": 1, "head_sha": "h1", "group_run_id": "g1", "stacked": stacked,
            "heads": [{"run_id": "p1", "drift_files": list(drift), "drift_declared_input_hits": [],
                       "required_contexts_green": True}]}


def t(test_id: str, outcome: str = "pass", attempts: int = 1, dur: float = 10.0, **kw) -> dict:
    return {"test_id": test_id, "outcome": outcome, "attempts": attempts, "duration_s": dur, **kw}


def corpus(tests: list[dict], **pair_kw) -> rpr.Corpus:
    return rpr.Corpus([group(), head()], [pair(**pair_kw)], tests={"g1": tests})


class ScenarioTests(unittest.TestCase):
    def test_every_scenario_scores_its_stated_verdict(self):
        results = rpr.run_scenarios(SCENARIOS)
        bad = [r for r in results if not r["ok"]]
        self.assertEqual(bad, [], "scenario verdicts differ from the fixtures")
        self.assertGreaterEqual(len(results), 20)

    def test_all_design_scenarios_are_present(self):
        names = {r["scenario"] for r in rpr.run_scenarios(SCENARIOS)}
        self.assertEqual(DESIGN_SCENARIOS - names, set())

    def test_cli_scenarios_exit_zero(self):
        proc = subprocess.run([sys.executable, str(TOOL), "score", "--scenarios", str(SCENARIOS)],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertNotIn("FAIL", proc.stdout)

    def test_a_wrong_expectation_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            doc = json.loads((SCENARIOS / "inert_drift_rules.json").read_text())
            doc["cases"] = doc["cases"][:1]
            doc["cases"][0]["expect"]["false_skips"] = 0  # the truth is 1
            (Path(tmp) / "x.json").write_text(json.dumps(doc))
            results = rpr.run_scenarios(Path(tmp))
            self.assertFalse(results[0]["ok"])
            self.assertEqual(results[0]["mismatches"]["false_skips"], {"expected": 0, "actual": 1})


class ScoreTests(unittest.TestCase):
    def test_inert_drift_skipping_a_real_failure_is_a_false_skip(self):
        result = rpr.score(corpus([t("skills-doc-sync", "fail", 2), t("other")]), "inert-drift")
        self.assertEqual(result["skipped_groups"], 1)
        self.assertEqual(result["false_skips"], 1)
        self.assertEqual(result["false_skip_rows"][0]["test_id"], "skills-doc-sync")
        self.assertEqual(result["flake_skips"], 0)
        self.assertEqual(result["verdict"], "UNSAFE: false skips")

    def test_the_report_states_how_many_failures_the_window_could_catch(self):
        result = rpr.score(corpus([t("a", "fail", 2), t("b", "timeout"), t("c"), t("d", "pass", 2)]), "none")
        self.assertEqual(result["failing_group_tests"], 2)   # a flake that passed on retry is not one
        self.assertIn("held 2 failing tests", rpr.render(result))
        self.assertIn("cannot see data reads", rpr.render(result))

    def test_a_run_stopped_on_a_failure_is_scored(self):
        stopped = rpr.Corpus([group(ctest={"ran": True, "complete": False, "failed": 1}), head()], [pair()],
                             tests={"g1": [t("a", "fail", 2)]})
        self.assertEqual(rpr.score(stopped, "inert-drift")["false_skips"], 1)
        cut = rpr.Corpus([group(ctest={"ran": True, "complete": False, "failed": 0}), head()], [pair()],
                         tests={"g1": [t("a")]})
        self.assertEqual(rpr.score(cut, "inert-drift")["statuses"], {"incomplete": 1})

    def test_timeout_is_a_failure(self):
        result = rpr.score(corpus([t("slow-one", "timeout", 2)]), "inert-drift")
        self.assertEqual(result["false_skips"], 1)

    def test_fail_then_pass_is_a_flake_skip_not_a_false_skip(self):
        result = rpr.score(corpus([t("WorkerPool", "pass", 2), t("other")]), "inert-drift")
        self.assertEqual(result["flake_skips"], 1)
        self.assertEqual(result["false_skips"], 0)

    def test_exonerated_failure_is_a_flake_skip(self):
        result = rpr.score(corpus([t("x", "fail", 2, exonerated=True)]), "inert-drift")
        self.assertEqual((result["flake_skips"], result["false_skips"]), (1, 0))

    def test_passing_skipped_group_reads_full_benefit(self):
        result = rpr.score(corpus([t("a", dur=5.0), t("b", dur=15.0)]), "inert-drift", {"min_sample": 1})
        self.assertEqual(result["benefit_median"], 1.0)
        self.assertEqual(result["skipped_test_seconds"], 20.0)
        self.assertEqual(result["verdict"], "safe over the window")

    def test_quartiles_come_from_the_per_group_shares(self):
        groups = [group(run_id=f"g{i}", group_sha=f"m{i}", checkout_sha=f"m{i}") for i in range(4)]
        pairs = [dict(pair(), group_run_id=f"g{i}") for i in range(4)]
        tests = {f"g{i}": [t("a", dur=1.0), t("b", dur=1.0)] for i in range(4)}
        pairs[0]["heads"][0]["drift_files"] = ["core/x.cpp"]  # runs: share 0
        result = rpr.score(rpr.Corpus(groups + [head()], pairs, tests=tests), "inert-drift")
        self.assertEqual((result["benefit_p25"], result["benefit_median"], result["benefit_p75"]), (1.0, 1.0, 1.0))
        self.assertEqual(result["group_test_seconds"], 8.0)
        pairs[1]["heads"][0]["drift_files"] = ["core/x.cpp"]
        result = rpr.score(rpr.Corpus(groups + [head()], pairs, tests=tests), "inert-drift")
        self.assertEqual((result["benefit_p25"], result["benefit_median"], result["benefit_p75"]), (0.0, 0.5, 1.0))

    def test_skip_nothing_control_reads_zero(self):
        result = rpr.score(corpus([t("a", "fail", 2)]), "none")
        self.assertEqual((result["benefit_median"], result["false_skips"]), (0.0, 0))

    def test_failure_in_a_group_the_policy_runs_is_not_a_false_skip(self):
        result = rpr.score(corpus([t("a", "fail", 2)], drift=("core/view/src/widgets.cpp",)), "inert-drift")
        self.assertEqual((result["skipped_groups"], result["false_skips"]), (0, 0))

    def test_missing_data_runs_everything_and_lowers_coverage(self):
        c = corpus([t("a", "fail", 2)], stacked=None)
        result = rpr.score(c, "inert-drift")
        self.assertEqual((result["evaluable_pairs"], result["coverage"], result["false_skips"]), (0, 0.0, 0))

    def test_whole_receipt_needs_exact_base_and_tree(self):
        result = rpr.score(corpus([t("a")]), "whole-receipt")
        self.assertEqual(result["skipped_groups"], 0)
        exact = rpr.Corpus([group(base_sha="b1", checkout_parents=["b1", "h1"], merge_tree="t1"), head()],
                           [pair(drift=())], tests={"g1": [t("a")]})
        self.assertEqual(rpr.score(exact, "whole-receipt")["skipped_groups"], 1)

    def test_head_without_green_full_suite_is_not_a_receipt(self):
        for override in ({"ctest": {"complete": True, "full_suite": False, "failed": 0}},
                         {"ctest": {"complete": True, "full_suite": True, "failed": 1}},
                         {"ctest": {"complete": False, "full_suite": True, "failed": 0}}):
            c = rpr.Corpus([group(), head(**override)], [pair()], tests={"g1": [t("a", "fail", 2)]})
            result = rpr.score(c, "inert-drift")
            self.assertEqual((result["skipped_groups"], result["evaluable_pairs"]), (0, 0), override)

    def test_observed_decisions_are_compared(self):
        c = rpr.Corpus([group(observed_decision="refuse"), head()], [pair()], tests={"g1": [t("a")]})
        self.assertEqual(rpr.score(c, "inert-drift")["replay_vs_observed"]["replay_only"], 1)

    def test_seams_refuse_to_score(self):
        for name in ("suite-source-key", "per-executable"):
            with self.assertRaises(rpr.PolicyNotImplemented):
                rpr.score(corpus([t("a")]), name)

    def test_cli_score_exits_one_on_a_false_skip(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            rpr.write_jsonl(root / "runs.jsonl", [group(), head()])
            rpr.write_jsonl(root / "pairs.jsonl", [pair()])
            rpr.write_jsonl(root / "tests" / "g1.jsonl.gz", [t("a", "fail", 2)])
            proc = subprocess.run([sys.executable, str(TOOL), "score", "--corpus", tmp, "--policy", "inert-drift"],
                                  capture_output=True, text=True)
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            self.assertIn("FALSE SKIPS 1", proc.stdout)
            rpr.write_jsonl(root / "tests" / "g1.jsonl.gz", [t("a")])
            proc = subprocess.run([sys.executable, str(TOOL), "score", "--corpus", tmp, "--policy", "inert-drift"],
                                  capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)


class ValidateTests(unittest.TestCase):
    def test_bound_records_pass(self):
        self.assertIsNone(rpr.validate_run(group()))
        self.assertIsNone(rpr.validate_run(head()))

    def test_unbound_records_are_rejected(self):
        self.assertIsNotNone(rpr.validate_run(head(checkout_parents=["b1", "older-head"])))
        self.assertIsNotNone(rpr.validate_run(group(checkout_sha="other")))
        self.assertIsNotNone(rpr.validate_run(head(checkout_sha=None)))
        self.assertIsNotNone(rpr.validate_run(head(base_sha="b9")))


LOG = """\
2026-09-30T19:05:11.2268210Z [command]/opt/homebrew/bin/git log -1 --format=%H
2026-09-30T19:05:11.2328200Z 491da3c2f9351d7ba7da866b8aa028aa6ac0088b
2026-09-30T19:09:34.0705630Z   echo "::notice title=protected receipt issued (macos)::exact-tree receipt for $PR_HEAD_SHA on base $PR_BASE_SHA"
2026-09-30T19:09:34.0705640Z   echo "::warning title=protected receipt NOT issued (macos)::${reason} — the merge group will validate in full"
2026-09-30T19:09:34.1319140Z ctest gate args: label_exclude=validation|^slow$|source-selftest stop_on_failure=--stop-on-failure
2026-09-30T19:09:34.1741680Z Test project /Users/admin/actions-runner/_work/pulp/pulp/build-macos
2026-09-30T19:10:13.5818700Z     1/5 Test #21481: pulp-browser-capture-node-integration .......   Passed   38.26 sec
2026-09-30T19:10:14.5818700Z     2/5 Test     #4: WorkerPool cold-idles workers without blocking later batches ....***Failed    4.46 sec
2026-09-30T19:10:15.5818700Z     3/5 Test     #5: register_font_url: detached worker ....***Skipped   0.01 sec
2026-09-30T19:10:16.5818700Z     4/5 Test     #6: a test whose name says Failed ....   Passed    1.00 sec
2026-09-30T19:10:17.5818700Z           Test     #4: WorkerPool cold-idles workers without blocking later batches ....   Passed    0.50 sec
2026-09-30T19:10:18.5818700Z     5/5 Test     #7: census ....***Timeout 120.01 sec
2026-09-30T19:14:17.8783890Z 80% tests passed, 1 tests failed out of 5
2026-09-30T19:14:18.0000000Z Test project /Users/admin/actions-runner/_work/pulp/pulp/build-macos
2026-09-30T19:14:19.0000000Z     1/1 Test #1: installed-capability ....   Passed    0.96 sec
2026-09-30T19:14:20.0000000Z 100% tests passed, 0 tests failed out of 1
"""


class ParseTests(unittest.TestCase):
    def parse(self, extra: str = "") -> dict:
        return rrc.parse_job_log((LOG + extra).splitlines())

    def test_checkout_and_largest_session(self):
        p = self.parse()
        self.assertEqual(p["checkout_sha"], "491da3c2f9351d7ba7da866b8aa028aa6ac0088b")
        self.assertEqual(p["ctest"]["executed"], 5)
        self.assertEqual(p["ctest"]["selected"], 5)
        self.assertTrue(p["ctest"]["complete"])
        self.assertEqual(p["ctest"]["label_exclude"], "validation|^slow$|source-selftest")

    def test_retry_lines_become_attempts_with_the_final_outcome(self):
        tests = {x["test_id"]: x for x in self.parse()["tests"]}
        retried = tests["WorkerPool cold-idles workers without blocking later batches"]
        self.assertEqual((retried["attempts"], retried["outcome"], retried["duration_s"]), (2, "pass", 4.96))
        self.assertEqual(tests["register_font_url: detached worker"]["outcome"], "skipped")
        self.assertEqual(tests["a test whose name says Failed"]["outcome"], "pass")
        self.assertEqual(tests["census"]["outcome"], "timeout")
        self.assertEqual(self.parse()["ctest"]["failed"], 1)

    def test_receipt_verdict_comes_from_the_rendered_notice_not_the_script(self):
        self.assertIsNone(self.parse()["receipt_issued"])
        issued = "2026-09-30T19:20:00.0000000Z ##[notice]exact-tree receipt for " + "a" * 40 + " on base " + "b" * 40 + "\n"
        self.assertIs(self.parse(issued)["receipt_issued"], True)
        refused = "2026-09-30T19:20:00.0000000Z ##[warning]selection too narrow — the merge group will validate in full\n"
        self.assertIs(self.parse(refused)["receipt_issued"], False)

    def test_a_verbatim_repeated_line_is_not_a_retry(self):
        line = "2026-09-30T19:10:16.5818700Z     4/5 Test     #6: a test whose name says Failed ....   Passed    1.00 sec\n"
        self.assertIn(line, LOG)
        tests = {x["test_id"]: x for x in rrc.parse_job_log(LOG.replace(line, line + line).splitlines())["tests"]}
        self.assertEqual(tests["a test whose name says Failed"]["attempts"], 1)

    def test_log_without_ctest_is_not_complete(self):
        p = rrc.parse_job_log(LOG.splitlines()[:2])
        self.assertEqual((p["ctest"]["ran"], p["ctest"]["complete"]), (False, False))


class CollectControlTests(unittest.TestCase):
    def run_main(self, manifest: dict) -> int:
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(rrc.Collector, "collect", return_value=manifest), \
                redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            return rpr.main(["collect", "--since", "2026-09-01", "--out", tmp, "--token", "x"])

    def test_zero_merge_groups_or_pairs_fails_the_control(self):
        self.assertEqual(self.run_main({"merge_groups": 0, "pairs_with_head_run": 0}), 1)
        self.assertEqual(self.run_main({"merge_groups": 5, "pairs_with_head_run": 0}), 1)
        self.assertEqual(self.run_main({"merge_groups": 5, "pairs_with_head_run": 3}), 0)

    def test_stacked_requires_the_base_to_have_landed_before_the_group(self):
        collector = mock.Mock()
        collector._git.return_value = subprocess.CompletedProcess([], 0)
        g = group(base_sha="q1", created_at="2026-09-29T12:00:00Z")
        queue = {"q1": {"pr": 7}}
        self.assertTrue(rrc._stacked(g, queue, {7: "2026-09-29T12:30:00Z"}, collector))
        self.assertFalse(rrc._stacked(g, queue, {7: "2026-09-29T11:30:00Z"}, collector))
        self.assertIsNone(rrc._stacked(g, queue, {}, collector))
        collector._git.return_value = subprocess.CompletedProcess([], 1)
        self.assertTrue(rrc._stacked(g, queue, {7: "2026-09-29T11:30:00Z"}, collector))


class DriftTests(unittest.TestCase):
    def collector(self, local: bool) -> rrc.Collector:
        c = rrc.Collector.__new__(rrc.Collector)
        c.commit = lambda sha: {"tree": "t", "parents": [], "local": local}
        c.prime_commits = lambda shas: None
        c.diff_files = lambda a, b: {("r1", "m1"): ["docs/a.md"], ("b1", "b2"): ["docs/a.md", "core/x.cpp"]}[(a, b)]
        return c

    def test_local_checkouts_diff_the_trees(self):
        self.assertEqual(self.collector(True).drift(head(), group()), (["docs/a.md"], "trees"))

    def test_unfetchable_checkout_falls_back_to_the_base_superset(self):
        self.assertEqual(self.collector(False).drift(head(), group()), (["docs/a.md", "core/x.cpp"], "bases"))

    def test_identical_trees_have_no_drift(self):
        self.assertEqual(self.collector(False).drift(head(merge_tree="t2"), group()), ([], "trees"))

    def test_different_heads_have_no_base_fallback(self):
        other = group(head_sha="h9", checkout_parents=["b2", "h9"])
        self.assertEqual(self.collector(False).drift(head(), other), (None, None))


class RunRecordTests(unittest.TestCase):
    def record(self, parsed: dict, conclusion: str = "failure") -> dict:
        c = rrc.Collector.__new__(rrc.Collector)
        job = {"id": 5, "name": "macos", "runner_name": "gate-vm-1", "conclusion": conclusion}
        c.jobs = lambda run_id: [job]
        c.parsed_log = lambda job_id: parsed
        c.commit = lambda sha: {"tree": "t", "parents": ["b", "h"], "local": True}
        run = {"id": 1, "head_sha": "m1", "created_at": "2026-09-01T00:00:00Z", "updated_at": None, "conclusion": "failure"}
        return c.run_record(run, "merge_group", 7, "h")[0]

    def test_failure_after_checkout_without_ctest_is_a_build_failure(self):
        rec = self.record({"checkout_sha": "m1", "ctest": {"ran": False}, "tests": [], "receipt_issued": None})
        self.assertTrue(rec["build_failed"])
        self.assertEqual(rpr.pair_status(rec), "build_failed")

    def test_failure_before_checkout_ran_nothing(self):
        rec = self.record({"checkout_sha": None, "ctest": {}, "tests": [], "receipt_issued": None})
        self.assertFalse(rec["build_failed"])
        self.assertEqual(rec["checkout_sha"], "m1")
        self.assertEqual(rpr.pair_status(rec), "no_suite")


class LogDownloadTests(unittest.TestCase):
    class Resp:
        def __init__(self, body: bytes, declared: int) -> None:
            self.body, self.headers = body, {"Content-Length": str(declared)}
        def read(self) -> bytes:
            return self.body
        def __enter__(self):
            return self
        def __exit__(self, *exc) -> None:
            return None

    def github(self, responses: list) -> rrc.GitHub:
        gh = rrc.GitHub("o/r", "token")
        gh._request = lambda url, accept: responses.pop(0)
        return gh

    def test_a_short_body_is_an_error_not_a_short_log(self):
        gh = self.github([self.Resp(LOG.encode()[:300], len(LOG.encode()))])
        with self.assertRaises(rrc.TruncatedLog):
            gh.job_log_lines(1)

    def test_a_truncated_download_is_retried_and_never_cached_partial(self):
        full = LOG.encode()
        c = rrc.Collector.__new__(rrc.Collector)
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(rrc.time, "sleep"):
            c.cache = Path(tmp)
            c.gh = self.github([self.Resp(full[:300], len(full)), self.Resp(full, len(full))])
            self.assertEqual(c.parsed_log(7)["ctest"]["executed"], 5)
            c.gh = self.github([self.Resp(full[:300], len(full))] * 4)
            self.assertTrue(c.parsed_log(8)["ctest"]["log_unavailable"])
            self.assertTrue((Path(tmp) / f"logs-v{rrc.PARSER_VERSION}" / "7.json.gz").is_file())
            self.assertFalse((Path(tmp) / f"logs-v{rrc.PARSER_VERSION}" / "8.json.gz").exists())


class SourceKeyClassifyTests(unittest.TestCase):
    MAP = {
        "compiled": {"executables": ["test/a"], "sources": ["test/test_a.cpp"]},
        "other-exe": {"executables": ["test/b"], "sources": ["test/test_b.cpp"]},
        "gpu": {"executables": ["test/c"], "sources": [], "resource_locks": ["pulp_gpu"]},
        "browser": {"executables": ["test/d"], "sources": [], "labels": ["browser-capture"]},
        "renamed": {"executables": ["test/gone"], "sources": []},
        "script": {"labels": []},
        "sealed": {"labels": ["hermetic"]},
        "list-drift": {"labels": []},
    }
    ENTRIES = {"script": {"inputs": ["tools/x"]}, "sealed": {"inputs": ["tools/y"]},
               "list-drift": {"inputs": ["tools/z"]}}
    EXES = {"test/a", "test/b", "test/c", "test/d"}

    def run_sets(self, drift, rebuilt=frozenset(), head=None, group=None):
        out = rrc.classify_source_keys(list(drift), list(self.MAP) + ["unmapped"], self.MAP,
                                       head if head is not None else self.ENTRIES,
                                       group if group is not None else self.ENTRIES, set(rebuilt), self.EXES)
        return {v: set(d["run"]) for v, d in out.items()}, out

    def test_unrelated_drift_skips_mapped_tests_but_never_unknown_ones(self):
        runs, _ = self.run_sets(["docs/guide.md"])
        self.assertEqual(runs["per-entry"], {"renamed", "unmapped"})
        self.assertEqual(runs["strict-data"], {"renamed", "unmapped", "gpu", "browser", "script", "list-drift"})

    def test_a_rebuilt_executable_or_drifted_source_runs_its_tests(self):
        runs, _ = self.run_sets(["core/x.cpp"], rebuilt={"test/a"})
        self.assertIn("compiled", runs["per-entry"])
        self.assertNotIn("other-exe", runs["per-entry"])
        self.assertIn("other-exe", self.run_sets(["test/test_b.cpp"])[0]["per-entry"])

    def test_strict_runs_compiled_tests_on_data_and_everything_on_cmake(self):
        runs, out = self.run_sets(["tools/import-design/fixture.json"])
        self.assertNotIn("compiled", runs["per-entry"])
        self.assertIn("compiled", runs["strict-data"])
        self.assertEqual(out["strict-data"]["executables_rebuilt"], 0)
        runs, out = self.run_sets(["test/cmake/x_tests.cmake"])
        self.assertIn("compiled", runs["strict-data"])
        self.assertEqual(out["strict-data"]["executables_rebuilt"], len(self.EXES))
        self.assertEqual(out["per-entry"]["executables_rebuilt"], 0)

    def test_script_tests_follow_their_own_entry(self):
        runs, _ = self.run_sets(["tools/x/run.py"])
        self.assertIn("script", runs["per-entry"])
        self.assertNotIn("sealed", runs["per-entry"])
        changed = dict(self.ENTRIES, sealed={"inputs": ["tools/y", "tools/new"]})
        self.assertIn("sealed", self.run_sets(["docs/a.md"], group=changed)[0]["per-entry"])

    def test_a_list_edit_reruns_declared_scripts_only_at_list_level(self):
        runs, _ = self.run_sets(["test/ctest_script_inputs.json"])
        self.assertNotIn("script", runs["per-entry"])
        self.assertIn("script", runs["list-level"])

    def test_strict_skips_only_hermetic_scripts_and_never_environment_bound_tests(self):
        runs, _ = self.run_sets(["docs/a.md"])
        self.assertNotIn("sealed", runs["strict-data"])
        self.assertIn("script", runs["strict-data"])
        self.assertTrue({"gpu", "browser"} <= runs["strict-data"])
        self.assertFalse({"gpu", "browser"} & runs["per-entry"])

    def test_an_unreadable_list_makes_script_tests_unknown(self):
        runs, _ = self.run_sets(["docs/a.md"], head={})
        self.assertIn("script", runs["per-entry"])
        out = rrc.classify_source_keys(["docs/a.md"], ["script"], self.MAP, None, self.ENTRIES, set(), self.EXES)
        self.assertEqual(out["per-entry"]["run"], ["script"])


class CodemodelTests(unittest.TestCase):
    HEAD = {
        "lib": {"digest": "L1", "type": "STATIC_LIBRARY", "artifacts": ["<build>/libx.a"], "dependencies": []},
        "a": {"digest": "A1", "type": "EXECUTABLE", "artifacts": ["<build>/test/a"], "dependencies": ["lib"]},
        "b": {"digest": "B1", "type": "EXECUTABLE", "artifacts": ["<build>/test/b"], "dependencies": []},
    }

    def group(self, **digests):
        g = json.loads(json.dumps(self.HEAD))
        for name, d in digests.items():
            g.setdefault(name, {"type": "EXECUTABLE", "artifacts": [f"<build>/test/{name}"], "dependencies": []})
            g[name]["digest"] = d
        return g

    def test_unchanged_codemodel_rekeys_nothing(self):
        self.assertEqual(rrc.codemodel_rekeyed(self.HEAD, self.group()), (set(), {"test/a", "test/b"}))

    def test_a_changed_library_rekeys_its_dependents(self):
        rekeyed, _ = rrc.codemodel_rekeyed(self.HEAD, self.group(lib="L2"))
        self.assertEqual(rekeyed, {"test/a"})

    def test_a_changed_or_new_executable_is_rekeyed(self):
        self.assertEqual(rrc.codemodel_rekeyed(self.HEAD, self.group(b="B2"))[0], {"test/b"})
        self.assertEqual(rrc.codemodel_rekeyed(self.HEAD, self.group(c="C1"))[0], {"test/c"})

    MAP = {"ta": {"executables": ["test/a"], "sources": []}, "tb": {"executables": ["test/b"], "sources": []},
           "tz": {"executables": ["test/z"], "sources": []}}
    EXES = {"test/a", "test/b", "test/z"}

    def classify(self, drift, rekeyed):
        return rrc.classify_source_keys(drift, ["ta", "tb", "tz"], self.MAP, {}, {}, set(), self.EXES,
                                        (rekeyed, {"test/a", "test/b"}))

    def test_a_cmake_change_rekeys_only_what_the_codemodel_moved(self):
        out = self.classify(["test/cmake/x_tests.cmake"], {"test/a"})
        self.assertEqual(set(out["strict-data"]["run"]), {"ta", "tb", "tz"})
        self.assertEqual(set(out["cmake-codemodel"]["run"]), {"ta", "tz"})  # tz: undescribed, keeps strict
        self.assertEqual(out["strict-data"]["executables_rebuilt"], 3)
        self.assertEqual(out["cmake-codemodel"]["executables_rebuilt"], 2)
        self.assertEqual((out["cmake-codemodel"]["described_total"], out["cmake-codemodel"]["described_rebuilt"]), (2, 1))

    def test_without_cmake_drift_an_undescribed_executable_is_not_rebuilt(self):
        out = self.classify(["docs/a.md"], set())
        self.assertEqual(out["cmake-codemodel"]["run"], [])
        self.assertEqual(out["cmake-codemodel"]["executables_rebuilt"], 0)

    def test_the_data_rule_still_holds(self):
        out = self.classify(["tools/fixture.json"], set())
        self.assertEqual(set(out["cmake-codemodel"]["run"]), {"ta", "tb", "tz"})

    def test_a_dependency_pin_bump_rekeys_every_consumer(self):
        for pin in ("tools/deps/manifest.json", "tools/cmake/PulpDependencies.cmake", "tools/cmake/PulpFetchContent.cmake"):
            out = self.classify([pin], set())
            for variant in ("strict-data", "cmake-codemodel"):
                self.assertEqual(set(out[variant]["run"]), {"ta", "tb", "tz"}, (pin, variant))
                self.assertEqual(out[variant]["executables_rebuilt"], 3, (pin, variant))

    def test_a_root_cmakelists_change_rekeys_every_consumer(self):
        out = self.classify(["CMakeLists.txt"], set())
        self.assertEqual(set(out["cmake-codemodel"]["run"]), {"ta", "tb", "tz"})
        self.assertEqual(out["cmake-codemodel"]["executables_rebuilt"], 3)
        out = self.classify(["test/cmake/x_tests.cmake"], set())
        self.assertEqual(set(out["cmake-codemodel"]["run"]), {"tz"})  # a manifest edit stays target-granular

    def test_commit_bound_executables_rebuild_and_rerun_in_every_pair(self):
        out = rrc.classify_source_keys(["docs/a.md"], ["ta", "tb", "tz"], self.MAP, {}, {}, set(), self.EXES,
                                       (set(), {"test/a", "test/b"}), frozenset({"test/b"}))
        for variant in ("strict-data", "cmake-codemodel"):
            self.assertEqual(out[variant]["run"], ["tb"], variant)
            self.assertEqual(out[variant]["executables_rebuilt"], 1, variant)
        self.assertEqual(out["per-entry"]["run"], [])

    def test_a_rebuilt_spawned_executable_reruns_the_test_that_depends_on_it(self):
        # tb's executable depends (add_dependencies) on the tool test/tool;
        # only the tool's source drifted.
        out = rrc.classify_source_keys(["tools/x/tool.cpp"], ["ta", "tb"], self.MAP, {}, {}, {"test/tool"},
                                       self.EXES | {"test/tool"}, (set(), {"test/a", "test/b", "test/tool"}),
                                       spawns=self.spawns(b=["tool"]))
        for variant in ("strict-data", "cmake-codemodel"):
            self.assertEqual(out[variant]["run"], ["tb"], variant)
            self.assertEqual(out[variant]["executables_rebuilt"], 0, variant)  # the tests' own binaries stay
        self.assertEqual(out["per-entry"]["run"], [])

    def test_a_rebuilt_undeclared_tool_reruns_every_compiled_test(self):
        out = rrc.classify_source_keys(["tools/x/tool.cpp"], ["ta", "tb"], self.MAP, {}, {}, {"test/tool"},
                                       self.EXES | {"test/tool"}, (set(), {"test/a", "test/b", "test/tool"}),
                                       spawnable=frozenset({"test/tool"}))
        for variant in ("strict-data", "cmake-codemodel"):
            self.assertEqual(out[variant]["run"], ["ta", "tb"], variant)
            self.assertEqual(out[variant]["spawnable_rebuilt"], ["test/tool"], variant)
        for variant in ("strict-data", "cmake-codemodel"):
            self.assertEqual((out[variant]["spawnable_fallback"], out[variant]["fallback_only_tests"]), (True, 2))
        # The same fallback under a data drift costs nothing: the data rule already reruns them.
        free = rrc.classify_source_keys(["tools/x/tool.cpp", "tools/x/fixture.json"], ["ta", "tb"], self.MAP, {}, {},
                                        {"test/tool"}, self.EXES | {"test/tool"},
                                        (set(), {"test/a", "test/b", "test/tool"}), spawnable=frozenset({"test/tool"}))
        self.assertEqual((free["cmake-codemodel"]["spawnable_fallback"], free["cmake-codemodel"]["fallback_only_tests"]),
                         (True, 0))
        p = dict(pair(), source_key={"cmake-codemodel-recorded": out["cmake-codemodel"]}, source_key_head_run_id="p1")
        result = rpr.score(rpr.Corpus([group(), head()], [p], tests={"g1": [t("ta"), t("tb")], "p1": [t("ta"), t("tb")]}),
                           "source-key-codemodel-recorded")
        self.assertEqual((result["spawnable_fallback_pairs"], result["fallback_only_rerun_pairs"]), (1, 1))
        quiet = rrc.classify_source_keys(["docs/a.md"], ["ta", "tb"], self.MAP, {}, {}, set(),
                                         self.EXES | {"test/tool"}, (set(), {"test/a", "test/b", "test/tool"}),
                                         spawnable=frozenset({"test/tool"}))
        self.assertEqual(quiet["cmake-codemodel"]["run"], [])

    @staticmethod
    def spawns(**deps):
        """A SpawnIndex over test executables a, b and tools test/tool and
        module test/mod.so, with the given add_dependencies edges."""
        targets = {n: {"type": "EXECUTABLE", "artifacts": [f"<build>/test/{n}"], "dependencies": deps.get(n, [])}
                   for n in ("a", "b", "tool")}
        targets["mod"] = {"type": "MODULE_LIBRARY", "artifacts": ["<build>/test/mod.so"], "dependencies": []}
        return SpawnIndex(targets)

    def scanned(self, rebuilt, scan, modules=frozenset(), rebuilt_modules=frozenset(), drift=("tools/x/tool.cpp",)):
        return rrc.classify_source_keys(list(drift), ["ta", "tb"], self.MAP, {}, {}, set(rebuilt),
                                        self.EXES | {"test/tool"}, (set(), {"test/a", "test/b", "test/tool"}),
                                        spawns=self.spawns(), spawnable=frozenset({"test/tool"}), spawn_scan=scan,
                                        modules=frozenset(modules), rebuilt_modules=frozenset(rebuilt_modules))

    @staticmethod
    def scan(scanned, **entries):
        return rrc.spawn_scan_of({"executables_scanned_for": ["data", "spawns"], "executables_scanned": list(scanned),
                                  "executables": dict(entries)})

    def test_a_scanned_clean_executable_skips_the_spawnable_fallback(self):
        out = self.scanned({"test/tool"}, self.scan(["a", "b", "tool"]))
        for variant in ("strict-data", "cmake-codemodel"):
            self.assertEqual(out[variant]["run"], [], variant)
            self.assertEqual((out[variant]["fallback_only_tests"], out[variant]["fallback_only_tests_legacy"]), (0, 2))
            self.assertTrue(out[variant]["spawn_scanned"])

    def test_an_executable_the_scan_did_not_cover_keeps_the_fallback(self):
        out = self.scanned({"test/tool"}, self.scan(["a", "tool"]))       # b added after the scan
        self.assertEqual(out["cmake-codemodel"]["run"], ["tb"])
        self.assertEqual(out["cmake-codemodel"]["fallback_only_tests"], 1)
        quiet = self.scanned(set(), self.scan(["a", "tool"]), drift=("docs/a.md",))
        self.assertEqual(quiet["cmake-codemodel"]["run"], [])             # nothing it could reach was rebuilt

    def test_no_scan_or_an_older_list_keeps_the_fallback_for_every_test(self):
        for scan in (None, rrc.spawn_scan_of({"executables": {}}),
                     rrc.spawn_scan_of({"executables_scanned_for": ["data"], "executables_scanned": ["a", "b"]})):
            out = self.scanned({"test/tool"}, scan)
            self.assertEqual(out["cmake-codemodel"]["run"], ["ta", "tb"], scan)
            self.assertFalse(out["cmake-codemodel"]["spawn_scanned"])

    def test_spawn_states_declared_and_none_are_clean_anything_else_always_runs(self):
        names = ["a", "b", "tool"]
        clean = self.scanned({"test/tool"}, self.scan(names, a={"spawns": "declared"}, b={"spawns": "none"}))
        self.assertEqual(clean["cmake-codemodel"]["run"], [])
        for state in ("undeclared", "untracked", "maybe"):
            out = self.scanned(set(), self.scan(names, b={"spawns": state}), drift=("docs/a.md",))
            self.assertEqual(out["cmake-codemodel"]["run"], ["tb"], state)  # nothing rebuilt, still runs
            self.assertEqual(out["cmake-codemodel"]["spawn_undeclared_tests"], 1, state)
            self.assertEqual(out["per-entry"]["run"], [], state)            # not a strict variant

    @staticmethod
    def data_scan(scanned, **entries):
        """A data scan that detected its declared readers (a known reader `k`
        always rides along so a scan with no other declared entry still
        proves it saw something)."""
        entries = {"k": {"data": "declared", "inputs": ["test/fixtures/k"], "detected_sources": ["test/k.cpp"]},
                   **{n: {"detected_sources": ["test/x.cpp"], **e} for n, e in entries.items()}}
        return rrc.spawn_scan_of({"executables_scanned_for": ["data", "spawns"], "executables_scanned": list(scanned),
                                  "executables": entries}, "data")

    def test_a_data_scan_that_cannot_show_it_saw_its_readers_is_not_trusted(self):
        def doc(*detected):
            return {"executables_scanned_for": ["data"], "executables_scanned": ["a", "b"],
                    "executables": {f"r{i}": {"data": "declared", "inputs": ["x"], "detected_sources": d}
                                    for i, d in enumerate(detected)}}
        self.assertIsNotNone(rrc.spawn_scan_of(doc(["s"]) , "data"))
        self.assertIsNone(rrc.spawn_scan_of(doc(*[[]] * 3), "data"))            # blind: nothing detected
        self.assertIsNone(rrc.spawn_scan_of(doc(*[["s"]] * 5, []), "data"))     # 5 of 6 is below the share
        self.assertIsNotNone(rrc.spawn_scan_of(doc(*[["s"]] * 6, []), "data"))  # 6 of 7 meets it
        legacy = doc(["s"])
        del legacy["executables"]["r0"]["detected_sources"]
        self.assertIsNone(rrc.spawn_scan_of(legacy, "data"))                    # the list cannot show it
        self.assertIsNone(rrc.spawn_scan_of({**doc(), "executables": {}}, "data"))  # no known reader at all
        self.assertIsNotNone(rrc.spawn_scan_of({**doc(), "executables": {}, "executables_scanned_for": ["spawns"]},
                                               "spawns"))                       # the spawn scan is not gated

    def test_the_data_manifest_scopes_the_data_rule_per_executable(self):
        drift = ["tools/scripts/foo.py"]   # runtime surface, not a declared input of either test
        scan = self.data_scan(["a", "b", "tool"], a={"data": "declared", "inputs": ["test/fixtures/a"]})

        def run(scan, drift=drift):
            return rrc.classify_source_keys(drift, ["ta", "tb"], self.MAP, {}, {}, set(), self.EXES | {"test/tool"},
                                            (set(), {"test/a", "test/b", "test/tool"}), data_scan=scan)
        out = run(scan)
        self.assertEqual(out["cmake-codemodel"]["run"], ["ta", "tb"])      # the broad rule: every compiled test
        self.assertEqual(out["manifest-data"]["run"], [])
        self.assertTrue(out["manifest-data"]["data_scanned"])
        self.assertEqual(run(scan, ["test/fixtures/a/x.json"])["manifest-data"]["run"], ["ta"])  # a declared input
        undeclared = self.data_scan(["a", "b"], b={"data": "undeclared", "inputs": []})
        self.assertEqual(run(undeclared)["manifest-data"]["run"], ["tb"])  # undeclared reads: any surface drift
        self.assertEqual(run(self.data_scan(["a"]))["manifest-data"]["run"], ["tb"])  # b not scanned
        self.assertEqual(run(None)["manifest-data"]["run"], ["ta", "tb"])  # no scan: the broad rule
        older = rrc.spawn_scan_of({"executables_scanned_for": ["spawns"], "executables_scanned": ["a", "b"]}, "data")
        self.assertIsNone(older)
        odd = self.data_scan(["a", "b"], a={"data": "partly"})
        self.assertEqual(run(odd)["manifest-data"]["run"], ["ta"])         # an unknown state fails closed
        self.assertEqual(run(scan, ["docs/a.md"])["manifest-data"]["run"], [])

    def outputs(self, drift, hashed, changed, rebuilt=frozenset(), **kw):
        return rrc.classify_source_keys(list(drift), ["ta", "tb"], self.MAP, {}, {}, set(rebuilt), self.EXES | {"test/tool"},
                                        (set(), {"test/a", "test/b", "test/tool"}),
                                        outputs=(frozenset(hashed), frozenset(changed)), **kw)

    def test_output_key_rebuilds_only_executables_whose_bytes_changed(self):
        hashed = {"test/a", "test/b", "test/tool"}
        out = self.outputs(["CMakeLists.txt"], hashed, {"test/b"})   # a version stamp re-keys everything
        self.assertEqual(out["cmake-codemodel"]["run"], ["ta", "tb"])
        self.assertEqual(out["output-key"]["run"], ["tb"])
        self.assertEqual(self.outputs(["CMakeLists.txt"], hashed, set())["output-key"]["run"], [])
        # Commit-bound by the learned list, yet the bytes came out the same.
        bound = self.outputs(["docs/a.md"], hashed, set(), commit_bound=frozenset({"test/a"}))
        self.assertEqual((bound["cmake-codemodel"]["run"], bound["output-key"]["run"]), (["ta"], []))

    def test_output_key_keeps_the_source_key_where_no_hash_was_recorded(self):
        out = self.outputs(["CMakeLists.txt"], {"test/b"}, set())
        self.assertEqual(out["output-key"]["run"], ["ta"])            # a has no hash: the stamp still reaches it
        source_only = self.outputs(["test/test_a.cpp"], {"test/b"}, set(), rebuilt={"test/a", "test/b"})
        self.assertEqual(source_only["output-key"]["run"], ["ta"])    # a: its source reached it; b: same bytes
        none = rrc.classify_source_keys(["CMakeLists.txt"], ["ta"], self.MAP, {}, {}, set(), self.EXES,
                                        (set(), {"test/a"}))
        self.assertNotIn("output-key", none)                          # no hashes: no variant

    def test_output_key_reruns_a_test_whose_spawned_tool_changed_bytes(self):
        out = self.outputs(["docs/a.md"], {"test/a", "test/b", "test/tool"}, {"test/tool"},
                           spawns=self.spawns(b=["tool"]), spawnable=frozenset({"test/tool"}),
                           spawn_scan=self.scan(["a", "b", "tool"]))
        self.assertEqual(out["output-key"]["run"], ["tb"])
        self.assertEqual(out["output-key"]["executables_rebuilt"], 0)  # the tests' own binaries are unchanged

    def test_recorded_outputs_compare_only_executables_both_jobs_hashed(self):
        self.assertEqual(rrc.recorded_outputs({"a": "1", "b": "2", "c": "3"}, {"a": "1", "b": "9"}, ["a", "b", "c", "d"]),
                         (frozenset({"a", "b"}), frozenset({"b"})))
        self.assertIsNone(rrc.recorded_outputs(None, {"a": "1"}, ["a"]))

    def test_a_rebuilt_module_triggers_the_fallback_and_reaches_its_loader(self):
        scan = self.scan(["a", "tool"])
        out = self.scanned(set(), scan, modules={"test/mod.so"}, rebuilt_modules={"test/mod.so"})
        self.assertEqual(out["cmake-codemodel"]["run"], ["tb"])           # b unscanned: a module may reach it
        self.assertEqual(out["cmake-codemodel"]["spawnable_rebuilt"], ["test/mod.so"])
        idle = self.scanned(set(), scan, modules={"test/mod.so"})
        self.assertEqual(idle["cmake-codemodel"]["run"], [])
        loader = rrc.classify_source_keys(["docs/a.md"], ["ta", "tb"], self.MAP, {}, {}, set(),
                                          self.EXES | {"test/tool"}, (set(), {"test/a", "test/b", "test/tool"}),
                                          spawns=self.spawns(a=["mod"]), spawn_scan=self.scan(["a", "b", "tool"]),
                                          modules=frozenset({"test/mod.so"}),
                                          rebuilt_modules=frozenset({"test/mod.so"}))
        self.assertEqual(loader["cmake-codemodel"]["run"], ["ta"])          # its closure holds the module
        widened = self.scanned(set(), self.scan(["a", "tool"]), modules={"test/mod.so"},
                               drift=("CMakeLists.txt", "cmake/x.cmake"))
        self.assertIn("test/mod.so", widened["strict-data"]["spawnable_rebuilt"])  # a CMake change rebuilds all

    def test_an_absent_input_is_hit_by_its_creation(self):
        self.assertTrue(rrc._declared_hit(["!pulp.toml"], ["pulp.toml"]))
        self.assertFalse(rrc._declared_hit(["!pulp.toml"], ["docs/pulp.toml.md"]))

    def test_no_codemodel_writes_no_variant(self):
        out = rrc.classify_source_keys([], ["ta"], self.MAP, {}, {}, set(), self.EXES)
        self.assertNotIn("cmake-codemodel", out)

    def test_codemodel_targets_read_from_the_reuse_record_artifact(self):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as zf:
            zf.writestr("codemodel-abc.json", json.dumps({"targets": {"a": dict(self.HEAD["a"], sources="s")}}))
            zf.writestr("job.json", "{}")
        gh = mock.Mock()
        gh.repository = "o/r"
        gh.json.return_value = {"artifacts": [{"name": "reuse-record-macos", "expired": False,
                                               "archive_download_url": "https://x/zip"}]}
        resp = mock.MagicMock()
        resp.__enter__.return_value.read.return_value = buf.getvalue()
        gh._request.return_value = resp
        c = rrc.Collector.__new__(rrc.Collector)
        with tempfile.TemporaryDirectory() as tmp:
            c.cache, c.gh = Path(tmp), gh
            self.assertEqual(c.codemodel_targets("9"), {"a": self.HEAD["a"]})
            gh.json.return_value = {"artifacts": []}
            self.assertIsNone(c.codemodel_targets("10"))


class FakeGraph:
    """A Ninja graph in the affected_tests_shadow shape: sources and headers
    map to objects, objects to archives."""
    def __init__(self, root: str, build: str) -> None:
        self.root, self.build_dir = root, build
        self.src_to_out = {
            f"{root}/core/a.cpp": {"core/CMakeFiles/a.dir/a.cpp.o"},
            f"{root}/core/a.hpp": {"core/CMakeFiles/a.dir/a.cpp.o"},
            f"{root}/core/b.cpp": {"core/CMakeFiles/a.dir/b.cpp.o"},
            f"{root}/test/t.cpp": {"test/CMakeFiles/old-name.dir/t.cpp.o"},
        }
        self.fwd = {"core/CMakeFiles/a.dir/a.cpp.o": {"core/liba.a"},
                    "core/CMakeFiles/a.dir/b.cpp.o": {"core/liba.a"}}

    def norm(self, p: str) -> str:
        return p if os.path.isabs(p) else os.path.join(self.build_dir, p)

    def affected_outputs(self, changed: list[str]) -> set[str]:
        return {self.norm(o) for f in changed for o in self.src_to_out.get(f, ())}


class RecordedGraphTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.root, self.build = os.path.join(self.tmp, "src"), os.path.join(self.tmp, "build")
        os.makedirs(self.root), os.makedirs(self.build)
        self.index = rrc.GraphIndex(FakeGraph(os.path.realpath(self.root), os.path.realpath(self.build)),
                                    Path(self.root), Path(self.build))
        # Executables named as the CURRENT build names them; the graph knew
        # the test source under another target.
        self.link = {
            "test/group-a": {"objects": ["test/CMakeFiles/group-a.dir/t.cpp.o"], "members": {"core/liba.a": ["a.cpp.o"]}},
            "test/group-b": {"objects": ["test/CMakeFiles/group-b.dir/new_test.cpp.o"],
                             "members": {"core/liba.a": ["b.cpp.o"], "/sdk/libskia.a": ["x.o"]}},
            "test/group-c": {"objects": [], "members": {"core/liba.a": ["added_since.cpp.o"]}},
        }

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmp)

    def test_object_paths_name_their_sources(self):
        self.assertEqual(rrc.object_source("test/CMakeFiles/x.dir/support/w.cpp.o"), "test/support/w.cpp")
        self.assertEqual(rrc.object_source("tools/cli/CMakeFiles/x.dir/__/__/core/a.cpp.o"), "core/a.cpp")
        self.assertIsNone(rrc.object_source("CMakeFiles/x.dir/__/__/outside.cpp.o"))
        self.assertIsNone(rrc.object_source("libfoo.a"))

    def test_a_header_reaches_executables_through_the_members_they_pulled(self):
        self.assertEqual(rrc.recorded_rebuilt(self.link, ["core/a.hpp"], self.index),
                         {"test/group-a", "test/group-b", "test/group-c"})  # b, c: header drift reaches unknown sources

    def test_a_source_reaches_only_its_own_objects_under_a_new_executable_name(self):
        self.assertEqual(rrc.recorded_rebuilt(self.link, ["test/t.cpp"], self.index), {"test/group-a"})
        self.assertEqual(rrc.recorded_rebuilt(self.link, ["core/b.cpp"], self.index), {"test/group-b"})

    def test_an_unknown_member_or_object_is_reached_by_its_own_source(self):
        self.assertEqual(rrc.recorded_rebuilt(self.link, ["core/added_since.cpp"], self.index), {"test/group-c"})
        self.assertEqual(rrc.recorded_rebuilt(self.link, ["test/new_test.cpp"], self.index), {"test/group-b"})

    def test_docs_and_pinned_archives_rebuild_nothing(self):
        self.assertEqual(rrc.recorded_rebuilt(self.link, ["docs/a.md", "tools/x.json"], self.index), set())

    def test_annotate_writes_recorded_variants_from_the_group_record(self):
        import gzip
        corpus = Path(self.tmp) / "corpus"
        rpr.write_jsonl(corpus / "runs.jsonl", [group(), head()])
        p = pair(drift=("test/t.cpp",))
        rpr.write_jsonl(corpus / "pairs.jsonl", [p])
        rpr.write_jsonl(corpus / "tests" / "g1.jsonl.gz", [t("ta"), t("tb")])
        targets = {"x": {"digest": "d", "type": "EXECUTABLE", "artifacts": ["<build>/test/group-a"], "dependencies": []},
                   "y": {"digest": "e", "type": "EXECUTABLE", "artifacts": ["<build>/test/group-b"], "dependencies": []}}
        cache = corpus / "cache" / "record-v5"
        cache.mkdir(parents=True)
        # group-a is rebuilt but its bytes came out the same (over-approximation);
        # group-b's bytes changed but nothing in the drift reaches it: the
        # control must name it.
        for run_id, rec in (("p1", {"targets": targets, "link": None, "executables": None,
                                    "binaries": {"test/group-a": "1", "test/group-b": "2", "test/group-c": "3"}}),
                            ("g1", {"targets": targets, "link": self.link,
                                    "executables": {"ta": "test/group-a", "tb": "test/group-b"},
                                    "binaries": {"test/group-a": "1", "test/group-b": "2x", "test/group-c": "3"}})):
            with gzip.open(cache / f"{run_id}.json.gz", "wt") as fh:
                json.dump(rec, fh)
        graph = self.index.graph
        result = rrc.annotate_source_keys(corpus, Path(self.tmp), graph, Path(self.root), Path(self.build),
                                          {}, None, mock.Mock())
        self.assertEqual(result["commit_bound_executables"], [])  # no pair checked out the same tree
        self.assertEqual(result["pairs_with_recorded_graph"], 1)
        keys = next(rpr.read_jsonl(corpus / "pairs.jsonl"))["source_key"]
        self.assertEqual(keys["cmake-codemodel-recorded"]["run"], ["ta"])  # t.cpp drifted: group-a only
        self.assertEqual(keys["cmake-codemodel-recorded"]["unreached_changed_binaries"], ["test/group-b"])
        self.assertEqual(keys["cmake-codemodel-recorded"]["unreached_kinds"], {"test/group-b": "backs_test"})
        self.assertEqual(keys["cmake-codemodel-recorded"]["binaries_compared"], 3)
        self.assertEqual(keys["cmake-codemodel-recorded"]["rebuilt_identical_binaries"], 1)
        p2 = dict(pair(drift=("test/t.cpp",)), source_key=keys, source_key_head_run_id="p1")
        result = rpr.score(rpr.Corpus([group(), head()], [p2], tests={"g1": [t("ta"), t("tb")], "p1": [t("ta"), t("tb")]}),
                           "source-key-codemodel-recorded")
        self.assertEqual((result["unreached_changed_binaries"], result["verdict"]), (1, "UNSAFE: changed binaries not rebuilt"))
        self.assertEqual((keys["cmake-codemodel-recorded"]["executables_rebuilt"],
                          keys["cmake-codemodel-recorded"]["executables_total"]), (1, 2))  # only executables the tests run

    def test_the_binary_control_covers_rebuilt_executables_no_test_runs(self):
        """A changed binary no group test runs (a spawned tool, a bench) is
        checked against everything the variant rebuilds, not only the
        executables behind the group's tests."""
        import gzip
        targets = {n: {"digest": "d", "type": "EXECUTABLE", "artifacts": [f"<build>/test/{n}"], "dependencies": []}
                   for n in ("group-a", "group-b", "group-c")}

        def unreached(drift, run_id):
            corpus = Path(self.tmp) / f"ctl-{run_id}"
            rpr.write_jsonl(corpus / "runs.jsonl", [group(), head()])
            rpr.write_jsonl(corpus / "pairs.jsonl", [pair(drift=drift)])
            rpr.write_jsonl(corpus / "tests" / "g1.jsonl.gz", [t("ta")])
            cache = corpus / "cache" / "record-v5"
            cache.mkdir(parents=True)
            for rid, rec in (("p1", {"link": None, "executables": None, "binaries": {"test/group-c": "3"}}),
                             ("g1", {"link": self.link, "executables": {"ta": "test/group-a"},
                                     "binaries": {"test/group-c": "3x"}})):
                with gzip.open(cache / f"{rid}.json.gz", "wt") as fh:
                    json.dump({"targets": targets, **rec}, fh)
            rrc.annotate_source_keys(corpus, Path(self.tmp), self.index.graph, Path(self.root), Path(self.build),
                                     {}, None, mock.Mock())
            return next(rpr.read_jsonl(corpus / "pairs.jsonl"))["source_key"]["cmake-codemodel-recorded"][
                "unreached_changed_binaries"]
        self.assertEqual(unreached(("core/added_since.cpp",), "reached"), [])        # rebuilt, no test runs it
        self.assertEqual(unreached(("test/t.cpp",), "missed"), ["test/group-c"])     # changed and not rebuilt
        kinds = next(rpr.read_jsonl(Path(self.tmp) / "ctl-missed" / "pairs.jsonl"))["source_key"][
            "cmake-codemodel-recorded"]["unreached_kinds"]
        self.assertEqual(kinds, {"test/group-c": "neither"})
        # The same miss labelled by what reaches it: a test spawning it, or a test running it.
        targets["group-a"]["dependencies"] = ["group-c"]
        unreached(("test/t.cpp",), "spawned")
        self.assertEqual(next(rpr.read_jsonl(Path(self.tmp) / "ctl-spawned" / "pairs.jsonl"))["source_key"][
            "cmake-codemodel-recorded"]["unreached_kinds"], {"test/group-c": "spawnable"})

    def test_a_recorded_module_is_rebuilt_only_by_drift_its_link_reaches(self):
        import gzip
        targets = {n: {"digest": "d", "type": "EXECUTABLE", "artifacts": [f"<build>/test/{n}"], "dependencies": []}
                   for n in ("group-a", "group-b")}
        targets["plug"] = {"digest": "m", "type": "MODULE_LIBRARY", "artifacts": ["<build>/test/plug.so"],
                           "dependencies": []}

        def rebuilt_programs(drift, recorded, label):
            corpus = Path(self.tmp) / f"mod-{label}"
            rpr.write_jsonl(corpus / "runs.jsonl", [group(), head()])
            rpr.write_jsonl(corpus / "pairs.jsonl", [pair(drift=drift)])
            rpr.write_jsonl(corpus / "tests" / "g1.jsonl.gz", [t("ta")])
            link = dict(self.link)
            if recorded:
                link["test/plug.so"] = {"objects": [], "members": {"core/liba.a": ["b.cpp.o"]}}
            cache = corpus / "cache" / "record-v5"
            cache.mkdir(parents=True)
            for rid, rec in (("p1", {"link": None, "executables": None, "binaries": None}),
                             ("g1", {"link": link, "executables": {"ta": "test/group-a"}, "binaries": None})):
                with gzip.open(cache / f"{rid}.json.gz", "wt") as fh:
                    json.dump({"targets": targets, **rec}, fh)
            rrc.annotate_source_keys(corpus, Path(self.tmp), self.index.graph, Path(self.root), Path(self.build),
                                     {}, None, mock.Mock())
            return next(rpr.read_jsonl(corpus / "pairs.jsonl"))["source_key"]["cmake-codemodel-recorded"][
                "spawnable_rebuilt"]
        self.assertIn("test/plug.so", rebuilt_programs(("core/b.cpp",), True, "member"))
        self.assertNotIn("test/plug.so", rebuilt_programs(("test/t.cpp",), True, "elsewhere"))
        # Control: the same drift with the module's link unrecorded fails closed.
        self.assertIn("test/plug.so", rebuilt_programs(("test/t.cpp",), False, "unrecorded"))

    def test_commit_bound_executables_are_learned_from_same_tree_pairs(self):
        import gzip
        corpus = Path(self.tmp) / "learn"
        g2 = group(run_id="g2", group_sha="m2", checkout_sha="m2")
        rpr.write_jsonl(corpus / "runs.jsonl", [group(), g2, head()])
        same = pair(drift=())
        moved = dict(pair(drift=("test/t.cpp",)), group_run_id="g2")
        rpr.write_jsonl(corpus / "pairs.jsonl", [same, moved])
        rpr.write_jsonl(corpus / "tests" / "g1.jsonl.gz", [t("ta")])
        rpr.write_jsonl(corpus / "tests" / "g2.jsonl.gz", [t("ta"), t("tb")])
        targets = {"x": {"digest": "d", "type": "EXECUTABLE", "artifacts": ["<build>/test/group-a"], "dependencies": []},
                   "y": {"digest": "e", "type": "EXECUTABLE", "artifacts": ["<build>/test/group-b"], "dependencies": []}}
        cache = corpus / "cache" / "record-v5"
        cache.mkdir(parents=True)
        recs = {"p1": {"binaries": {"test/group-a": "1", "test/group-b": "2"}, "link": None, "executables": None},
                "g1": {"binaries": {"test/group-a": "1", "test/group-b": "2-stamped"}, "link": self.link,
                       "executables": {"ta": "test/group-a"}},
                "g2": {"binaries": {"test/group-a": "1x", "test/group-b": "2-stamped-again"}, "link": self.link,
                       "executables": {"ta": "test/group-a", "tb": "test/group-b"}}}
        for run_id, rec in recs.items():
            with gzip.open(cache / f"{run_id}.json.gz", "wt") as fh:
                json.dump({"targets": targets, **rec}, fh)
        result = rrc.annotate_source_keys(corpus, Path(self.tmp), self.index.graph, Path(self.root), Path(self.build),
                                          {}, None, mock.Mock())
        self.assertEqual(result["commit_bound_executables"], ["test/group-b"])
        # g1 checked out the same tree: group-b is rebuilt (commit-bound) and
        # its bytes did change, so nothing was over-approximated there; g2's
        # group-a changed and was rebuilt too.
        over = {p["group_run_id"]: p["source_key"]["cmake-codemodel-recorded"]["rebuilt_identical_binaries"]
                for p in rpr.read_jsonl(corpus / "pairs.jsonl")}
        self.assertEqual(over, {"g1": 0, "g2": 0})
        keys = {p["group_run_id"]: p["source_key"] for p in rpr.read_jsonl(corpus / "pairs.jsonl")}
        self.assertIn("tb", keys["g2"]["cmake-codemodel-recorded"]["run"])
        self.assertEqual(keys["g2"]["cmake-codemodel-recorded"]["unreached_changed_binaries"], [])

    def annotate_v2(self, drift, head_schema, group_schema, declared=("test/group-c",), headers="ninja-deps",
                    legacy=False):
        """One pair whose head and group records carry the given codemodel
        schemas; the group declares `declared` commit-bound."""
        import gzip
        corpus = Path(self.tmp) / f"v2-{head_schema}-{group_schema}-{headers}-{legacy}-{len(drift)}"
        rpr.write_jsonl(corpus / "runs.jsonl", [group(), head()])
        rpr.write_jsonl(corpus / "pairs.jsonl", [pair(drift=drift)])
        rpr.write_jsonl(corpus / "tests" / "g1.jsonl.gz", [t("ta"), t("tb"), t("tc")])
        targets = {n: {"digest": "d", "type": "EXECUTABLE", "artifacts": [f"<build>/test/{n}"], "dependencies": []}
                   for n in ("group-a", "group-b", "group-c")}
        cache = corpus / "cache" / "record-v5"
        cache.mkdir(parents=True)
        common = {"targets": targets, "binaries": None, "generated_headers": headers}
        recs = {"p1": {**common, "link": None, "executables": None, "digest_schema": head_schema,
                       "declared_commit_bound": None},
                "g1": {**common, "link": self.link, "digest_schema": group_schema,
                       "executables": {"ta": "test/group-a", "tb": "test/group-b", "tc": "test/group-c"},
                       "declared_commit_bound": list(declared)}}
        for run_id, rec in recs.items():
            with gzip.open(cache / f"{run_id}.json.gz", "wt") as fh:
                json.dump(rec, fh)
        result = rrc.annotate_source_keys(corpus, Path(self.tmp), self.index.graph, Path(self.root), Path(self.build),
                                          {}, None, mock.Mock(), legacy)
        keys = next(rpr.read_jsonl(corpus / "pairs.jsonl"))["source_key"]["cmake-codemodel-recorded"]
        return result, keys

    def test_content_keyed_records_retire_the_root_cmakelists_rule(self):
        v2 = "pulp-codemodel-digest/v2"
        result, keys = self.annotate_v2(("CMakeLists.txt",), v2, v2)
        self.assertEqual(result["pairs_content_keyed"], 1)
        self.assertEqual(keys["run"], ["tc"])                 # only the declared commit-bound one
        self.assertEqual(keys["executables_rebuilt"], 1)

    def test_v1_mixed_and_unkeyed_records_keep_the_blunt_rules(self):
        v1, v2 = "pulp-codemodel-digest/v1", "pulp-codemodel-digest/v2"
        for head_schema, group_schema, headers, legacy in ((v1, v1, None, False), (v1, v2, "ninja-deps", False),
                                                           (v2, v2, "unavailable", False), (v2, v2, "ninja-deps", True)):
            result, keys = self.annotate_v2(("CMakeLists.txt",), head_schema, group_schema, headers=headers,
                                            legacy=legacy)
            self.assertEqual(result["pairs_content_keyed"], 0, (head_schema, group_schema, headers, legacy))
            self.assertEqual(set(keys["run"]), {"ta", "tb", "tc"}, (head_schema, group_schema, headers, legacy))

    def test_declared_commit_bound_replaces_the_learned_set(self):
        v2 = "pulp-codemodel-digest/v2"
        _, keys = self.annotate_v2(("docs/a.md",), v2, v2, declared=("test/group-b",))
        self.assertEqual(keys["run"], ["tb"])

    def record_from(self, files: dict) -> dict:
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as zf:
            for name, body in files.items():
                zf.writestr(name, body)
        gh = mock.Mock()
        gh.repository = "o/r"
        gh.json.return_value = {"artifacts": [{"name": "reuse-record-macos", "archive_download_url": "u"}]}
        resp = mock.MagicMock()
        resp.__enter__.return_value.read.return_value = buf.getvalue()
        gh._request.return_value = resp
        c = rrc.Collector.__new__(rrc.Collector)
        c.cache, c.gh = Path(self.tmp) / f"rec-{len(files)}-{id(files)}", gh
        return c.reuse_record("6")

    def test_the_record_reads_commit_bound_from_tests_and_the_codemodel(self):
        model = {"schema": "pulp-codemodel-digest/v2", "generated_headers": "ninja-deps",
                 "commit_bound_declared": ["stamp"],
                 "targets": {"z": {"type": "EXECUTABLE", "artifacts": ["<build>/test/z"], "commit_bound": True},
                             "y": {"type": "EXECUTABLE", "artifacts": ["<build>/test/y"], "commit_bound": False}}}
        rows = [{"test_id": "x case", "executable": "<build>/test/x", "commit_bound": True},
                {"test_id": "y", "executable": "<build>/test/y", "commit_bound": False}]
        rec = self.record_from({"codemodel-abc.json": json.dumps(model),
                                "tests.jsonl": "\n".join(json.dumps(r) for r in rows)})
        self.assertEqual(rec["declared_commit_bound"], ["test/x", "test/z"])
        self.assertTrue(rrc.content_keyed(rec))
        self.assertFalse(rrc.content_keyed(dict(rec, generated_headers="unavailable")))

    def test_an_undeclared_build_or_a_null_verdict_declares_nothing(self):
        model = {"schema": "pulp-codemodel-digest/v2", "generated_headers": "ninja-deps", "targets": {}}
        unavailable = dict(model, commit_bound_declared="unavailable")
        row = json.dumps({"test_id": "x", "executable": "<build>/test/x", "commit_bound": True})
        self.assertIsNone(self.record_from({"codemodel-a.json": json.dumps(unavailable),
                                            "tests.jsonl": row})["declared_commit_bound"])
        null_row = json.dumps({"test_id": "x", "executable": "<build>/test/x", "commit_bound": None})
        self.assertIsNone(self.record_from({"codemodel-a.json": json.dumps(dict(model, commit_bound_declared=[])),
                                            "tests.jsonl": null_row, "job.json": "{}"})["declared_commit_bound"])

    def test_the_record_expands_link_members_and_test_executables(self):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as zf:
            zf.writestr("link-members-abc.json", json.dumps({
                "schema": "pulp-link-members/v1",
                "members": {"<build>/core/liba.a": ["a.cpp.o", "b.cpp.o"]},
                "executables": {"<build>/test/x": {"objects": ["<build>/test/CMakeFiles/x.dir/t.cpp.o"],
                                                   "archives": {"<build>/core/liba.a": {"members": [1], "whole": False}}},
                                "<build>/test/y": {"objects": [],
                                                   "archives": {"<build>/core/liba.a": {"members": [], "whole": True}}}}}))
            zf.writestr("tests.jsonl", json.dumps({"test_id": "t1", "executable": "<build>/test/x"}) + "\n")
        gh = mock.Mock()
        gh.repository = "o/r"
        gh.json.return_value = {"artifacts": [{"name": "reuse-record-macos", "archive_download_url": "u"}]}
        resp = mock.MagicMock()
        resp.__enter__.return_value.read.return_value = buf.getvalue()
        gh._request.return_value = resp
        c = rrc.Collector.__new__(rrc.Collector)
        c.cache, c.gh = Path(self.tmp), gh
        rec = c.reuse_record("5")
        self.assertIsNone(rec["targets"])
        self.assertEqual(rec["executables"], {"t1": "test/x"})
        self.assertEqual(rec["link"]["test/x"], {"objects": ["test/CMakeFiles/x.dir/t.cpp.o"], "members": {"core/liba.a": ["b.cpp.o"]}})
        self.assertEqual(rec["link"]["test/y"]["members"], {"core/liba.a": ["a.cpp.o", "b.cpp.o"]})

    def test_a_link_record_the_reader_cannot_vouch_for_is_no_record(self):
        # An unknown schema, a kind the replay does not model (a shared library
        # changes what loads without changing the loader's map) or an
        # unrecorded link all read as "no link record", so every executable
        # falls back to the blunt rules instead of trusting part of the map.
        def links(schema="pulp-link-members/v3", kind="module", **extra):
            return json.dumps({"schema": schema, "members": {}, "unreadable": 0, **extra,
                               "executables": {"<build>/p.so": {"kind": kind, "objects": ["<build>/p.o"],
                                                                "archives": {}}}})
        # Held for the whole test: record_from keys its cache on id(files).
        files = [{"link-members-a.json": doc} for doc in (
            links(), links("pulp-link-members/v4"), links(None), links(kind="shared"), links(unrecorded=1))]
        self.assertEqual(self.record_from(files[0])["link"], {"p.so": {"objects": ["p.o"], "members": {}}})
        for f in files[1:]:
            self.assertIsNone(self.record_from(f)["link"], f)


class SourceKeyPolicyTests(unittest.TestCase):
    def corpus(self, head_tests, run, rebuilt=1, total=4):
        p = pair()
        p["source_key"] = {"strict-data": {"run": run, "executables_total": total, "executables_rebuilt": rebuilt}}
        p["source_key_head_run_id"] = "p1"
        return rpr.Corpus([group(), head()], [p],
                          tests={"g1": [t("a", "fail", 2), t("b"), t("c")], "p1": head_tests})

    def test_skips_unchanged_tests_the_head_passed_first_time(self):
        c = self.corpus([t("a"), t("b"), t("c", "pass", 2)], run=[])
        result = rpr.score(c, "source-key")
        self.assertEqual(result["false_skips"], 1)          # a: unchanged, passed on the head, failed here
        self.assertEqual(result["skipped_test_seconds"], 20.0)  # c passed only on retry: not evidence
        self.assertEqual(result["build_skipped_pooled"], 0.75)

    def test_a_test_in_the_run_set_or_absent_from_the_head_runs(self):
        result = rpr.score(self.corpus([t("b")], run=["b"]), "source-key")
        self.assertEqual((result["false_skips"], result["skipped_test_seconds"]), (0, 0.0))

    def test_no_reconstructed_key_is_unevaluable(self):
        c = rpr.Corpus([group(), head()], [pair()], tests={"g1": [t("a", "fail", 2)]})
        result = rpr.score(c, "source-key")
        self.assertEqual((result["evaluable_pairs"], result["false_skips"]), (0, 0))


class RecordCoverageTests(unittest.TestCase):
    def test_executed_jobs_without_a_record_are_named_and_cancelled_ones_are_not(self):
        rec = "Record per-test results for reuse replay (macOS)"
        ran = [{"name": "Build", "conclusion": "success"}, {"name": "Test (non-Windows)", "conclusion": "failure"},
               {"name": rec, "conclusion": "success"}]
        lost = [{"name": "Build", "conclusion": None}, {"name": "Test (non-Windows)", "conclusion": None},
                {"name": rec, "conclusion": None}]
        lost_after_tests = [{"name": "Build", "conclusion": "success"},
                            {"name": "Test fast deterministic tier (pull request head)", "conclusion": "success"},
                            {"name": "Test (non-Windows)", "conclusion": None}, {"name": rec, "conclusion": None}]
        jobs = {
            1: [{"id": 11, "name": "macos", "runner_name": "gate-vm", "conclusion": "success", "steps": ran}],
            2: [{"id": 12, "name": "macos", "runner_name": "gate-vm", "conclusion": "failure", "steps": ran}],
            3: [{"id": 13, "name": "macos", "runner_name": None, "conclusion": "cancelled", "steps": []}],
            4: [{"id": 14, "name": "macos", "runner_name": "gate-vm", "conclusion": "success", "steps": ran}],
            5: [{"id": 15, "name": "macos", "runner_name": "gate-vm", "conclusion": "success", "steps": ran}],
            6: [{"id": 16, "name": "macos", "runner_name": "gate-vm", "conclusion": "failure", "steps": lost}],
            7: [{"id": 17, "name": "macos", "runner_name": "gate-vm", "conclusion": "failure",
                 "steps": lost_after_tests}],
        }
        artifacts = {1: ["reuse-record-macos"], 2: ["ctest-logs-macos"], 3: [], 4: ["reuse-record-macos-attempt-2"],
                     5: [], 6: [], 7: []}
        c = rrc.Collector.__new__(rrc.Collector)
        c.jobs = lambda run_id: jobs[run_id]
        gh = mock.Mock()
        gh.repository = "o/r"
        gh.json.side_effect = lambda path: {"artifacts": [{"name": n} for n in artifacts[int(path.split("/runs/")[1].split("/")[0])]]}
        with tempfile.TemporaryDirectory() as tmp:
            c.cache, c.gh = Path(tmp), gh
            runs = [{"id": i, "status": "completed", "event": "pull_request", "created_at": "2026-10-02T12:00:00Z"}
                    for i in (1, 2, 3, 4, 6, 7)]
            runs.append({"id": 5, "status": "completed", "event": "merge_group", "created_at": "2026-09-30T12:00:00Z"})
            cov = c.record_coverage(runs)
        self.assertEqual((cov["executed_jobs"], cov["without_record"]), (3, 1))  # run 5 predates recording
        self.assertEqual([m["run_id"] for m in cov["runs_without_record"]], ["2"])
        # runner lost before the record step, whether or not a test step ran first
        self.assertEqual([m["run_id"] for m in cov["interrupted"]], ["6", "7"])


class ScriptInputsTests(unittest.TestCase):
    def test_a_checkout_missing_locally_is_read_through_the_api(self):
        doc = {"tests": {"t": {"inputs": ["tools/x/"]}}}
        calls = []

        def request(url, accept):
            calls.append(url)
            if "ref=" + "b" * 40 in url:
                raise urllib.error.HTTPError(url, 404, "nf", {}, None)
            resp = mock.MagicMock()
            resp.__enter__.return_value.read.return_value = json.dumps(doc).encode()
            return resp
        c = rrc.Collector.__new__(rrc.Collector)
        c._git = lambda *a, **k: subprocess.CompletedProcess(a, 128, "", "fatal: bad object")
        gh = mock.Mock()
        gh.repository, gh._request = "o/r", request
        with tempfile.TemporaryDirectory() as tmp:
            c.cache, c.gh = Path(tmp), gh
            self.assertEqual(c.input_list_at("a" * 40), {"tools/x"})
            self.assertEqual(c.script_inputs_at("a" * 40), doc)  # cached, no second request
            self.assertIsNone(c.script_inputs_at("b" * 40))       # absent at that commit
            self.assertEqual(len(calls), 2)
            self.assertTrue(all("test/ctest_script_inputs.json" in u for u in calls))
            c.gh = None
            self.assertIsNone(c.script_inputs_at("c" * 40))       # offline: unread, not guessed

    def test_a_failed_fetch_is_unread_and_retried_next_time(self):
        attempts = []

        def request(url, accept):
            attempts.append(url)
            raise urllib.error.HTTPError(url, 502, "bad gateway", {}, None)
        c = rrc.Collector.__new__(rrc.Collector)
        c._git = lambda *a, **k: subprocess.CompletedProcess(a, 128, "", "fatal: bad object")
        gh = mock.Mock()
        gh.repository, gh._request = "o/r", request
        with tempfile.TemporaryDirectory() as tmp:
            c.cache, c.gh = Path(tmp), gh
            self.assertIsNone(c.script_inputs_at("d" * 40))
            self.assertIsNone(c.input_list_at("d" * 40))
            self.assertEqual(len(attempts), 2)                    # the failure was not cached
            self.assertFalse((Path(tmp) / "script-inputs" / f"{'d' * 40}.json.gz").exists())


class GraftTests(unittest.TestCase):
    def test_parents_are_read_through_shallow_grafts(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp) / "repo"
            git = ["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false"]
            subprocess.run(["git", "init", "-q", str(repo)], check=True)
            for n in ("a", "b"):
                (repo / n).write_text(n)
                subprocess.run(git + ["add", n], check=True)
                subprocess.run(git + ["commit", "-q", "-m", n], check=True)
            head_sha, parent_sha = subprocess.run(git + ["rev-parse", "HEAD", "HEAD~1"], check=True,
                                                  capture_output=True, text=True).stdout.split()
            (repo / ".git" / "shallow").write_text(head_sha + "\n")
            # The control is plain git, so an ambient GIT_SHALLOW_FILE (the
            # collector's own override, often exported while replaying) must
            # not reach it.
            plain = {k: v for k, v in os.environ.items() if k != "GIT_SHALLOW_FILE"}
            grafted = subprocess.run(git + ["log", "-1", "--format=%P", head_sha], capture_output=True, text=True,
                                     env=plain)
            self.assertEqual(grafted.stdout.strip(), "", "control: the graft hides the parent from plain git")
            c = rrc.Collector.__new__(rrc.Collector)
            c.repo, c.cache = repo, Path(tmp) / "cache"
            self.assertEqual(c._read_commits([head_sha])[head_sha]["parents"], [parent_sha])


if __name__ == "__main__":
    unittest.main()
