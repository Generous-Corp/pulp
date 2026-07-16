#!/usr/bin/env python3
"""Tests for queue_age_watchdog.py.

The load-bearing cases are the QUIET ones. A monitor that cries wolf gets
muted, and a muted monitor is worse than none: it still looks like coverage.
The measured-baseline test below is the contract that keeps this thing quiet
on a real busy afternoon.
"""
from __future__ import annotations

import datetime as dt
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import queue_age_watchdog as qaw  # noqa: E402

NOW = dt.datetime(2026, 7, 16, 12, 0, 0, tzinfo=dt.timezone.utc)
MAC = ["self-hosted", "macOS", "pulp-studio"]


def ago(minutes: float) -> str:
    return (NOW - dt.timedelta(minutes=minutes)).isoformat().replace("+00:00", "Z")


def queued(minutes: float, labels=None, name="build", workflow="Build"):
    return {
        "run_id": 1,
        "run_url": "https://example.invalid/run/1",
        "workflow": workflow,
        "job": name,
        "labels": list(labels if labels is not None else MAC),
        "queued_at": ago(minutes),
    }


def live(minutes: float, labels=None, status="in_progress"):
    return {
        "labels": list(labels if labels is not None else MAC),
        "status": status,
        "started_at": ago(minutes),
    }


def levels(findings):
    return [f["level"] for f in findings]


class TestLaneComparison(unittest.TestCase):
    def test_equal_sets_are_comparable(self):
        self.assertTrue(qaw.lanes_are_comparable({"a", "b"}, {"a", "b"}))

    def test_subset_in_either_direction_is_comparable(self):
        self.assertTrue(qaw.lanes_are_comparable({"a"}, {"a", "b"}))
        self.assertTrue(qaw.lanes_are_comparable({"a", "b"}, {"a"}))

    def test_disjoint_and_overlapping_sets_are_not_comparable(self):
        self.assertFalse(qaw.lanes_are_comparable({"a"}, {"b"}))
        # Overlap alone is not enough: {self-hosted, macOS} and
        # {self-hosted, linux} share a label but are different lanes.
        self.assertFalse(qaw.lanes_are_comparable({"x", "a"}, {"x", "b"}))

    def test_empty_set_is_never_comparable(self):
        self.assertFalse(qaw.lanes_are_comparable(set(), {"a"}))


class TestQuietCases(unittest.TestCase):
    """Every one of these MUST produce zero alarms."""

    def test_measured_healthy_baseline_is_silent(self):
        # Baseline measured on this repo under normal healthy load:
        # median queue age 5 min, oldest 31 min, 3 runs over 30 min.
        # A naive "age > 30" rule alarms 3x here. This must alarm zero times.
        ages = [1, 2, 3, 4, 5, 5, 5, 6, 8, 12, 19, 27, 30.5, 31, 30.2]
        snap = {
            "queued_jobs": [queued(a) for a in ages],
            "live_jobs": [live(4), live(11), live(20)],
        }
        findings = qaw.analyze(snap, NOW)
        self.assertEqual([f for f in findings if f["level"] == "alarm"], [])
        # The three over the warn threshold are still surfaced as warnings.
        self.assertEqual(len(findings), 3)
        self.assertEqual(set(levels(findings)), {"warn"})

    def test_idle_fleet_with_nothing_queued_is_silent(self):
        # The whole point: zero runners registered is the healthy-idle state
        # for a JIT lane. Nothing queued -> nothing to say.
        findings = qaw.analyze({"queued_jobs": [], "live_jobs": []}, NOW)
        self.assertEqual(findings, [])

    def test_deep_queue_stays_quiet_while_lane_is_busy(self):
        # Saturation, not death: age is way past alarm but a runner is visibly
        # chewing on work.
        snap = {"queued_jobs": [queued(120)], "live_jobs": [live(90)]}
        self.assertEqual(levels(qaw.analyze(snap, NOW)), ["warn"])

    def test_single_runner_on_a_long_job_is_not_dead(self):
        # One runner, one 90-min job, a queue behind it: nothing has *started*
        # since the job queued, but the in_progress job proves the lane lives.
        # Age alone would have called this dead.
        snap = {"queued_jobs": [queued(60)], "live_jobs": [live(90)]}
        self.assertEqual(levels(qaw.analyze(snap, NOW)), ["warn"])

    def test_completed_job_that_started_after_queueing_proves_life(self):
        snap = {
            "queued_jobs": [queued(60)],
            "live_jobs": [live(50, status="completed")],
        }
        self.assertEqual(levels(qaw.analyze(snap, NOW)), ["warn"])

    def test_label_drift_between_comparable_lanes_stays_quiet(self):
        # The queued job asked for a subset of what the live job asked for.
        # Same physical lane; must not alarm.
        snap = {
            "queued_jobs": [queued(60, labels=["self-hosted", "macOS"])],
            "live_jobs": [live(5, labels=MAC)],
        }
        self.assertEqual(levels(qaw.analyze(snap, NOW)), ["warn"])

    def test_below_warn_threshold_is_not_reported_at_all(self):
        snap = {"queued_jobs": [queued(29.9)], "live_jobs": []}
        self.assertEqual(qaw.analyze(snap, NOW), [])

    def test_between_warn_and_alarm_never_alarms_even_with_dead_lane(self):
        snap = {"queued_jobs": [queued(44)], "live_jobs": []}
        self.assertEqual(levels(qaw.analyze(snap, NOW)), ["warn"])


