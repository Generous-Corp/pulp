#!/usr/bin/env python3
"""Tests for merge_stall_watchdog.py.

The load-bearing behavior is the two-condition, two-tick predicate: only a PR
that is genuinely merge-ready (required-green + CLEAN/BEHIND + auto-merge) AND
has stayed that way across two consecutive sweeps AND for longer than the age
threshold may trip. Everything else — a young PR, a conflicted PR, a
required-check-red PR, a one-sweep blip — must stay quiet, and a merge (the PR
leaving the open set) must let the tracker close.
"""
from __future__ import annotations

import datetime as dt
import json
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import merge_stall_watchdog as msw  # noqa: E402

NOW = dt.datetime(2026, 7, 16, 12, 0, 0, tzinfo=dt.timezone.utc)
REQUIRED = ["macos", "Enforce version & skill sync"]


def ago(minutes: float) -> str:
    return (NOW - dt.timedelta(minutes=minutes)).isoformat().replace("+00:00", "Z")


def green_checks(green_age_min: float, names=None) -> dict[str, dict]:
    """All required checks green, last one having completed green_age_min ago."""
    names = names or REQUIRED
    return {n: {"green": True, "completed_at": ago(green_age_min)} for n in names}


def pr(
    number: int,
    *,
    merge_state="CLEAN",
    auto_merge=True,
    is_draft=False,
    checks=None,
    green_age_min=60.0,
    title="a change",
) -> dict:
    return {
        "number": number,
        "title": title,
        "url": f"https://example.invalid/pr/{number}",
        "is_draft": is_draft,
        "merge_state_status": merge_state,
        "auto_merge_enabled": auto_merge,
        "checks": green_checks(green_age_min) if checks is None else checks,
    }


def snap(*prs) -> dict:
    return {
        "generated_at": NOW.isoformat(),
        "required_checks": list(REQUIRED),
        "required_checks_source": "branch-protection",
        "open_prs": list(prs),
    }


def levels(findings):
    return [f["level"] for f in findings]


class TestTrips(unittest.TestCase):
    def test_green_behind_pr_stuck_two_ticks_alarms(self):
        # The exact incident shape: a green, BEHIND, auto-merge PR that has been
        # merge-ready for an hour. First sweep: pending. Second consecutive
        # sweep: alarm.
        s = snap(pr(101, merge_state="BEHIND", green_age_min=60))

        f1, stuck1 = msw.analyze(s, prev_stuck=[], now=NOW)
        self.assertEqual(levels(f1), ["pending"])
        self.assertEqual(stuck1, [101])

        f2, stuck2 = msw.analyze(s, prev_stuck=stuck1, now=NOW)
        self.assertEqual(levels(f2), ["alarm"])
        self.assertEqual(f2[0]["number"], 101)
        self.assertEqual(f2[0]["merge_state_status"], "BEHIND")
        self.assertEqual(stuck2, [101])

    def test_clean_pr_stuck_two_ticks_alarms(self):
        s = snap(pr(102, merge_state="CLEAN", green_age_min=90))
        _, stuck1 = msw.analyze(s, prev_stuck=[], now=NOW)
        f2, _ = msw.analyze(s, prev_stuck=stuck1, now=NOW)
        self.assertEqual(levels(f2), ["alarm"])


