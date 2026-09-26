#!/usr/bin/env python3
"""Tests for tools/scripts/build_speed_scorecard.py and bench_diff's section mode.

GitHub inputs are recorded API responses under fixtures/build_speed/, so the
record mapping is checked against real job/step/runner shapes without a
network call. Shipyard is exercised through its argv (record) and through
`metrics list`-shaped rows (report); no test writes to a real metrics store.

Run:  python3 -m unittest test_build_speed_scorecard   (from tools/scripts)
"""
from __future__ import annotations

import datetime as dt
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIX = HERE / "fixtures" / "build_speed"


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


sc = _load("build_speed_scorecard")
bd = _load("bench_diff")
UTC = dt.timezone.utc


class PercentileTests(unittest.TestCase):
    def test_linear_interpolation(self) -> None:
        self.assertEqual(sc.percentile([4, 1, 3, 2], 50), 2.5)
        self.assertAlmostEqual(sc.percentile([1, 2, 3, 4], 90), 3.7)
        self.assertEqual(sc.percentile([7], 90), 7.0)

    def test_no_data_is_none_not_zero(self) -> None:
        self.assertIsNone(sc.percentile([], 50))
        self.assertIsNone(sc.percentile([None], 50))

    def test_since_parsing(self) -> None:
        now = dt.datetime(2026, 9, 23, 12, tzinfo=UTC)
        self.assertEqual(sc.parse_since("7d", now), dt.datetime(2026, 9, 16, 12, tzinfo=UTC))
        self.assertEqual(sc.parse_since("2026-09-01", now), dt.datetime(2026, 9, 1, tzinfo=UTC))
        with self.assertRaises(ValueError):
            sc.parse_since("soon", now)


class RecordMappingTests(unittest.TestCase):
    def test_physical_host(self) -> None:
        self.assertEqual(sc.physical_host("m1-pulp-gate-slot2-02-9255-8"), "m1")
        self.assertEqual(sc.physical_host("studio-pulp-gate-01-58256-1"), "m3")
        self.assertIsNone(sc.physical_host("GitHub Actions 1000119589"))
        self.assertIsNone(sc.physical_host(None))

    def test_gate_job_steps_and_queue(self) -> None:
        f = json.loads((FIX / "merge_group_gate_run.json").read_text())
        recs = sc.gate_job_records(f["run"], f["jobs"])
        by_target = {r["target"]: r for r in recs}
        job = by_target["macos-gate/merge_group"]
        self.assertEqual((job["project"], job["host"], job["status"]), ("pulp", "m1", "success"))
        self.assertEqual(job["pr"], 8756)  # from the gh-readonly-queue branch
        # started 00:14:15, completed 01:19:48
        self.assertEqual(job["duration_ms"], (65 * 60 + 33) * 1000)
        gate_id = next(j["id"] for j in f["jobs"] if j["name"] == "macos")
        self.assertEqual(job["external_id"], f"github:{f['run']['id']}/{gate_id}/1")
        build = by_target["macos-gate/merge_group/Build"]
        self.assertEqual(build["project"], "pulp-gate-steps")
        self.assertEqual(build["duration_ms"], (32 * 60 + 40) * 1000)  # 00:19:34 → 00:52:14
        self.assertIn("macos-gate/merge_group/Test", by_target)
        self.assertIn("macos-gate/merge_group/queue", by_target)
        # the iOS gate is not a separate step, so nothing is invented for it
        self.assertNotIn("macos-gate/merge_group/iOS compile gate", by_target)
        # other jobs in the run are ignored
        self.assertTrue(all(r["job"] == "macos" for r in recs))
        self.assertEqual(len({r["external_id"] for r in recs}), len(recs))

    def test_receipt_reuse_placeholder_is_not_a_gate_run(self) -> None:
        f = json.loads((FIX / "merge_group_receipt_reuse_run.json").read_text())
        recs = sc.gate_job_records(f["run"], f["jobs"])
        self.assertEqual([r["target"] for r in recs], ["macos-gate/merge_group/receipt-reused"])
        self.assertEqual(recs[0]["host"], "github")

    def test_pr_latency_uses_first_and_last_enqueue(self) -> None:
        nodes = json.loads((FIX / "merged_prs_graphql.json").read_text())["data"]["search"]["nodes"]
        recs = sc.pr_latency_records(nodes[0])  # PR 8760
        by = {r["target"]: r["duration_ms"] for r in recs}
        self.assertEqual(by["pr/enqueue-to-merged"], (2 * 60 + 55) * 1000)  # 20:06:47 → 20:09:42
        self.assertEqual(by["pr/open-to-merged"], (38 * 60 + 30) * 1000)
        reenqueued = {"number": 1, "createdAt": "2026-09-23T00:00:00Z",
                      "mergedAt": "2026-09-23T03:00:00Z",
                      "timelineItems": {"nodes": [
                          {"__typename": "AddedToMergeQueueEvent", "createdAt": "2026-09-23T02:30:00Z"},
                          {"__typename": "AddedToMergeQueueEvent", "createdAt": "2026-09-23T01:00:00Z"}]}}
        by = {r["target"]: r["duration_ms"] for r in sc.pr_latency_records(reenqueued)}
        self.assertEqual(by["pr/enqueue-to-merged"], 2 * 3600 * 1000)
        self.assertEqual(by["pr/last-enqueue-to-merged"], 30 * 60 * 1000)

    def test_record_argv(self) -> None:
        argv = sc.record_argv({"project": "pulp", "job": "macos", "target": "macos-gate/pull_request",
                               "host": "m5", "duration_ms": 1000, "pr": None,
                               "external_id": "github:1/2/1"}, "shipyard")
        self.assertEqual(argv[:3], ["shipyard", "metrics", "record"])
        self.assertIn("--external-id", argv)
        self.assertNotIn("--pr", argv)  # unknown is omitted, not sent as 0


