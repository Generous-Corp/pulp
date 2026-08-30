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
from unittest import mock

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


def unexpanded(
    minutes: float,
    status="pending",
    workflow="Build and Test",
    event="pull_request",
    workflow_path=".github/workflows/build.yml",
):
    return {
        "run_id": 99,
        "run_url": "https://example.invalid/run/99",
        "workflow": workflow,
        "workflow_path": workflow_path,
        "event": event,
        "status": status,
        "head_sha": "a" * 40,
        "head_branch": "feature/example",
        "queued_at": ago(minutes),
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

    def test_aged_build_and_test_run_with_zero_jobs_alarms(self):
        snap = {
            "unexpanded_runs": [unexpanded(46)],
            "queued_jobs": [],
            "live_jobs": [live(1)],
        }
        findings = qaw.analyze(snap, NOW)
        self.assertEqual(levels(findings), ["alarm"])
        self.assertEqual(findings[0]["kind"], "unexpanded_workflow_run")
        self.assertEqual(findings[0]["lane_evidence"], "workflow run has zero jobs")

    def test_unexpanded_run_at_exactly_the_threshold_alarms(self):
        snap = {"unexpanded_runs": [unexpanded(45)]}
        self.assertEqual(levels(qaw.analyze(snap, NOW)), ["alarm"])

    def test_unexpanded_run_below_warn_threshold_is_silent(self):
        snap = {"unexpanded_runs": [unexpanded(29.9)]}
        self.assertEqual(qaw.analyze(snap, NOW), [])

    def test_unexpanded_run_between_thresholds_warns(self):
        snap = {"unexpanded_runs": [unexpanded(44)]}
        self.assertEqual(levels(qaw.analyze(snap, NOW)), ["warn"])

    def test_unexpanded_detection_is_narrow_to_pull_request_build_workflow(self):
        snap = {
            "unexpanded_runs": [
                unexpanded(90, workflow="Docs"),
                unexpanded(90, status="in_progress"),
                unexpanded(90, event="push"),
                unexpanded(90, event="merge_group"),
                unexpanded(90, event="workflow_dispatch"),
                unexpanded(90, workflow_path=".github/workflows/other.yml"),
            ]
        }
        self.assertEqual(qaw.analyze(snap, NOW), [])


class TestDegradedSweep(unittest.TestCase):
    """Incomplete evidence must never alarm — an unobserved lane isn't a dead one."""

    def test_collection_errors_suppress_an_otherwise_valid_alarm(self):
        snap = {
            "queued_jobs": [queued(90)],
            "live_jobs": [],
            "errors": [{"run_id": 7, "status": "in_progress", "error": "HTTP 502"}],
        }
        findings = qaw.analyze(snap, NOW)
        self.assertEqual(levels(findings), ["warn"])
        self.assertEqual(findings[0]["lane_evidence"], "evidence incomplete this sweep")

    def test_the_same_snapshot_without_errors_does_alarm(self):
        # Proves the suppression above is the errors key doing the work, not a
        # coincidence of the fixture.
        snap = {"queued_jobs": [queued(90)], "live_jobs": [], "errors": []}
        self.assertEqual(levels(qaw.analyze(snap, NOW)), ["alarm"])

    def test_summary_discloses_a_degraded_sweep(self):
        summary = qaw.render_summary([], qaw.ALARM_MINUTES, [{"error": "boom"}])
        self.assertIn("Degraded sweep", summary)
        self.assertIn("alarms are suppressed", summary)

    def test_summary_is_clean_when_nothing_failed(self):
        self.assertNotIn("Degraded", qaw.render_summary([], qaw.ALARM_MINUTES, []))

    def test_a_blind_sweep_never_reports_the_fleet_healthy(self):
        # "I found nothing" is not "there is nothing". A degraded sweep with no
        # findings must not read as a clean bill of health.
        summary = qaw.render_summary([], qaw.ALARM_MINUTES, [{"error": "boom"}])
        self.assertNotIn("healthy", summary)
        self.assertIn("not a clean bill of health", summary)

    def test_collection_errors_suppress_unexpanded_run_alarm(self):
        snap = {
            "unexpanded_runs": [unexpanded(90)],
            "errors": [{"run_id": 7, "status": "in_progress", "error": "HTTP 502"}],
        }
        findings = qaw.analyze(snap, NOW)
        self.assertEqual(levels(findings), ["warn"])
        self.assertEqual(findings[0]["lane_evidence"], "evidence incomplete this sweep")

    def test_truncated_collection_suppresses_unexpanded_run_alarm(self):
        snap = {
            "unexpanded_runs": [unexpanded(90)],
            "truncated": ["pending"],
        }
        findings = qaw.analyze(snap, NOW)
        self.assertEqual(levels(findings), ["warn"])
        self.assertEqual(findings[0]["lane_evidence"], "evidence incomplete this sweep")


class TestCollection(unittest.TestCase):
    def test_collects_pending_build_and_test_run_only_after_empty_jobs_response(self):
        pending_runs = {
            "workflow_runs": [
                {
                    "id": 99,
                    "name": "Build and Test",
                    "status": "pending",
                    "event": "pull_request",
                    "path": ".github/workflows/build.yml",
                    "head_sha": "a" * 40,
                    "created_at": ago(60),
                    "html_url": "https://example.invalid/run/99",
                    "head_branch": "feature/example",
                },
                {
                    "id": 100,
                    "name": "Docs",
                    "status": "pending",
                    "event": "pull_request",
                    "path": ".github/workflows/docs.yml",
                    "head_sha": "b" * 40,
                    "created_at": ago(60),
                },
            ]
        }

        def api(path):
            if "status=pending" in path:
                return pending_runs
            if path.endswith("/runs/99/jobs?per_page=100"):
                return {"total_count": 0, "jobs": []}
            if path.endswith("/runs/100/jobs?per_page=100"):
                return {"total_count": 0, "jobs": []}
            if path.endswith("/runs/99"):
                return pending_runs["workflow_runs"][0]
            return {"workflow_runs": []}

        with mock.patch.object(qaw, "_gh_api", side_effect=api):
            snapshot = qaw.collect_snapshot("Generous-Corp/pulp", NOW)

        self.assertEqual(snapshot["errors"], [])
        self.assertEqual(len(snapshot["unexpanded_runs"]), 1)
        self.assertEqual(snapshot["unexpanded_runs"][0]["run_id"], 99)

    def test_collects_queued_build_and_test_run_with_empty_jobs(self):
        run = {
            "id": 102,
            "name": "Build and Test",
            "status": "queued",
            "event": "pull_request",
            "path": ".github/workflows/build.yml",
            "head_sha": "a" * 40,
            "created_at": ago(60),
        }

        def api(path):
            if "status=queued" in path:
                return {"workflow_runs": [run]}
            if path.endswith("/runs/102/jobs?per_page=100"):
                return {"total_count": 0, "jobs": []}
            if path.endswith("/runs/102"):
                return run
            return {"workflow_runs": []}

        with mock.patch.object(qaw, "_gh_api", side_effect=api):
            snapshot = qaw.collect_snapshot("Generous-Corp/pulp", NOW)

        self.assertEqual(len(snapshot["unexpanded_runs"]), 1)
        self.assertEqual(snapshot["unexpanded_runs"][0]["status"], "queued")

    def test_missing_jobs_field_is_degraded_not_zero_job_evidence(self):
        run = {
            "id": 103,
            "name": "Build and Test",
            "status": "pending",
            "event": "pull_request",
            "path": ".github/workflows/build.yml",
            "head_sha": "a" * 40,
            "created_at": ago(60),
        }

        def api(path):
            if "status=pending" in path:
                return {"workflow_runs": [run]}
            if path.endswith("/runs/103/jobs?per_page=100"):
                return {}
            return {"workflow_runs": []}

        with mock.patch.object(qaw, "_gh_api", side_effect=api):
            snapshot = qaw.collect_snapshot("Generous-Corp/pulp", NOW)

        self.assertEqual(snapshot["unexpanded_runs"], [])
        self.assertEqual(len(snapshot["errors"]), 1)

    def test_empty_jobs_requires_total_count_zero(self):
        run = {
            "id": 105,
            "name": "Build and Test",
            "status": "pending",
            "event": "pull_request",
            "path": ".github/workflows/build.yml",
            "head_sha": "a" * 40,
            "created_at": ago(60),
        }

        def api(path):
            if "status=pending" in path:
                return {"workflow_runs": [run]}
            if path.endswith("/runs/105/jobs?per_page=100"):
                return {"total_count": 1, "jobs": []}
            return {"workflow_runs": []}

        with mock.patch.object(qaw, "_gh_api", side_effect=api):
            snapshot = qaw.collect_snapshot("Generous-Corp/pulp", NOW)

        self.assertEqual(snapshot["unexpanded_runs"], [])
        self.assertEqual(len(snapshot["errors"]), 1)

    def test_exact_run_reread_must_match_list_evidence(self):
        run = {
            "id": 106,
            "name": "Build and Test",
            "status": "pending",
            "event": "pull_request",
            "path": ".github/workflows/build.yml",
            "head_sha": "a" * 40,
            "created_at": ago(60),
        }

        def api(path):
            if "status=pending" in path:
                return {"workflow_runs": [run]}
            if path.endswith("/runs/106/jobs?per_page=100"):
                return {"total_count": 0, "jobs": []}
            if path.endswith("/runs/106"):
                return {**run, "status": "in_progress"}
            return {"workflow_runs": []}

        with mock.patch.object(qaw, "_gh_api", side_effect=api):
            snapshot = qaw.collect_snapshot("Generous-Corp/pulp", NOW)

        self.assertEqual(snapshot["unexpanded_runs"], [])
        self.assertEqual(snapshot["errors"], [])

    def test_target_status_transition_degrades_instead_of_proving_recovery(self):
        run = {
            "id": 112,
            "name": "Build and Test",
            "status": "pending",
            "event": "pull_request",
            "path": ".github/workflows/build.yml",
            "head_sha": "a" * 40,
            "created_at": ago(60),
        }

        def api(path):
            if "status=pending" in path:
                return {"workflow_runs": [run]}
            if path.endswith("/runs/112/jobs?per_page=100"):
                return {"total_count": 0, "jobs": []}
            if path.endswith("/runs/112"):
                return {**run, "status": "queued"}
            return {"workflow_runs": []}

        with mock.patch.object(qaw, "_gh_api", side_effect=api):
            snapshot = qaw.collect_snapshot("Generous-Corp/pulp", NOW)

        self.assertEqual(snapshot["unexpanded_runs"], [])
        self.assertEqual(len(snapshot["errors"]), 1)

    def test_second_jobs_read_must_remain_empty(self):
        run = {
            "id": 113,
            "name": "Build and Test",
            "status": "pending",
            "event": "pull_request",
            "path": ".github/workflows/build.yml",
            "head_sha": "a" * 40,
            "created_at": ago(60),
        }
        jobs_calls = 0

        def api(path):
            nonlocal jobs_calls
            if "status=pending" in path:
                return {"workflow_runs": [run]}
            if path.endswith("/runs/113/jobs?per_page=100"):
                jobs_calls += 1
                if jobs_calls == 1:
                    return {"total_count": 0, "jobs": []}
                return {"total_count": 1, "jobs": [{"status": "queued"}]}
            if path.endswith("/runs/113"):
                return run
            return {"workflow_runs": []}

        with mock.patch.object(qaw, "_gh_api", side_effect=api):
            snapshot = qaw.collect_snapshot("Generous-Corp/pulp", NOW)

        self.assertEqual(jobs_calls, 2)
        self.assertEqual(snapshot["unexpanded_runs"], [])
        self.assertEqual(snapshot["errors"], [])

    def test_exact_run_identity_mismatch_degrades_the_sweep(self):
        run = {
            "id": 111,
            "name": "Build and Test",
            "status": "pending",
            "event": "pull_request",
            "path": ".github/workflows/build.yml",
            "head_sha": "a" * 40,
            "created_at": ago(60),
        }

        def api(path):
            if "status=pending" in path:
                return {"workflow_runs": [run]}
            if path.endswith("/runs/111/jobs?per_page=100"):
                return {"total_count": 0, "jobs": []}
            if path.endswith("/runs/111"):
                return {**run, "head_sha": "b" * 40}
            return {"workflow_runs": []}

        with mock.patch.object(qaw, "_gh_api", side_effect=api):
            snapshot = qaw.collect_snapshot("Generous-Corp/pulp", NOW)

        self.assertEqual(snapshot["unexpanded_runs"], [])
        self.assertEqual(len(snapshot["errors"]), 1)

    def test_non_pull_request_events_are_excluded_before_exact_reread(self):
        for index, event in enumerate(("push", "merge_group", "workflow_dispatch"), 107):
            with self.subTest(event=event):
                run = {
                    "id": index,
                    "name": "Build and Test",
                    "status": "pending",
                    "event": event,
                    "path": ".github/workflows/build.yml",
                    "head_sha": "a" * 40,
                    "created_at": ago(60),
                }

                def api(path):
                    if "status=pending" in path:
                        return {"workflow_runs": [run]}
                    if path.endswith(f"/runs/{index}/jobs?per_page=100"):
                        return {"total_count": 0, "jobs": []}
                    if path.endswith(f"/runs/{index}"):
                        self.fail("excluded event reached exact-run reread")
                    return {"workflow_runs": []}

                with mock.patch.object(qaw, "_gh_api", side_effect=api):
                    snapshot = qaw.collect_snapshot("Generous-Corp/pulp", NOW)

                self.assertEqual(snapshot["unexpanded_runs"], [])

    def test_build_and_test_run_with_a_job_is_not_unexpanded(self):
        run = {
            "id": 101,
            "name": "Build and Test",
            "status": "queued",
            "event": "pull_request",
            "path": ".github/workflows/build.yml",
            "head_sha": "a" * 40,
            "created_at": ago(60),
        }

        def api(path):
            if "status=queued" in path:
                return {"workflow_runs": [run]}
            if path.endswith("/runs/101/jobs?per_page=100"):
                return {
                    "total_count": 1,
                    "jobs": [
                        {
                            "status": "queued",
                            "name": "macos",
                            "labels": MAC,
                            "created_at": ago(60),
                        }
                    ]
                }
            return {"workflow_runs": []}

        with mock.patch.object(qaw, "_gh_api", side_effect=api):
            snapshot = qaw.collect_snapshot("Generous-Corp/pulp", NOW)

        self.assertEqual(snapshot["unexpanded_runs"], [])
        self.assertEqual(len(snapshot["queued_jobs"]), 1)

    def test_deduplicates_run_ids_across_status_pages(self):
        run = {
            "id": 104,
            "name": "Build and Test",
            "status": "pending",
            "event": "pull_request",
            "path": ".github/workflows/build.yml",
            "head_sha": "a" * 40,
            "created_at": ago(60),
        }
        jobs_calls = 0

        def api(path):
            nonlocal jobs_calls
            if "status=pending" in path:
                return {"workflow_runs": [run]}
            if "status=queued" in path:
                return {"workflow_runs": [{**run, "status": "queued"}]}
            if path.endswith("/runs/104/jobs?per_page=100"):
                jobs_calls += 1
                return {"total_count": 0, "jobs": []}
            if path.endswith("/runs/104"):
                return run
            return {"workflow_runs": []}

        with mock.patch.object(qaw, "_gh_api", side_effect=api):
            snapshot = qaw.collect_snapshot("Generous-Corp/pulp", NOW)

        self.assertEqual(len(snapshot["unexpanded_runs"]), 1)
        self.assertEqual(jobs_calls, 2)
        self.assertEqual(len(snapshot["errors"]), 1)


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

    def test_body_names_unexpanded_run_and_does_not_call_it_a_runner_failure(self):
        findings = qaw.analyze({"unexpanded_runs": [unexpanded(61)]}, NOW)
        body = qaw.render_body(findings, qaw.ALARM_MINUTES, NOW)
        self.assertIn("Workflow runs with zero jobs", body)
        self.assertIn("run `99`", body)
        self.assertIn("`pending` with zero jobs", body)
        self.assertNotIn("wants labels", body)


class TestWorkflowWiring(unittest.TestCase):
    def test_degraded_sweep_cannot_maintain_or_close_tracker(self):
        workflow = (
            Path(__file__).resolve().parents[2]
            / ".github/workflows/runner-health-check.yml"
        ).read_text(encoding="utf-8")
        self.assertIn("echo \"degraded=${degraded}\" >> \"$GITHUB_OUTPUT\"", workflow)
        self.assertIn(
            "if: env.DRY_RUN != 'true' && steps.scan.outputs.degraded != 'true'",
            workflow,
        )
        self.assertIn("s.get('errors') or s.get('truncated')", workflow)


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

    def test_unexpanded_run_snapshot_writes_actionable_body(self):
        snap = {
            "generated_at": NOW.isoformat(),
            "unexpanded_runs": [unexpanded(75)],
        }
        proc, tmp = self._run(snap)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("alarm_count=1", proc.stdout)
        body = (tmp / "body.md").read_text(encoding="utf-8")
        self.assertIn("run `99`", body)
        self.assertIn("same ref", body)

    def test_degraded_snapshot_exposes_machine_readable_output(self):
        snap = {
            "generated_at": NOW.isoformat(),
            "errors": [{"error": "boom"}],
        }
        proc, _ = self._run(snap)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("degraded=true", proc.stdout)

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