class TestQuietCases(unittest.TestCase):
    """Every one of these MUST produce zero alarms."""

    def test_young_green_pr_does_not_trip(self):
        # Merge-ready for only 20 min: under the 45-min threshold. Not even
        # pending, no matter how many sweeps it is seen.
        s = snap(pr(201, merge_state="CLEAN", green_age_min=20))
        f1, stuck1 = msw.analyze(s, prev_stuck=[], now=NOW)
        self.assertEqual(f1, [])
        self.assertEqual(stuck1, [])
        # Even if it were somehow in the previous set, age still gates it.
        f2, _ = msw.analyze(s, prev_stuck=[201], now=NOW)
        self.assertEqual(f2, [])

    def test_dirty_pr_never_trips(self):
        # Conflicts with base. Waiting on the author, not the merger.
        s = snap(pr(202, merge_state="DIRTY", green_age_min=120))
        f1, stuck1 = msw.analyze(s, prev_stuck=[], now=NOW)
        self.assertEqual(f1, [])
        f2, _ = msw.analyze(s, prev_stuck=[202], now=NOW)
        self.assertEqual(f2, [])

    def test_blocked_pr_never_trips(self):
        # BLOCKED = a required check red/missing or a review pending. Waiting on
        # something real, not on a wedged merger.
        s = snap(pr(203, merge_state="BLOCKED", green_age_min=120))
        f2, _ = msw.analyze(s, prev_stuck=[203], now=NOW)
        self.assertEqual(f2, [])

    def test_unstable_pr_never_trips(self):
        # A non-required check still moving; required set is green but GitHub's
        # verdict is UNSTABLE, which the predicate excludes.
        s = snap(pr(204, merge_state="UNSTABLE", green_age_min=120))
        f2, _ = msw.analyze(s, prev_stuck=[204], now=NOW)
        self.assertEqual(f2, [])

    def test_required_check_red_does_not_trip(self):
        # CLEAN would not actually happen with a red required check, but guard
        # the predicate directly: one required check not green -> not stuck.
        checks = green_checks(90)
        checks["macos"] = {"green": False, "completed_at": ago(90)}
        s = snap(pr(205, merge_state="CLEAN", checks=checks, green_age_min=90))
        f2, _ = msw.analyze(s, prev_stuck=[205], now=NOW)
        self.assertEqual(f2, [])

    def test_missing_required_check_does_not_trip(self):
        # A required check absent entirely (pending / never started) is not
        # green. Only the non-required checks are present.
        checks = {"Enforce version & skill sync": {"green": True, "completed_at": ago(90)}}
        s = snap(pr(206, merge_state="CLEAN", checks=checks, green_age_min=90))
        f2, _ = msw.analyze(s, prev_stuck=[206], now=NOW)
        self.assertEqual(f2, [])

    def test_no_auto_merge_does_not_trip(self):
        # Green + CLEAN but auto-merge not enabled: waiting on a human to press
        # merge, which is not a wedged-merger stall.
        s = snap(pr(207, merge_state="CLEAN", auto_merge=False, green_age_min=120))
        f2, _ = msw.analyze(s, prev_stuck=[207], now=NOW)
        self.assertEqual(f2, [])

    def test_draft_does_not_trip(self):
        s = snap(pr(208, merge_state="CLEAN", is_draft=True, green_age_min=120))
        f2, _ = msw.analyze(s, prev_stuck=[208], now=NOW)
        self.assertEqual(f2, [])

    def test_single_tick_blip_stays_pending(self):
        # Qualifies this sweep but was NOT stuck last sweep: pending only, never
        # alarm. This is the anti-flap guard against a transient false reading.
        s = snap(pr(209, merge_state="CLEAN", green_age_min=90))
        f1, _ = msw.analyze(s, prev_stuck=[], now=NOW)
        self.assertEqual(levels(f1), ["pending"])
        self.assertNotIn("alarm", levels(f1))


class TestRecovery(unittest.TestCase):
    def test_merge_clears_the_finding(self):
        # Two sweeps stuck -> alarm. Then the PR merges: it leaves the open set,
        # so the third sweep produces zero findings and an empty stuck set,
        # which is what lets the workflow close the tracker.
        s = snap(pr(301, merge_state="BEHIND", green_age_min=60))
        _, stuck1 = msw.analyze(s, prev_stuck=[], now=NOW)
        f2, stuck2 = msw.analyze(s, prev_stuck=stuck1, now=NOW)
        self.assertEqual(levels(f2), ["alarm"])

        merged = snap()  # the stuck PR is no longer in the open set
        f3, stuck3 = msw.analyze(merged, prev_stuck=stuck2, now=NOW)
        self.assertEqual(f3, [])
        self.assertEqual(stuck3, [])

    def test_pr_becomes_dirty_clears_the_finding(self):
        # Recovery need not be a merge: if the stuck PR goes DIRTY (someone
        # pushed a conflicting base), it drops out of the stuck set and the
        # tracker can close.
        stuck = snap(pr(302, merge_state="CLEAN", green_age_min=90))
        _, stuck1 = msw.analyze(stuck, prev_stuck=[], now=NOW)
        f2, _ = msw.analyze(stuck, prev_stuck=stuck1, now=NOW)
        self.assertEqual(levels(f2), ["alarm"])

        now_dirty = snap(pr(302, merge_state="DIRTY", green_age_min=95))
        f3, stuck3 = msw.analyze(now_dirty, prev_stuck=stuck1, now=NOW)
        self.assertEqual(f3, [])
        self.assertEqual(stuck3, [])