def _row(target: str, minutes: float, status: str, when: str, host: str = "m3") -> dict:
    return {"target": target, "total_ms": int(minutes * 60000), "status": status,
            "completed_at": when, "host": host}


class PipelineStatsTests(unittest.TestCase):
    SINCE = dt.datetime(2026, 9, 20, tzinfo=UTC)

    def test_window_hosts_and_failure_rate(self) -> None:
        jobs = [_row("macos-gate/pull_request", 30, "success", "2026-09-21T00:00:00+00:00", "m3"),
                _row("macos-gate/pull_request", 50, "success", "2026-09-21T01:00:00+00:00", "m1"),
                _row("macos-gate/pull_request", 99, "success", "2026-09-01T00:00:00+00:00", "m1"),
                _row("macos-gate/merge_group", 40, "failure", "2026-09-22T00:00:00+00:00"),
                _row("macos-gate/merge_group/receipt-reused", 0.1, "success",
                     "2026-09-22T00:00:00+00:00", "github"),
                _row("macos", 5, "success", "2026-09-22T00:00:00+00:00")]
        steps = [_row("macos-gate/pull_request/Build", 20, "success", "2026-09-21T00:00:00+00:00")]
        queue = [_row("merge-group-run", 40, "failure", "2026-09-22T00:00:00+00:00"),
                 _row("merge-group-run", 40, "success", "2026-09-22T05:00:00+00:00"),
                 _row("merge-group-run", 40, "cancelled", "2026-09-22T06:00:00+00:00"),
                 _row("merge-group-run", 40, "success", "2026-09-23T00:00:00+00:00"),
                 _row("pr/open-to-merged", 120, "success", "2026-09-23T00:00:00+00:00")]
        s = sc.pipeline_stats(jobs, steps, queue, self.SINCE)
        self.assertEqual(s["gate"]["pull_request"], {"n": 2, "p50": 40.0, "p90": 48.0})
        self.assertEqual(s["gate"]["merge_group"]["n"], 0)  # failures are not timing samples
        self.assertEqual(s["gate_failures"]["merge_group"], 1)
        self.assertEqual(set(s["gate_by_host"]), {"m1", "m3"})
        self.assertEqual(s["receipt_reused"], 1)
        mg = s["merge_group_failure"]
        self.assertEqual(mg["per_day"]["2026-09-22"], {"failed": 1, "runs": 2, "rate": 0.5})
        self.assertAlmostEqual(mg["window_rate"], 1 / 3)
        self.assertEqual(s["latency"]["pr/open-to-merged"]["p50"], 2.0)
        self.assertEqual(s["steps"]["macos-gate/pull_request/Build"]["p50"], 20.0)

    def test_sections_mark_unmeasured_as_none(self) -> None:
        s = sc.pipeline_stats([], [], [], self.SINCE)
        secs = {x["title"]: x for x in sc.to_sections(s, None, None)}
        self.assertIsNone(secs["Required macos gate job (min)"]["values"]["pull_request p50"])
        self.assertIsNone(secs["merge_group failure rate (%)"]["values"]["window"])


