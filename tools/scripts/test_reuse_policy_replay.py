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
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import reuse_policy_replay as rpr  # noqa: E402
import reuse_replay_collect as rrc  # noqa: E402

SCENARIOS = HERE / "fixtures" / "reuse_policy_replay"
TOOL = HERE / "reuse_policy_replay.py"

DESIGN_SCENARIOS = {
    "env-read selftest", "generated-list drift", "host-shaped gate", "masked verdict",
    "flake on one VM", "base-red poison", "iOS PostBuild crash", "unpinned tool", "stale build dir",
}


def group(**kw) -> dict:
    run = {"run_id": "g1", "run_kind": "merge_group", "pr": 1, "head_sha": "h1", "group_sha": "m1",
           "checkout_sha": "m1", "checkout_parents": ["b2", "h1"], "base_sha": "b2", "merge_tree": "t2",
           "runner_image": None, "created_at": "2026-09-29T12:00:00Z",
           "ctest": {"complete": True, "failed": 0}, "build_failed": False, "observed_decision": None}
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
        for name in ("source-key", "per-executable"):
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
            grafted = subprocess.run(git + ["log", "-1", "--format=%P", head_sha], capture_output=True, text=True)
            self.assertEqual(grafted.stdout.strip(), "", "control: the graft hides the parent from plain git")
            c = rrc.Collector.__new__(rrc.Collector)
            c.repo, c.cache = repo, Path(tmp) / "cache"
            self.assertEqual(c._read_commits([head_sha])[head_sha]["parents"], [parent_sha])


if __name__ == "__main__":
    unittest.main()