class TestRequiredCheckResolution(unittest.TestCase):
    def test_snapshot_required_set_is_honored(self):
        # The predicate uses whatever required set the snapshot carries (read
        # from branch protection at runtime), not a hardcoded list. A PR green
        # on the documented set but NOT on a custom required set does not trip.
        s = snap(pr(401, merge_state="CLEAN", green_age_min=90))
        s["required_checks"] = ["macos", "Enforce version & skill sync", "linux"]
        f2, _ = msw.analyze(s, prev_stuck=[401], now=NOW)
        self.assertEqual(f2, [])  # "linux" is required but not present/green

    def test_default_required_set_used_when_snapshot_omits_it(self):
        s = {
            "open_prs": [
                pr(
                    402,
                    merge_state="CLEAN",
                    checks=green_checks(90, msw.DEFAULT_REQUIRED_CHECKS),
                )
            ]
        }
        _, stuck1 = msw.analyze(s, prev_stuck=[], now=NOW)
        f2, _ = msw.analyze(s, prev_stuck=stuck1, now=NOW)
        self.assertEqual(levels(f2), ["alarm"])


class TestMultiPrPopulation(unittest.TestCase):
    def test_alarms_and_pendings_coexist_sorted(self):
        stuck_old = pr(501, merge_state="BEHIND", green_age_min=120)
        stuck_new = pr(502, merge_state="CLEAN", green_age_min=60)
        young = pr(503, merge_state="CLEAN", green_age_min=10)
        dirty = pr(504, merge_state="DIRTY", green_age_min=120)
        s = snap(stuck_old, stuck_new, young, dirty)

        # Previous sweep only saw 501 as stuck -> 501 alarms, 502 pending,
        # 503 too young, 504 dirty.
        findings, stuck_now = msw.analyze(s, prev_stuck=[501], now=NOW)
        self.assertEqual(levels(findings), ["alarm", "pending"])
        self.assertEqual([f["number"] for f in findings], [501, 502])
        self.assertEqual(stuck_now, [501, 502])


class TestMergeQueueStall(unittest.TestCase):
    def queue_snapshot(self, *, age=60, depth=16, last_batch_age=60):
        return {
            "merge_queue": {
                "depth": depth,
                "last_merge_group_started_at": ago(last_batch_age),
                "head": {
                    "number": 6997,
                    "title": "queue head",
                    "url": "https://example.invalid/pr/6997",
                    "state": "AWAITING_CHECKS",
                    "enqueued_at": ago(age),
                    "blocking_checks": [
                        {"name": "macos", "state": "QUEUED", "url": ""}
                    ],
                },
            }
        }

    def test_old_queue_and_old_batch_alarm_in_one_sweep(self):
        findings = msw.analyze_merge_queue(self.queue_snapshot(), NOW)
        self.assertEqual([f["level"] for f in findings], ["queue_alarm"])
        self.assertEqual(findings[0]["queue_depth"], 16)
        self.assertEqual(findings[0]["blocking_checks"][0]["name"], "macos")

    def test_recent_batch_keeps_old_queue_quiet(self):
        findings = msw.analyze_merge_queue(
            self.queue_snapshot(age=120, last_batch_age=10), NOW
        )
        self.assertEqual(findings, [])

    def test_young_head_is_not_stalled(self):
        self.assertEqual(
            msw.analyze_merge_queue(self.queue_snapshot(age=10), NOW), []
        )

    def test_empty_or_unavailable_queue_is_quiet(self):
        self.assertEqual(msw.analyze_merge_queue({}, NOW), [])
        self.assertEqual(
            msw.analyze_merge_queue(self.queue_snapshot(depth=0), NOW), []
        )