class SplitTests(unittest.TestCase):
    SINCE = dt.datetime(2026, 9, 20, tzinfo=UTC)
    SPLIT = dt.datetime(2026, 9, 24, 13, 12, tzinfo=UTC)

    def test_rows_fall_on_the_side_of_the_split_they_completed_on(self) -> None:
        jobs = [_row("macos-gate/pull_request", 40, "success", "2026-09-24T13:11:59+00:00"),
                _row("macos-gate/pull_request", 50, "success", "2026-09-22T00:00:00+00:00"),
                _row("macos-gate/pull_request", 20, "success", "2026-09-24T13:12:00+00:00"),
                _row("macos-gate/pull_request", 99, "failure", "2026-09-25T00:00:00+00:00"),
                _row("macos-gate/pull_request", 77, "success", "2026-09-19T00:00:00+00:00")]
        steps = [_row("macos-gate/merge_group/Test", 30, "success", "2026-09-23T00:00:00+00:00"),
                 _row("macos-gate/merge_group/Test", 15, "success", "2026-09-25T00:00:00+00:00")]
        sp = sc.split_stats(jobs, steps, self.SINCE, self.SPLIT)
        by = {(r["event"], r["stage"]): r for r in sp["rows"]}
        gate = by[("pull_request", "gate job")]
        # before the window start (77) and failures (99) are not samples
        self.assertEqual(gate["before"], {"n": 2, "p50": 45.0, "p90": 49.0})
        self.assertEqual(gate["after"], {"n": 1, "p50": 20.0, "p90": 20.0})
        self.assertAlmostEqual(gate["p50_change_pct"], (20 - 45) / 45 * 100)
        test = by[("merge_group", "Test")]
        self.assertEqual((test["before"]["p50"], test["after"]["p50"]), (30.0, 15.0))
        empty = by[("merge_group", "gate job")]
        self.assertIsNone(empty["p50_change_pct"])  # no samples is unknown, not 0%
        md = "\n".join(sc.render_split_markdown(sp))
        self.assertIn("| pull_request | gate job | 2 | 45.0 min | 49.0 min | 1 | 20.0 min | 20.0 min | -56% |", md)
        self.assertNotIn("| merge_group | gate job |", md)  # empty on both sides

    def test_split_before_the_window_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            sc.split_stats([], [], self.SPLIT, self.SINCE)

    def test_split_argument_accepts_minute_precision_z(self) -> None:
        self.assertEqual(sc._parse_split("2026-09-24T13:12Z"), self.SPLIT)


def _q(target: str, minutes: float, when: str, status: str = "success", host: str = "github",
       ext: str = "") -> dict:
    return {"target": target, "total_ms": int(minutes * 60000), "status": status,
            "completed_at": when, "host": host, "external_id": ext}