class TestAlarmCases(unittest.TestCase):
    def test_dead_lane_with_no_runners_alarms(self):
        snap = {"queued_jobs": [queued(46)], "live_jobs": []}
        findings = qaw.analyze(snap, NOW)
        self.assertEqual(levels(findings), ["alarm"])
        self.assertEqual(findings[0]["lane_evidence"], "no live runner observed")

    def test_alarm_at_exactly_the_threshold(self):
        snap = {"queued_jobs": [queued(45)], "live_jobs": []}
        self.assertEqual(levels(qaw.analyze(snap, NOW)), ["alarm"])

    def test_a_live_sibling_lane_does_not_rescue_a_dead_one(self):
        # Linux is fine; the macOS lane is dead. The whole point of reporting
        # labels is that a human sees WHICH lane is sick.
        snap = {
            "queued_jobs": [queued(60, labels=MAC)],
            "live_jobs": [live(2, labels=["self-hosted", "linux"])],
        }
        findings = qaw.analyze(snap, NOW)
        self.assertEqual(levels(findings), ["alarm"])
        self.assertEqual(findings[0]["labels"], sorted(MAC))

    def test_a_start_predating_the_queued_job_does_not_prove_life(self):
        # The lane served something 70 min ago and has been dead since; our job
        # queued 60 min ago and has seen nothing start. That start says nothing
        # about the lane's health now.
        snap = {
            "queued_jobs": [queued(60)],
            "live_jobs": [live(70, status="completed")],
        }
        self.assertEqual(levels(qaw.analyze(snap, NOW)), ["alarm"])

    def test_findings_are_sorted_alarms_first_then_oldest(self):
        snap = {
            "queued_jobs": [
                queued(32, name="young-warn"),
                queued(50, name="old-alarm", labels=["dead-a"]),
                queued(90, name="oldest-alarm", labels=["dead-b"]),
            ],
            "live_jobs": [live(1, labels=MAC)],
        }
        findings = qaw.analyze(snap, NOW)
        self.assertEqual(
            [f["job"] for f in findings], ["oldest-alarm", "old-alarm", "young-warn"]
        )

    def test_thresholds_are_tunable(self):
        snap = {"queued_jobs": [queued(35)], "live_jobs": []}
        self.assertEqual(levels(qaw.analyze(snap, NOW)), ["warn"])
        self.assertEqual(
            levels(qaw.analyze(snap, NOW, warn_minutes=10, alarm_minutes=30)),
            ["alarm"],
        )


class TestGrouping(unittest.TestCase):
    def test_lanes_group_worst_first_with_counts(self):
        findings = qaw.analyze(
            {
                "queued_jobs": [
                    queued(50, labels=["lane-a"]),
                    queued(70, labels=["lane-a"], workflow="Coverage"),
                    queued(60, labels=["lane-b"]),
                ],
                "live_jobs": [],
            },
            NOW,
        )
        rows = qaw.group_by_lane(findings)
        self.assertEqual([r["lane"] for r in rows], ["lane-a", "lane-b"])
        self.assertEqual(rows[0]["count"], 2)
        self.assertEqual(rows[0]["oldest"], 70)
        self.assertEqual(rows[0]["workflows"], ["Build", "Coverage"])