class TestMergeQueueCollection(unittest.TestCase):
    def test_collection_rejects_graphql_errors_in_http_200_response(self):
        payload = {
            "data": {"repository": {"mergeQueue": None}},
            "errors": [{"message": "temporary schema failure"}],
        }
        with mock.patch.object(msw, "_gh", return_value=json.dumps(payload)):
            with self.assertRaisesRegex(KeyError, "merge queue query failed"):
                msw.collect_merge_queue("Generous-Corp/pulp", "main", REQUIRED)

    def test_collection_scopes_queue_and_runs_to_requested_base(self):
        queue_payload = {
            "data": {
                "repository": {
                    "mergeQueue": {
                        "entries": {"totalCount": 0, "nodes": []}
                    }
                }
            }
        }
        runs_payload = {
            "workflow_runs": [
                {
                    "head_branch": "gh-readonly-queue/release/pr-9-old",
                    "run_started_at": "2026-07-16T11:59:00Z",
                },
                {
                    "head_branch": "gh-readonly-queue/main/pr-8-base",
                    "run_started_at": "2026-07-16T11:30:00Z",
                },
            ]
        }
        calls: list[list[str]] = []

        def fake_gh(args):
            calls.append(args)
            return json.dumps(queue_payload if "graphql" in args else runs_payload)

        with mock.patch.object(msw, "_gh", side_effect=fake_gh):
            result = msw.collect_merge_queue("Generous-Corp/pulp", "main", REQUIRED)

        self.assertNotIn("last_merge_group_started_at", result)
        self.assertIn("branch=main", calls[0])
        self.assertIn("mergeQueue(branch:$branch)", msw._MERGE_QUEUE_QUERY)

    def test_collection_reads_blockers_from_current_merge_group_sha(self):
        queue_payload = {
            "data": {
                "repository": {
                    "mergeQueue": {
                        "configuration": {"maximumEntriesToBuild": 2},
                        "entries": {
                            "totalCount": 2,
                            "nodes": [
                                {
                                    "position": 1,
                                    "enqueuedAt": "2026-07-16T11:00:00Z",
                                    "state": "AWAITING_CHECKS",
                                    "pullRequest": {
                                        "number": 42,
                                        "title": "queued",
                                        "url": "https://example.invalid/pr/42",
                                    },
                                    "headCommit": {"oid": "queue-head-red"},
                                },
                                {
                                    "position": 2,
                                    "enqueuedAt": "2026-07-16T11:05:00Z",
                                    "state": "QUEUED",
                                    "pullRequest": {
                                        "number": 99,
                                        "title": "cumulative follower",
                                        "url": "https://example.invalid/pr/99",
                                    },
                                    "headCommit": {"oid": "unrelated-newer"},
                                }
                            ],
                        }
                    }
                }
            }
        }
        runs_payload = {
            "workflow_runs": [
                {
                    "head_branch": "gh-readonly-queue/main/pr-99-unrelated",
                    "head_sha": "unrelated-newer",
                    "created_at": "2026-07-16T11:55:00Z",
                    "run_started_at": "2026-07-16T11:56:00Z",
                },
                {
                    "head_branch": "gh-readonly-queue/main/pr-42-current",
                    "head_sha": "merge-group-red",
                    "created_at": "2026-07-16T11:30:00Z",
                    # A rerun/start delay must not refresh batch formation.
                    "run_started_at": "2026-07-16T11:59:00Z",
                }
            ]
        }
        rollup_payload = {
            "data": {
                "repository": {
                    "object": {
                        "oid": "merge-group-red",
                        "statusCheckRollup": {
                            "contexts": {
                                "nodes": [
                                    {
                                        "__typename": "CheckRun",
                                        "name": "macos",
                                        "status": "QUEUED",
                                        "conclusion": None,
                                        "detailsUrl": "https://example.invalid/check",
                                    }
                                ]
                            }
                        },
                    }
                }
            }
        }

        def fake_gh(args):
            if "graphql" not in args:
                return json.dumps(runs_payload)
            query = next(arg for arg in args if arg.startswith("query="))
            return json.dumps(
                rollup_payload if "object(oid:$oid)" in query else queue_payload
            )

        with mock.patch.object(msw, "_gh", side_effect=fake_gh):
            result = msw.collect_merge_queue(
                "Generous-Corp/pulp", "main", REQUIRED
            )

        self.assertEqual(result["head"]["queue_head_sha"], "queue-head-red")
        self.assertEqual(result["head"]["merge_group_sha"], "merge-group-red")
        self.assertEqual(
            result["last_merge_group_started_at"], "2026-07-16T11:55:00Z"
        )
        self.assertEqual(result["maximum_entries_to_build"], 2)
        blockers = {
            check["name"]: check for check in result["head"]["blocking_checks"]
        }
        self.assertEqual(blockers["macos"]["state"], "QUEUED")

    def test_completed_failure_reports_its_conclusion(self):
        commit = {
            "statusCheckRollup": {
                "contexts": {
                    "nodes": [
                        {
                            "__typename": "CheckRun",
                            "name": "macos",
                            "status": "COMPLETED",
                            "conclusion": "FAILURE",
                            "detailsUrl": "https://example.invalid/check",
                        }
                    ]
                }
            }
        }
        blockers = msw._blocking_contexts(commit, {"macos"})
        self.assertEqual(blockers[0]["state"], "FAILURE")