class MergeLatencyAndCostTests(unittest.TestCase):
    SINCE = dt.datetime(2026, 9, 20, tzinfo=UTC)
    SPLIT = dt.datetime(2026, 9, 24, tzinfo=UTC)
    NOW = dt.datetime(2026, 9, 26, tzinfo=UTC)  # after side = 2 days, before = 4

    def test_queue_cancelled_merge_group_job_is_not_a_reused_receipt(self) -> None:
        f = json.loads((FIX / "merge_group_receipt_reuse_run.json").read_text())
        job = dict(next(j for j in f["jobs"] if j["name"] == "macos"))
        job.update(runner_name="", conclusion="cancelled", created_at="2026-09-23T10:13:09Z",
                   completed_at="2026-09-23T10:33:09Z")
        recs = sc.gate_job_records(f["run"], [job])
        # recorded as starvation (the 20 minutes it waited), never as a placeholder
        self.assertEqual([(r["target"], r["duration_ms"], r["host"]) for r in recs],
                         [("macos-gate/merge_group/no-runner-cancel", 20 * 60000, "none")])
        self.assertEqual(sc.gate_ms_by_run(recs), {})

    def test_release_jobs_on_gate_runners_are_recorded_with_their_queue(self) -> None:
        run = {"id": 5, "name": "Release CLI", "head_branch": "main", "head_sha": "abc"}
        jobs = [{"id": 1, "name": "CLI darwin-arm64", "status": "completed", "conclusion": "success",
                 "runner_name": "m5-pulp-gate-slot2-02-34267-1", "created_at": "2026-09-25T12:00:00Z",
                 "started_at": "2026-09-25T12:30:00Z", "completed_at": "2026-09-25T12:40:00Z"},
                {"id": 2, "name": "CLI linux-x64", "status": "completed", "conclusion": "success",
                 "runner_name": "GitHub Actions 1000130753", "created_at": "2026-09-25T12:00:00Z",
                 "started_at": "2026-09-25T12:00:10Z", "completed_at": "2026-09-25T12:10:00Z"}]
        recs = sc.release_job_records(run, jobs, "release-cli.yml")
        self.assertEqual({(r["target"], r["duration_ms"], r["host"]) for r in recs},
                         {("release/release-cli", 10 * 60000, "m5"),
                          ("release/release-cli/queue", 30 * 60000, "m5")})

    def test_ejections_carry_reason_and_the_queue_time_they_lost(self) -> None:
        pr = {"number": 7, "createdAt": "2026-09-23T00:00:00Z", "mergedAt": "2026-09-23T05:00:00Z",
              "timelineItems": {"nodes": [
                  {"__typename": "AddedToMergeQueueEvent", "createdAt": "2026-09-23T01:00:00Z"},
                  {"__typename": "RemovedFromMergeQueueEvent", "createdAt": "2026-09-23T01:40:00Z",
                   "reason": "FAILED_CHECKS"},
                  {"__typename": "AddedToMergeQueueEvent", "createdAt": "2026-09-23T02:00:00Z"},
                  {"__typename": "RemovedFromMergeQueueEvent", "createdAt": "2026-09-23T02:05:00Z",
                   "reason": "MERGE_CONFLICT"},
                  {"__typename": "AddedToMergeQueueEvent", "createdAt": "2026-09-23T04:00:00Z"},
                  {"__typename": "RemovedFromMergeQueueEvent", "createdAt": "2026-09-23T05:00:00Z",
                   "reason": "MERGED"}]}}
        ej = [r for r in sc.pr_latency_records(pr) if r["target"].startswith("pr/ejected/")]
        self.assertEqual([(r["target"], r["duration_ms"]) for r in ej],
                         [("pr/ejected/failed_checks", 40 * 60000),
                          ("pr/ejected/merge_conflict", 5 * 60000)])
        self.assertEqual(len({r["external_id"] for r in ej}), 2)
        by = {r["target"]: r["duration_ms"] for r in sc.pr_latency_records(pr)}
        self.assertEqual(by["pr/last-enqueue-to-merged"], 60 * 60000)

    def test_gate_minutes_are_attributed_to_the_pr_across_heads_and_merge_groups(self) -> None:
        runs = [{"id": 1, "pull_requests": [{"number": 7}], "head_branch": "feature/x"},
                {"id": 2, "pull_requests": [], "head_branch": "gh-readonly-queue/main/pr-7-abc"},
                {"id": 3, "pull_requests": [], "head_branch": "gh-readonly-queue/main/pr-9-def"},
                {"id": 4, "pull_requests": [], "head_branch": "main"}]
        pr_of_run = {str(r["id"]): p for r in runs if (p := sc.run_pr(r)) is not None}
        self.assertEqual(pr_of_run, {"1": 7, "2": 7, "3": 9})
        rows = [_q("macos-gate/pull_request", 20, "", "success", "m3", "github:1/10/1"),
                _q("macos-gate/pull_request", 5, "", "cancelled", "m1", "github:1/11/2"),
                _q("macos-gate/merge_group", 30, "", "failure", "m5", "github:2/12/1"),
                _q("macos-gate/merge_group/receipt-reused", 1, "", "success", "github",
                   "github:2/13/1"),
                _q("macos-gate/pull_request/Build", 9, "", "success", "m3", "github:1/10/1/step/Build"),
                _q("macos-gate/merge_group", 25, "", "success", "m3", "github:3/14/1")]
        # a fresh record (duration_ms) for a row also listed is counted once
        fresh = {"target": "macos-gate/pull_request", "duration_ms": 20 * 60000, "host": "m3",
                 "external_id": "github:1/10/1"}
        ms = sc.gate_ms_by_run(rows + [fresh])
        self.assertEqual(ms, {"1": 25 * 60000, "2": 30 * 60000, "3": 25 * 60000})
        prs = [{"number": 7, "createdAt": "2026-09-21T00:00:00Z", "mergedAt": "2026-09-22T00:00:00Z"},
               {"number": 8, "createdAt": "2026-09-21T00:00:00Z", "mergedAt": "2026-09-22T00:00:00Z"},
               {"number": 9, "createdAt": "2026-09-01T00:00:00Z", "mergedAt": "2026-09-22T00:00:00Z"},
               {"number": 10, "createdAt": "2026-09-25T00:00:00Z", "mergedAt": "2026-09-25T23:30:00Z"}]
        recs = sc.pr_gate_minutes_records(prs, pr_of_run, ms, self.SINCE, self.NOW)
        # 9 opened before the ingested window, 10 merged too recently to be final
        self.assertEqual({r["pr"]: r["duration_ms"] for r in recs}, {7: 55 * 60000, 8: 0})
        self.assertTrue(all(r["target"] == "pr/gate-minutes" for r in recs))

    def test_placeholder_kind_comes_from_the_annotation(self) -> None:
        self.assertEqual(sc.placeholder_kind([
            {"message": "The ubuntu-latest label will migrate"},
            {"message": "an exact-tree protected receipt from an earlier run was reused"}]),
            "receipt-reused")
        self.assertEqual(sc.placeholder_kind([{"message": "the merge group changed no native "
                                                          "build input, so the matrix was skipped"}]),
                         "skip-safe")
        self.assertEqual(sc.placeholder_kind([]), "unknown")
        rec = sc.placeholder_kind_record({"external_id": "github:1/2/1",
                                          "completed_at": "2026-09-25T00:00:00+00:00"}, "skip-safe")
        self.assertEqual((rec["target"], rec["external_id"]),
                         ("macos-gate/merge_group/placeholder/skip-safe", "github:1/2/1/kind"))
        # a classification row is not a gate run and carries no gate minutes
        self.assertEqual(sc.gate_ms_by_run([{**rec, "total_ms": 0}]), {})

    def _fixture(self) -> tuple[list[dict], list[dict], list[dict]]:
        jobs = [_q("macos-gate/pull_request", 20, "2026-09-22T00:00:00+00:00", "cancelled", "m3"),
                _q("macos-gate/pull_request", 40, "2026-09-25T00:00:00+00:00", "cancelled", "m1"),
                _q("macos-gate/merge_group", 30, "2026-09-22T00:00:00+00:00", "success", "m3"),
                _q("macos-gate/merge_group", 30, "2026-09-22T01:00:00+00:00", "failure", "m3"),
                _q("macos-gate/merge_group", 30, "2026-09-25T00:00:00+00:00", "success", "m5"),
                _q("macos-gate/merge_group/receipt-reused", 1, "2026-09-25T01:00:00+00:00"),
                _q("macos-gate/merge_group/placeholder/receipt-reused", 0,
                   "2026-09-25T01:00:00+00:00"),
                _q("macos-gate/merge_group/receipt-reused", 1, "2026-09-25T02:00:00+00:00",
                   "cancelled"),
                # a skip-safe group is a placeholder but not a reuse; one legacy
                # placeholder has no classification yet
                _q("macos-gate/merge_group/receipt-reused", 1, "2026-09-22T03:00:00+00:00"),
                _q("macos-gate/merge_group/placeholder/skip-safe", 0, "2026-09-22T03:00:00+00:00"),
                _q("macos-gate/merge_group/receipt-reused", 1, "2026-09-22T04:00:00+00:00"),
                _q("macos-gate/pull_request/no-runner-cancel", 90, "2026-09-25T03:00:00+00:00",
                   "cancelled", "none"),
                _q("macos-gate/pull_request/no-runner-cancel", 30, "2026-09-25T04:00:00+00:00",
                   "cancelled", "none"),
                _q("release/release-cli", 12, "2026-09-25T05:00:00+00:00", "success", "m5")]
        steps = [_q("macos-gate/pull_request/queue", 10, "2026-09-22T00:00:00+00:00", host="m3"),
                 _q("macos-gate/pull_request/queue", 30, "2026-09-22T00:00:00+00:00", host="m3"),
                 _q("macos-gate/pull_request/queue", 4, "2026-09-25T00:00:00+00:00", host="m5"),
                 _q("release/release-cli/queue", 40, "2026-09-25T04:48:00+00:00", host="m5")]
        queue = [
            # before: three PRs, two merged by one push
            _q("pr/open-to-merged", 600, "2026-09-22T10:00:00+00:00"),
            _q("pr/open-to-merged", 120, "2026-09-22T10:00:00+00:00"),
            _q("pr/open-to-merged", 300, "2026-09-23T10:00:00+00:00"),
            _q("pr/last-enqueue-to-merged", 60, "2026-09-22T10:00:00+00:00"),
            _q("pr/last-enqueue-to-merged", 40, "2026-09-22T10:00:00+00:00"),
            _q("pr/last-enqueue-to-merged", 50, "2026-09-23T10:00:00+00:00"),
            _q("pr/gate-minutes", 80, "2026-09-22T10:00:00+00:00"),
            _q("pr/gate-minutes", 40, "2026-09-23T10:00:00+00:00"),
            _q("pr/ejected/failed_checks", 30, "2026-09-22T05:00:00+00:00", "failure"),
            _q("pr/ejected/merge_conflict", 3, "2026-09-22T06:00:00+00:00", "failure"),
            _q("merge-group-run", 40, "2026-09-22T00:00:00+00:00", "failure"),
            _q("merge-group-run", 40, "2026-09-22T02:00:00+00:00", "success"),
            _q("merge-group-run", 40, "2026-09-22T03:00:00+00:00", "cancelled"),
            # after: one PR
            _q("pr/open-to-merged", 180, "2026-09-25T10:00:00+00:00"),
            _q("pr/last-enqueue-to-merged", 30, "2026-09-25T10:00:00+00:00"),
            _q("pr/gate-minutes", 45, "2026-09-25T10:00:00+00:00"),
            _q("merge-group-run", 40, "2026-09-25T00:00:00+00:00", "success"),
        ]
        return jobs, steps, queue

    def test_both_sides_are_normalised_per_pr_and_per_day(self) -> None:
        jobs, steps, queue = self._fixture()
        ms = sc.merge_split_stats(jobs, steps, queue, self.SINCE, self.SPLIT, now=self.NOW)
        b, a = ms["sides"]["before"], ms["sides"]["after"]
        self.assertEqual((b["merged"], a["merged"]), (3, 1))
        self.assertAlmostEqual(b["merged_per_day"], 3 / 4)
        self.assertEqual(b["latency"]["pr/last-enqueue-to-merged"]["p50"], 50.0)
        self.assertEqual(b["latency"]["pr/open-to-merged"]["p50"], 5.0)  # hours
        self.assertAlmostEqual(b["batch"]["mean"], (2 + 2 + 1) / 3)
        self.assertEqual(b["gate_minutes_per_pr"], {"n": 2, "p50": 60.0, "mean": 60.0})
        self.assertEqual(b["cancelled_gate_minutes"]["total"], 20.0)
        self.assertAlmostEqual(b["cancelled_gate_minutes"]["per_day"], 5.0)
        self.assertAlmostEqual(a["cancelled_gate_minutes"]["per_merged_pr"], 40.0)
        # a cancelled or skip-safe placeholder is not a reuse; 0 of 2 before, 1 of 2 after
        self.assertEqual(b["receipt_reuse"], {"reused": 0, "merge_groups": 2, "rate": 0.0,
                                              "rate_upper": 0.0, "skip_safe": 1,
                                              "unclassified": 1, "unknown": 0})
        self.assertNotIn("upper bound", "\n".join(sc.render_merge_split_markdown(ms)))
        # an unannotated placeholder (older gate) bounds the rate instead of hiding
        jobs.append(_q("macos-gate/merge_group/placeholder/unknown", 0, "2026-09-22T05:00:00+00:00"))
        ub = sc.merge_split_stats(jobs, steps, queue, self.SINCE, self.SPLIT,
                                  now=self.NOW)["sides"]["before"]["receipt_reuse"]
        self.assertEqual((ub["rate"], ub["unknown"]), (0.0, 1))
        self.assertAlmostEqual(ub["rate_upper"], 1 / 3)
        self.assertEqual(a["receipt_reuse"]["rate"], 0.5)
        self.assertEqual(b["merge_group_runs"], {"completed": 3, "failed": 1, "cancelled": 1,
                                                 "failure_rate": 0.5})
        self.assertEqual(b["ejections"]["by_reason"], {"failed_checks": 1, "merge_conflict": 1})
        self.assertAlmostEqual(b["ejections"]["per_merged_pr"], 2 / 3)
        self.assertEqual(a["ejections"]["total"], 0)
        self.assertEqual(b["queue_by_host"]["pull_request"]["m3"]["p50"], 20.0)
        self.assertEqual(list(a["queue_by_host"]["pull_request"]), ["m5"])
        # no-runner cancels count as starvation, never as cancelled gate-minutes
        self.assertEqual(a["no_runner_cancels"]["pull_request"]["n"], 2)
        self.assertEqual(a["no_runner_cancels"]["pull_request"]["p50"], 60.0)
        self.assertAlmostEqual(a["no_runner_cancels"]["pull_request"]["per_day"], 1.0)
        self.assertEqual(b["no_runner_cancels"]["pull_request"]["n"], 0)
        self.assertEqual(a["cancelled_gate_minutes"]["total"], 40.0)
        self.assertEqual(a["release_on_gate"], {"jobs": 1, "minutes": 12.0,
                                                "queue": {"n": 1, "p50": 40.0, "p90": 40.0}})
        md = "\n".join(sc.render_merge_split_markdown(ms))
        self.assertIn("| last enqueue→merged p50 | 3 | 50.0 min | 1 | 30.0 min | -40% |", md)
        self.assertIn("| receipt reuse rate | 2 | 0.0 % | 2 | 50.0 % | +50 pp |", md)
        self.assertIn("failed_checks 1 → 0", md)

    def test_after_from_and_until_bound_the_after_side(self) -> None:
        jobs, steps, queue = self._fixture()
        ms = sc.merge_split_stats(jobs, steps, queue, self.SINCE, self.SPLIT,
                                  after_from=dt.datetime(2026, 9, 25, 5, tzinfo=UTC),
                                  until=dt.datetime(2026, 9, 25, 11, tzinfo=UTC), now=self.NOW)
        a = ms["sides"]["after"]
        self.assertEqual(a["window"], ["2026-09-25T05:00:00Z", "2026-09-25T11:00:00Z"])
        self.assertEqual(a["merged"], 1)
        self.assertEqual(a["merge_group_runs"]["completed"], 0)  # 00:00 falls in the gap
        self.assertAlmostEqual(a["merged_per_day"], 4.0)
        with self.assertRaises(ValueError):
            sc.split_halves(self.SINCE, self.SPLIT, dt.datetime(2026, 9, 23, tzinfo=UTC), None)
        with self.assertRaises(ValueError):
            sc.split_halves(self.SINCE, self.SPLIT, None, self.SPLIT)


