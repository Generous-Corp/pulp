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