class TestMainCli(unittest.TestCase):
    def test_snapshot_and_prev_state_roundtrip_writes_alarm(self):
        # Drive main() end-to-end over files, as the workflow does: a stuck PR
        # whose number is in the prev-state file must produce an alarm body and
        # a state file naming it.
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / "snap.json").write_text(
                json.dumps(snap(pr(601, merge_state="BEHIND", green_age_min=90)))
            )
            (d / "prev.json").write_text(json.dumps({"stuck_prs": [601]}))
            rc = msw.main(
                [
                    "--snapshot", str(d / "snap.json"),
                    "--prev-state", str(d / "prev.json"),
                    "--findings-out", str(d / "findings.json"),
                    "--state-out", str(d / "state.json"),
                    "--body-out", str(d / "body.md"),
                ]
            )
            self.assertEqual(rc, 0)
            findings = json.loads((d / "findings.json").read_text())
            self.assertEqual([f["level"] for f in findings], ["alarm"])
            state = json.loads((d / "state.json").read_text())
            self.assertEqual(state["stuck_prs"], [601])
            body = (d / "body.md").read_text()
            self.assertIn("#601", body)
            self.assertIn("merge-stall-check.yml", body)

    def test_no_findings_writes_no_body(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / "snap.json").write_text(
                json.dumps(snap(pr(602, merge_state="DIRTY", green_age_min=90)))
            )
            rc = msw.main(
                [
                    "--snapshot", str(d / "snap.json"),
                    "--findings-out", str(d / "findings.json"),
                    "--state-out", str(d / "state.json"),
                    "--body-out", str(d / "body.md"),
                ]
            )
            self.assertEqual(rc, 0)
            self.assertFalse((d / "body.md").exists())
            self.assertEqual(json.loads((d / "state.json").read_text())["stuck_prs"], [])

    def test_degraded_snapshot_preserves_previous_two_sweep_state(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            degraded = snap()
            degraded["errors"] = [{"stage": "graphql", "error": "timeout"}]
            (d / "snap.json").write_text(json.dumps(degraded))
            (d / "prev.json").write_text(json.dumps({"stuck_prs": [601]}))

            rc = msw.main(
                [
                    "--snapshot", str(d / "snap.json"),
                    "--prev-state", str(d / "prev.json"),
                    "--findings-out", str(d / "findings.json"),
                    "--state-out", str(d / "state.json"),
                    "--body-out", str(d / "body.md"),
                ]
            )

            self.assertEqual(rc, 0)
            state = json.loads((d / "state.json").read_text())
            self.assertEqual(state["stuck_prs"], [601])

    def test_open_pr_graphql_errors_mark_live_snapshot_degraded(self):
        queue_payload = {
            "data": {
                "repository": {
                    "mergeQueue": {
                        "entries": {"totalCount": 0, "nodes": []}
                    }
                }
            }
        }
        runs_payload = {"workflow_runs": []}
        pr_error_payload = {
            "data": {"repository": None},
            "errors": [{"message": "temporary authorization failure"}],
        }

        def fake_gh(args):
            if "graphql" not in args:
                return json.dumps(runs_payload)
            query = next(arg for arg in args if arg.startswith("query="))
            if "mergeQueue(branch:$branch)" in query:
                return json.dumps(queue_payload)
            return json.dumps(pr_error_payload)

        with (
            mock.patch.object(msw, "_gh", side_effect=fake_gh),
            mock.patch.object(
                msw,
                "resolve_required_checks",
                return_value=(REQUIRED, "branch-protection"),
            ),
        ):
            snapshot = msw.collect_snapshot(
                "Generous-Corp/pulp", "main", NOW
            )

        self.assertEqual(snapshot["open_prs"], [])
        graphql_errors = [
            err for err in snapshot["errors"] if err["stage"] == "graphql"
        ]
        self.assertEqual(len(graphql_errors), 1)
        self.assertIn("authorization failure", graphql_errors[0]["error"])

    def test_workflow_counts_queue_alarms_for_issue_maintenance(self):
        workflow = (
            Path(__file__).resolve().parents[2]
            / ".github"
            / "workflows"
            / "merge-stall-check.yml"
        ).read_text(encoding="utf-8")
        # Every actionable level must reach COUNT, or the sweep detects a
        # stall and then declines to open the tracker that reports it.
        for level in ("alarm", "queue_alarm", "throughput_alarm", "blind_alarm"):
            self.assertIn(f"'{level}'", workflow)
        self.assertIn('degraded=${degraded}', workflow)
        self.assertIn('if [ "$DEGRADED" = "true" ]', workflow)

    def test_this_suite_is_actually_executed_by_ci(self):
        """A test nothing runs cannot fail, so it guards nothing.

        This file existed and asserted real behavior while no workflow ever
        invoked it. Pin the registration so it cannot silently lapse again.
        """
        lint = (
            Path(__file__).resolve().parents[2]
            / ".github"
            / "workflows"
            / "workflow-lint.yml"
        ).read_text(encoding="utf-8")
        self.assertIn(
            "python3 tools/scripts/test_merge_stall_watchdog.py", lint
        )
        self.assertIn("'tools/scripts/merge_stall_watchdog.py'", lint)

    def test_documented_fallback_contains_every_main_required_context(self):
        self.assertEqual(
            set(msw.DEFAULT_REQUIRED_CHECKS),
            {
                "macos",
                "Enforce version & skill sync",
                "Build + prove + (owner-gated) deploy",
                "Vellum trusted freeze",
                "Vellum freeze",
            },
        )


class TestChecksFromRollup(unittest.TestCase):
    """The GraphQL-rollup flattener that feeds analyze() in the live path."""

    def test_checkrun_success_is_green(self):
        commit = {
            "statusCheckRollup": {
                "contexts": {
                    "nodes": [
                        {
                            "__typename": "CheckRun",
                            "name": "macos",
                            "status": "COMPLETED",
                            "conclusion": "SUCCESS",
                            "completedAt": ago(50),
                        }
                    ]
                }
            }
        }
        checks = msw._checks_from_rollup(commit)
        self.assertTrue(checks["macos"]["green"])

    def test_checkrun_failure_is_not_green(self):
        commit = {
            "statusCheckRollup": {
                "contexts": {
                    "nodes": [
                        {
                            "__typename": "CheckRun",
                            "name": "macos",
                            "status": "COMPLETED",
                            "conclusion": "FAILURE",
                            "completedAt": ago(50),
                        }
                    ]
                }
            }
        }
        self.assertFalse(msw._checks_from_rollup(commit)["macos"]["green"])

    def test_in_progress_checkrun_is_not_green(self):
        commit = {
            "statusCheckRollup": {
                "contexts": {
                    "nodes": [
                        {
                            "__typename": "CheckRun",
                            "name": "macos",
                            "status": "IN_PROGRESS",
                            "conclusion": None,
                            "completedAt": None,
                        }
                    ]
                }
            }
        }
        self.assertFalse(msw._checks_from_rollup(commit)["macos"]["green"])


class MergeThroughputTests(unittest.TestCase):
    """The outcome heartbeat: nothing merging while work waits to merge."""

    @staticmethod
    def snap(last_merge_minutes_ago, depth, online=3, **extra):
        snapshot = {
            "generated_at": NOW.isoformat(),
            "merge_throughput": {
                "last_merge_at": ago(last_merge_minutes_ago),
                "last_merge_sha": "abc123def456",
            },
            "merge_queue": {"depth": depth},
            "runner_capacity": {"registered": online, "online": online, "busy": 0},
            "errors": [],
        }
        snapshot.update(extra)
        return snapshot

    def test_alarms_when_queue_waits_and_nothing_merges(self):
        found = msw.analyze_merge_throughput(self.snap(340, 5), NOW, 90)
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0]["level"], "throughput_alarm")
        self.assertEqual(found[0]["queue_depth"], 5)
        self.assertAlmostEqual(found[0]["minutes_since_last_merge"], 340, places=0)

    def test_empty_queue_is_never_an_alarm(self):
        """The denominator. A quiet repo merging nothing is correct."""
        self.assertEqual(
            msw.analyze_merge_throughput(self.snap(10_000, 0), NOW, 90), []
        )

    def test_recent_merge_is_quiet(self):
        self.assertEqual(
            msw.analyze_merge_throughput(self.snap(20, 5), NOW, 90), []
        )

    def test_dead_fleet_does_not_suppress_the_alarm(self):
        """Capacity is diagnosis, never a condition: zero runners still alarms."""
        found = msw.analyze_merge_throughput(self.snap(340, 5, online=0), NOW, 90)
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0]["runners_online"], 0)

    def test_alarm_is_never_invented_from_a_failed_read(self):
        """A missing input must not manufacture an alarm; blindness owns it."""
        blind = self.snap(340, 5)
        del blind["merge_queue"]
        self.assertEqual(msw.analyze_merge_throughput(blind, NOW, 90), [])