class TartciCellTests(unittest.TestCase):
    def test_installed_generation_names_a_sealed_host(self) -> None:
        self.assertEqual(sc.tartci_cell({"installed_generation": "3dd84d9b099f",
                                         "executing_generation": None, "checkout_head": None}),
                         "3dd84d9b099f / n/a / n/a")

    def test_sensor_older_than_the_field_says_so(self) -> None:
        self.assertEqual(sc.tartci_cell({"executing_generation": "abc", "checkout_head": "def"}),
                         "not published / abc / def")


class FleetTests(unittest.TestCase):
    def test_unreachable_host_is_labelled(self) -> None:
        st = sc.read_host_state("nowhere", "nowhere.invalid", HERE / "host_vitals.sh")
        self.assertEqual(st["status"], "UNREACHABLE")
        self.assertNotIn("build", st)
        md = sc.render_markdown({"generated_at": "t", "local": {"status": "NOT MEASURED", "reason": "x"},
                                 "pipeline": {"status": "NOT MEASURED", "reason": "y"}, "fleet": [st]})
        self.assertIn("**UNREACHABLE**", md)

    def test_local_section_without_build_dir(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(sc.local_build(Path(d), "all", False, [])["status"], "NOT MEASURED")
        self.assertEqual(sc.local_build(None, "all", False, [])["status"], "NOT MEASURED")


class LocalSectionTests(unittest.TestCase):
    LOG = {"selected": {"edge_seconds": 10.0, "categories": {
        "compile": {"edge_seconds": 10.0, "count": 5}}}}

    def _blast(self, pending: int) -> dict:
        return {"control": {"total": pending},
                "files": [{"file": "a.cpp", "status": "ok", "compiles": 1, "exe_links": 2,
                           "bundle_links": 0}]}

    def _titles(self, local: dict) -> set[str]:
        return {s["title"] for s in sc.to_sections(None, None, local)}

    def test_only_comparable_local_numbers_reach_the_diff(self) -> None:
        up_to_date = {"status": "ok", "ninja_log": self.LOG, "blast_radius": self._blast(3),
                      "log_is_clean_build": True}
        self.assertEqual(self._titles(up_to_date), {"Local build edge-seconds (s)",
                                                    "Local build edges (count)",
                                                    "Blast radius (edges)"})
        # a stale tree gives lower bounds, and an unlabelled log may be incremental
        stale = {"status": "ok", "ninja_log": self.LOG, "blast_radius": self._blast(3715),
                 "log_is_clean_build": False}
        self.assertEqual(self._titles(stale), set())


class BenchSectionsTests(unittest.TestCase):
    BASE = {"title": "Build speed", "date": "2026-09-23", "host": "m3", "pulp_commit": "272d383fb2",
            "sections": [{"title": "Gate (min)", "unit": "min", "lower_is_better": True,
                          "values": {"p50": 40.0, "p90": 60.0, "iOS": None}},
                         {"title": "ccache", "unit": "%", "lower_is_better": False,
                          "values": {"hit": 66.0}}]}
    CUR = {"title": "Build speed", "date": "2026-09-30", "host": "m3", "pulp_commit": "abc",
           "sections": [{"title": "Gate (min)", "unit": "min", "lower_is_better": True,
                         "values": {"p50": 25.0, "p90": 66.0, "iOS": 3.0}},
                        {"title": "ccache", "unit": "%", "lower_is_better": False,
                         "values": {"hit": 60.0}}]}

    def test_delta_and_regressions(self) -> None:
        out = bd.render_sections(self.BASE, self.CUR, 0.05)
        self.assertIn("| p50 | 40 min | 25 min | -37.5% (better) |", out)
        self.assertIn("| p90 | 60 min | 66 min | +10.0% (worse) |", out)
        self.assertIn("| iOS | — | 3 min | unknown |", out)  # unknown baseline, not 0
        self.assertIn("| hit | 66 % | 60 % | -9.1% (worse) |", out)
        self.assertIn("Gate (min) / p90", out)
        self.assertIn("ccache / hit", out)  # a drop in a higher-is-better metric
        self.assertNotIn("Gate (min) / p50", out.split("Regressions")[1])

    def test_per_key_direction_and_neutral_sections(self) -> None:
        base = {"sections": [{"title": "Fleet", "unit": "", "lower_is_better": True,
                              "higher_is_better_keys": ["hit"], "values": {"hit": 60, "load": 10}},
                             {"title": "Reuse", "unit": "count", "lower_is_better": None,
                              "values": {"n": 10}}]}
        cur = {"sections": [{"title": "Fleet", "unit": "", "lower_is_better": True,
                             "higher_is_better_keys": ["hit"], "values": {"hit": 80, "load": 20}},
                            {"title": "Reuse", "unit": "count", "lower_is_better": None,
                             "values": {"n": 50}}]}
        out = bd.render_sections(base, cur, 0.05)
        self.assertIn("| hit | 60 | 80 | +33.3% (better) |", out)
        self.assertIn("| load | 10 | 20 | +100.0% (worse) |", out)
        self.assertIn("| n | 10 | 50 | +400.0% |", out)
        regressions = out.split("Regressions")[1]
        self.assertIn("Fleet / load", regressions)
        self.assertNotIn("Reuse", regressions)
        self.assertNotIn("Fleet / hit", regressions)

    def test_small_values_keep_three_significant_figures(self) -> None:
        base = {"sections": [{"title": "Kernel", "unit": "ns/element", "lower_is_better": True,
                              "values": {"dot": 0.61, "sum": 0.028, "fir": 16.53}}]}
        cur = {"sections": [{"title": "Kernel", "unit": "ns/element", "lower_is_better": True,
                             "values": {"dot": 0.052, "sum": 0.028, "fir": 1.3417}}]}
        out = bd.render_sections(base, cur, 0.05)
        self.assertIn("| dot | 0.61 ns/element | 0.052 ns/element | -91.5% (better) |", out)
        self.assertIn("| sum | 0.028 ns/element | 0.028 ns/element | = |", out)
        self.assertIn("| fir | 16.5 ns/element | 1.34 ns/element | -91.9% (better) |", out)

    def test_cli_dispatches_on_sections(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            b, c = Path(d, "b.json"), Path(d, "c.json")
            b.write_text(json.dumps(self.BASE))
            c.write_text(json.dumps(self.CUR))
            proc = subprocess.run([sys.executable, str(HERE / "bench_diff.py"), str(b), str(c)],
                                  capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("## Gate (min)", proc.stdout)
        self.assertNotIn("Memory-bandwidth", proc.stdout)


if __name__ == "__main__":
    unittest.main()