class TestTimestampParsing(unittest.TestCase):
    def test_z_suffix_and_offset_forms_agree(self):
        self.assertEqual(
            qaw.parse_ts("2026-07-16T12:00:00Z"),
            qaw.parse_ts("2026-07-16T12:00:00+00:00"),
        )

    def test_naive_timestamps_are_treated_as_utc(self):
        self.assertEqual(qaw.parse_ts("2026-07-16T12:00:00"), NOW)

    def test_non_utc_offsets_normalize(self):
        self.assertEqual(qaw.parse_ts("2026-07-16T14:00:00+02:00"), NOW)


class TestRendering(unittest.TestCase):
    def test_body_names_the_sick_lane_and_its_labels(self):
        findings = qaw.analyze(
            {"queued_jobs": [queued(61, labels=MAC)], "live_jobs": []}, NOW
        )
        body = qaw.render_body(findings, qaw.ALARM_MINUTES, NOW)
        self.assertIn("pulp-studio", body)
        self.assertIn("runner-health-check.yml", body)
        self.assertIn("61 min", body)

    def test_body_omits_warn_only_findings(self):
        findings = qaw.analyze(
            {
                "queued_jobs": [queued(35, labels=["quiet-lane"])],
                "live_jobs": [live(1, labels=["quiet-lane"])],
            },
            NOW,
        )
        body = qaw.render_body(findings, qaw.ALARM_MINUTES, NOW)
        self.assertIn("0 job(s)", body)
        self.assertNotIn("quiet-lane", body)

    def test_summary_reports_healthy_when_empty(self):
        summary = qaw.render_summary([], qaw.ALARM_MINUTES)
        self.assertIn("healthy", summary)


class TestCli(unittest.TestCase):
    """Shell out to the real script — exit codes and files are the contract."""

    def _run(self, snapshot, extra=None):
        tmp = Path(tempfile.mkdtemp())
        snap_path = tmp / "snapshot.json"
        snap_path.write_text(json.dumps(snapshot), encoding="utf-8")
        proc = subprocess.run(
            [
                sys.executable,
                str(Path(__file__).resolve().parent / "queue_age_watchdog.py"),
                "--snapshot",
                str(snap_path),
                "--findings-out",
                str(tmp / "findings.json"),
                "--body-out",
                str(tmp / "body.md"),
            ]
            + (extra or []),
            capture_output=True,
            text=True,
        )
        return proc, tmp

    def test_healthy_snapshot_exits_zero_and_writes_no_body(self):
        snap = {
            "generated_at": NOW.isoformat(),
            "queued_jobs": [queued(31)],
            "live_jobs": [live(2)],
        }
        proc, tmp = self._run(snap)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("alarm_count=0", proc.stdout)
        self.assertFalse((tmp / "body.md").exists())

    def test_dead_lane_snapshot_writes_body_and_findings(self):
        snap = {
            "generated_at": NOW.isoformat(),
            "queued_jobs": [queued(75)],
            "live_jobs": [],
        }
        proc, tmp = self._run(snap)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("alarm_count=1", proc.stdout)
        self.assertIn("pulp-studio", (tmp / "body.md").read_text(encoding="utf-8"))
        findings = json.loads((tmp / "findings.json").read_text(encoding="utf-8"))
        self.assertEqual(findings[0]["level"], "alarm")

    def test_generated_at_pins_now_so_snapshots_are_replayable(self):
        # Replaying a recorded incident must reproduce its verdict rather than
        # ageing every job by the wall clock since it was captured.
        snap = {
            "generated_at": NOW.isoformat(),
            "queued_jobs": [queued(10)],
            "live_jobs": [live(2)],
        }
        proc, _ = self._run(snap)
        self.assertIn("alarm_count=0", proc.stdout)
        self.assertIn("Fleet looks healthy", proc.stdout)

    def test_inverted_thresholds_are_rejected(self):
        snap = {"generated_at": NOW.isoformat(), "queued_jobs": [], "live_jobs": []}
        proc, _ = self._run(snap, extra=["--warn-minutes", "60", "--alarm-minutes", "10"])
        self.assertEqual(proc.returncode, 2)
        self.assertIn("must be >=", proc.stderr)

    def test_live_mode_requires_a_repo(self):
        proc = subprocess.run(
            [
                sys.executable,
                str(Path(__file__).resolve().parent / "queue_age_watchdog.py"),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(proc.returncode, 2)
        self.assertIn("--repo required", proc.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