class TransientRetryTests(unittest.TestCase):
    """A slow minute upstream must not blind the guard for a whole sweep."""

    def _proc(self, code, stderr=""):
        return subprocess.CompletedProcess(
            args=["gh"], returncode=code, stdout="{}", stderr=stderr
        )

    def test_a_504_is_retried_and_can_succeed(self):
        calls = []

        def fake_run(args, **kwargs):
            calls.append(args)
            if len(calls) < 3:
                return self._proc(1, "gh: HTTP 504 We couldn't respond in time")
            return self._proc(0)

        with (
            mock.patch.object(msw.subprocess, "run", side_effect=fake_run),
            mock.patch.object(msw.time, "sleep"),
        ):
            self.assertEqual(msw._gh(["api", "graphql"]), "{}")
        self.assertEqual(len(calls), 3)

    def test_a_terminal_failure_is_not_retried(self):
        """Retrying a real error is background load, never a fix."""
        calls = []

        def fake_run(args, **kwargs):
            calls.append(args)
            return self._proc(1, "gh: HTTP 401 Bad credentials")

        with (
            mock.patch.object(msw.subprocess, "run", side_effect=fake_run),
            mock.patch.object(msw.time, "sleep"),
        ):
            with self.assertRaises(subprocess.CalledProcessError):
                msw._gh(["api", "graphql"])
        self.assertEqual(len(calls), 1)

    def test_retries_are_bounded(self):
        calls = []

        def fake_run(args, **kwargs):
            calls.append(args)
            return self._proc(1, "gh: HTTP 504 timeout")

        with (
            mock.patch.object(msw.subprocess, "run", side_effect=fake_run),
            mock.patch.object(msw.time, "sleep"),
        ):
            with self.assertRaises(subprocess.CalledProcessError):
                msw._gh(["api", "graphql"])
        self.assertEqual(len(calls), msw._GH_RETRIES)

    def test_pr_page_size_stays_within_the_budget_that_completes(self):
        """The 50-per-page query timed out on every sweep for days."""
        self.assertIn("first: 25", msw._PR_QUERY)


class BlindSweepTests(unittest.TestCase):
    """A sweep that observed nothing must never render as calm."""

    def test_failed_reads_raise_a_blind_alarm(self):
        snapshot = {
            "errors": [
                {"stage": "merge-queue", "error": "boom"},
                {"stage": "graphql", "error": "boom"},
            ],
            "open_prs": [],
            "merge_queue": None,
        }
        found = msw.analyze_blindness(snapshot)
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0]["level"], "blind_alarm")
        self.assertEqual(found[0]["failed_stages"], ["graphql", "merge-queue"])

    def test_blind_alarm_names_the_verdicts_it_could_not_reach(self):
        found = msw.analyze_blindness(
            {"errors": [{"stage": "merge-queue", "error": "boom"}]}
        )
        self.assertEqual(
            found[0]["silenced_alarms"], ["queue_alarm", "throughput_alarm"]
        )

    def test_a_complete_sweep_is_silent(self):
        """Control: the guard can be quiet, so a red above is real signal."""
        self.assertEqual(msw.analyze_blindness({"errors": []}), [])
        self.assertEqual(msw.analyze_blindness({}), [])

    def test_measured_outage_snapshot_is_no_longer_reported_as_calm(self):
        """Regression on the real 2026-09-21 05:55Z sweep.

        Both collection calls failed, so the watchdog held no observation at
        all, yet rendered "Merges are flowing" and exited green while the
        repository had merged nothing for four hours.
        """
        snapshot = {
            "generated_at": NOW.isoformat(),
            "repo": "Generous-Corp/pulp",
            "base": "main",
            "open_prs": [],
            "merge_queue": None,
            "errors": [
                {"stage": "merge-queue", "error": "Command '['gh', ...]'"},
                {"stage": "graphql", "error": "Command '['gh', ...]'"},
            ],
        }
        findings = msw.analyze_blindness(snapshot)
        findings.extend(msw.analyze_merge_throughput(snapshot, NOW, 90))
        self.assertTrue(findings, "a blind sweep must produce a finding")

        summary = msw.render_summary(findings, 45, snapshot["errors"])
        self.assertNotIn("Merges are flowing", summary)
        self.assertIn("blind sweep", summary)

        body = msw.render_body(findings, 45, NOW)
        self.assertIn("could not observe", body)
        self.assertIn("not evidence of health", body)

    def test_renderers_accept_every_new_level(self):
        """The summary table indexes per-level keys; a miss would KeyError."""
        findings = [
            {
                "level": "throughput_alarm",
                "minutes_since_last_merge": 340.0,
                "last_merge_at": ago(340),
                "last_merge_sha": "abc123def456",
                "queue_depth": 5,
                "runners_online": 3,
                "threshold_minutes": 90,
            },
            {
                "level": "blind_alarm",
                "failed_stages": ["merge-queue"],
                "silenced_alarms": ["queue_alarm", "throughput_alarm"],
                "detail": [{"stage": "merge-queue", "error": "boom"}],
            },
        ]
        summary = msw.render_summary(findings, 45, None)
        self.assertIn("throughput alarm", summary)
        self.assertIn("blind alarm", summary)
        body = msw.render_body(findings, 45, NOW)
        self.assertIn("Nothing is merging", body)


if __name__ == "__main__":
    unittest.main()
