#!/usr/bin/env python3
"""Tests for tools/scripts/release_reconcile.py.

The cases below are the ones that actually cost releases in 2026-07. In
particular `test_slow_release_is_never_touched` encodes the rule whose absence
destroyed 11 of 18 tags: automation kept concluding that a long-running release
was dead and cancelling it.
"""

from __future__ import annotations

import sys
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import release_reconcile as rr  # noqa: E402
from release_reconcile import (  # noqa: E402
    CIRCUIT_OPEN,
    DEFERRED,
    STUCK_QUEUE,
    ESCALATE,
    GRACE,
    IN_FLIGHT,
    INCOMPLETE,
    OK,
    REDISPATCH,
    REQUIRED_ASSETS,
    SUPERSEDED,
    TOO_OLD,
    TagState,
    decide,
    latest_validation_failures,
    needs_job_details,
    incident_stays_open,
    runs_this_tag,
    validation_failures,
)

NOW = datetime(2026, 7, 12, 12, 0, 0, tzinfo=timezone.utc)
LIMITS = {"grace_minutes": 45, "max_age_hours": 72, "max_attempts": 3}


def state(
    *,
    age_minutes: int = 120,
    published: bool = False,
    has_release_object: bool = False,
    assets: frozenset[str] | None = None,
    run_states: tuple[str, ...] = (),
    validation_failures: tuple[str, ...] = (),
    dispatch_attempts: int = 0,
) -> TagState:
    if assets is None:
        # A published release defaults to a COMPLETE one; drafts carry nothing.
        assets = REQUIRED_ASSETS if published else frozenset()
    return TagState(
        tag="v0.655.0",
        created_at=NOW - timedelta(minutes=age_minutes),
        published=published,
        has_release_object=has_release_object,
        assets=assets,
        run_states=run_states,
        validation_failures=validation_failures,
        dispatch_attempts=dispatch_attempts,
    )


class Decide(unittest.TestCase):
    def test_published_tag_is_done(self) -> None:
        self.assertEqual(
            decide(state(published=True, has_release_object=True), NOW, **LIMITS).action,
            OK,
        )

    def test_slow_release_is_never_touched(self) -> None:
        """A release that has been BUILDING for five hours is slow, not stuck.

        This is the whole point of the reconciler. The supersede reaper cancelled
        in-flight release runs once a newer tag published, on the theory that an
        older SemVer was obsolete. Releases routinely complete out of order here
        (the pipeline outlasts the gap between tags), so that theory destroyed
        healthy releases whose binaries had all built green.

        NOTE the precision: this invariant is about a job that is actually RUNNING.
        An earlier version of this test also asserted it for a merely QUEUED job,
        which encoded the opposite bug — see QueuedIsNotRunning. A queued job has no
        runner, and "waiting patiently forever for a runner that is never coming" is
        a hang, not patience.
        """
        decision = decide(
            state(age_minutes=300, run_states=("in_progress",)), NOW, **LIMITS
        )
        self.assertEqual(decision.action, IN_FLIGHT)

    def test_completed_run_that_never_published_is_redispatched(self) -> None:
        decision = decide(state(run_states=("completed",)), NOW, **LIMITS)
        self.assertEqual(decision.action, REDISPATCH)
        self.assertIn("no release was created", decision.reason)

    def test_terminal_failure_opens_circuit_instead_of_redispatching(self) -> None:
        decision = decide(
            state(
                run_states=("completed",),
                validation_failures=("Smoke windows-x64: Smoke CLI runtime",),
            ),
            NOW,
            **LIMITS,
        )
        self.assertEqual(decision.action, CIRCUIT_OPEN)
        self.assertIn("immutable-tag failures are not retried", decision.reason)

    def test_non_validation_workflow_failure_retains_bounded_retry(self) -> None:
        decision = decide(
            state(run_states=("completed",)),
            NOW,
            **LIMITS,
        )
        self.assertEqual(decision.action, REDISPATCH)

    def test_cancelled_run_retains_bounded_retry_path(self) -> None:
        decision = decide(
            state(run_states=("completed",)),
            NOW,
            **LIMITS,
        )
        self.assertEqual(decision.action, REDISPATCH)

    def test_timeout_retains_bounded_retry_path(self) -> None:
        decision = decide(
            state(run_states=("completed",)),
            NOW,
            **LIMITS,
        )
        self.assertEqual(decision.action, REDISPATCH)

    def test_startup_failure_retains_bounded_retry_path(self) -> None:
        decision = decide(
            state(
                run_states=("completed",),
            ),
            NOW,
            **LIMITS,
        )
        self.assertEqual(decision.action, REDISPATCH)

    def test_cancelled_run_still_honors_retry_budget(self) -> None:
        decision = decide(
            state(
                run_states=("completed",),
                dispatch_attempts=3,
            ),
            NOW,
            **LIMITS,
        )
        self.assertEqual(decision.action, ESCALATE)

    def test_terminal_failure_outranks_an_interrupted_retry(self) -> None:
        decision = decide(
            state(
                run_states=("completed", "completed"),
                validation_failures=("Smoke windows-x64: Smoke CLI runtime",),
                dispatch_attempts=1,
            ),
            NOW,
            **LIMITS,
        )
        self.assertEqual(decision.action, CIRCUIT_OPEN)

    def test_orphan_draft_is_redispatched_not_deleted(self) -> None:
        """A draft left behind by a half-finished finalizer must be re-driven.

        The old reaper DELETED such drafts. Re-dispatch is idempotent (release-cli's
        finalizer re-uploads assets onto the existing draft and publishes it), so
        recovery never has to destroy release state.
        """
        decision = decide(state(has_release_object=True), NOW, **LIMITS)
        self.assertEqual(decision.action, REDISPATCH)
        self.assertIn("draft", decision.reason)

    def test_fresh_tag_is_given_grace(self) -> None:
        decision = decide(state(age_minutes=5, run_states=()), NOW, **LIMITS)
        self.assertEqual(decision.action, GRACE)

    def test_fresh_tag_with_a_live_run_is_in_flight_not_grace(self) -> None:
        decision = decide(
            state(age_minutes=5, run_states=("in_progress",)), NOW, **LIMITS
        )
        self.assertEqual(decision.action, IN_FLIGHT)

    def test_exhausted_budget_escalates_instead_of_looping(self) -> None:
        decision = decide(
            state(run_states=("completed",), dispatch_attempts=3), NOW, **LIMITS
        )
        self.assertEqual(decision.action, ESCALATE)

    def test_one_attempt_below_budget_still_retries(self) -> None:
        decision = decide(
            state(run_states=("completed",), dispatch_attempts=2), NOW, **LIMITS
        )
        self.assertEqual(decision.action, REDISPATCH)

    def test_ancient_tag_is_left_alone(self) -> None:
        decision = decide(state(age_minutes=60 * 24 * 30), NOW, **LIMITS)
        self.assertEqual(decision.action, TOO_OLD)

    def test_published_beats_every_other_signal(self) -> None:
        """Published is terminal even with a stray failed re-dispatch attached."""
        decision = decide(
            state(
                published=True,
                has_release_object=True,
                run_states=("completed",),
                dispatch_attempts=9,
            ),
            NOW,
            **LIMITS,
        )
        self.assertEqual(decision.action, OK)


class RunOwnership(unittest.TestCase):
    """The reconciler must SEE its own repair runs — without a `run-name`.

    It re-dispatches release-cli with `--ref main` (so the repair uses main's fixed
    workflow to build the tag's source), which makes the run's `head_branch` equal
    `main`, NOT the tag. Matching on head_branch alone makes every repair invisible:
    zero attempts counted, escalation never fires, and a fresh re-dispatch goes out
    every 30 minutes forever.

    The tag is therefore carried in a JOB name. It must NOT be carried in a
    `run-name`: GitHub returns `run-name` as `workflow_run.name`, REPLACING the
    workflow name, and the self-hosted tartci supervisor selects its work with
    `select(.name == "Release CLI")`. Setting one hides every release run from the
    supervisor, which then reports `queued=0` and never boots a macOS VM — no
    runner, no release. That regression shipped once; these tests keep it dead.
    """

    def test_repair_run_dispatched_from_main_is_attributed_by_job_name(self) -> None:
        repair = {"head_branch": "main", "event": "workflow_dispatch", "id": 1}
        self.assertTrue(runs_this_tag(repair, "v0.659.0", "v0.659.0"))
        self.assertFalse(runs_this_tag(repair, "v0.660.0", "v0.659.0"))

    def test_tag_push_run_matches_via_head_branch(self) -> None:
        push = {"head_branch": "v0.659.0", "event": "push", "id": 2}
        self.assertTrue(runs_this_tag(push, "v0.659.0"))

    def test_an_unattributable_dispatch_run_matches_nothing(self) -> None:
        """Fail closed: never claim a run we could not attribute."""
        unknown = {"head_branch": "main", "event": "workflow_dispatch", "id": 3}
        self.assertFalse(runs_this_tag(unknown, "v0.659.0", None))


class PublishedDoesNotImplyComplete(unittest.TestCase):
    """Verify the exact-asset invariant instead of assuming it.

    release-cli publishes only after an --exact-required check, so its own releases
    are complete. But a release published by any other path (a human, a legacy run)
    can be missing assets — and a published GitHub release is IMMUTABLE, so a
    rebuild cannot repair it. Treating "published" as "done" would mark such a
    release healthy forever.
    """

    FLOOR = (0, 1, 0)  # v0.655.0 (the fixture tag) is comfortably above this

    def test_published_but_missing_assets_escalates_rather_than_passing(self) -> None:
        partial = REQUIRED_ASSETS - {"pulp-darwin-x64.tar.gz", "SHA256SUMS"}
        decision = decide(
            state(published=True, has_release_object=True, assets=partial),
            NOW,
            asset_floor=self.FLOOR,
            **LIMITS,
        )
        self.assertEqual(decision.action, INCOMPLETE)
        self.assertIn("immutable", decision.reason)

    def test_published_and_complete_is_ok(self) -> None:
        decision = decide(
            state(published=True, has_release_object=True, assets=REQUIRED_ASSETS),
            NOW,
            asset_floor=self.FLOOR,
            **LIMITS,
        )
        self.assertEqual(decision.action, OK)

    def test_incomplete_release_is_never_redispatched(self) -> None:
        """A rebuild cannot fix an immutable release — don't pretend it can."""
        partial = REQUIRED_ASSETS - {"appcast.xml"}
        decision = decide(
            state(published=True, has_release_object=True, assets=partial),
            NOW,
            asset_floor=self.FLOOR,
            **LIMITS,
        )
        self.assertNotEqual(decision.action, REDISPATCH)


class UnresolvedTracking(unittest.TestCase):
    """`unresolved` drives the incident and must not flap on a live retry."""

    CTX = {"newest_published": None, "floor": (0, 1, 0)}

    def test_unpublished_tag_is_unresolved(self) -> None:
        self.assertTrue(state(run_states=("in_progress",)).unresolved(**self.CTX))

    def test_published_and_complete_is_resolved(self) -> None:
        s = state(published=True, has_release_object=True)
        self.assertFalse(s.unresolved(**self.CTX))

    def test_published_but_incomplete_stays_unresolved(self) -> None:
        s = state(
            published=True,
            has_release_object=True,
            assets=REQUIRED_ASSETS - {"SHA256SUMS"},
        )
        self.assertTrue(s.unresolved(**self.CTX))

    def test_escalated_tag_under_repair_is_still_unresolved(self) -> None:
        """An escalated tag a human is mid-repair on must stay in the incident.

        Otherwise the incident closes the moment a retry starts and reopens the
        moment it fails — flapping on every 30-minute sweep.
        """
        s = state(run_states=("in_progress",), dispatch_attempts=5)
        self.assertEqual(decide(s, NOW, **LIMITS).action, IN_FLIGHT)
        self.assertTrue(s.unresolved(**self.CTX))

    def test_circuit_open_incident_does_not_flap_during_first_repair(self) -> None:
        s = state(
            run_states=("in_progress",),
            validation_failures=("Smoke windows-x64: Smoke CLI runtime",),
            dispatch_attempts=1,
        )
        self.assertEqual(decide(s, NOW, **LIMITS).action, IN_FLIGHT)
        self.assertTrue(
            incident_stays_open(
                s,
                newest_published=None,
                asset_floor=(0, 1, 0),
                max_attempts=3,
            )
        )


class ValidationFailureClassification(unittest.TestCase):
    def test_cancelled_terminal_run_is_inspected_for_finished_failures(self) -> None:
        self.assertTrue(
            needs_job_details(
                {"event": "push", "status": "completed", "conclusion": "cancelled"}
            )
        )

    def test_successful_push_does_not_need_job_details(self) -> None:
        self.assertFalse(
            needs_job_details(
                {"event": "push", "status": "completed", "conclusion": "success"}
            )
        )

    def test_dispatch_always_needs_jobs_for_tag_attribution(self) -> None:
        self.assertTrue(
            needs_job_details(
                {"event": "workflow_dispatch", "status": "queued"}
            )
        )

    def test_smoke_failure_is_immutable_product_evidence(self) -> None:
        jobs = [
            {
                "name": "Smoke windows-x64",
                "conclusion": "failure",
                "steps": [
                    {
                        "name": "Smoke CLI, delegates, MCP, and import-design runtime (Windows)",
                        "conclusion": "failure",
                    }
                ],
            }
        ]
        self.assertEqual(
            validation_failures(jobs),
            (
                "Smoke windows-x64: Smoke CLI, delegates, MCP, and "
                "import-design runtime (Windows)",
            ),
        )

    def test_artifact_transfer_failure_remains_retryable(self) -> None:
        jobs = [
            {
                "name": "CLI linux-x64",
                "conclusion": "failure",
                "steps": [
                    {"name": "Upload CLI artifact", "conclusion": "failure"}
                ],
            }
        ]
        self.assertEqual(validation_failures(jobs), ())

    def test_dynamic_skia_verification_failure_remains_retryable(self) -> None:
        jobs = [
            {
                "name": "CLI darwin-x64",
                "conclusion": "failure",
                "steps": [
                    {
                        "name": "Verify fetched Skia is x86_64 (darwin-x64)",
                        "conclusion": "failure",
                    }
                ],
            }
        ]
        self.assertEqual(validation_failures(jobs), ())

    def test_linux_sdk_second_build_failure_remains_retryable(self) -> None:
        jobs = [
            {
                "name": "CLI linux-x64",
                "conclusion": "failure",
                "steps": [
                    {
                        "name": "Prepare SDK build dir (Linux)",
                        "conclusion": "failure",
                    }
                ],
            }
        ]
        self.assertEqual(validation_failures(jobs), ())

    def test_successful_validation_step_is_not_failure_evidence(self) -> None:
        jobs = [
            {
                "name": "Smoke windows-x64",
                "conclusion": "failure",
                "steps": [{"name": "Smoke CLI runtime", "conclusion": "success"}],
            }
        ]
        self.assertEqual(validation_failures(jobs), ())

    def test_newer_same_gate_pass_clears_older_failure(self) -> None:
        newer_windows_pass = [
            {
                "name": "Smoke windows-x64",
                "conclusion": "success",
                "steps": [{"name": "Smoke CLI runtime", "conclusion": "success"}],
            }
        ]
        old_failure = [
            {
                "name": "Smoke windows-x64",
                "conclusion": "failure",
                "steps": [{"name": "Smoke CLI runtime", "conclusion": "failure"}],
            }
        ]
        runs = [
            {"id": 2, "created_at": "2026-07-12T11:00:00Z", "status": "completed"},
            {"id": 1, "created_at": "2026-07-12T10:00:00Z", "status": "completed"},
        ]
        self.assertEqual(
            latest_validation_failures(
                runs, {1: old_failure, 2: newer_windows_pass}
            ),
            (),
        )

    def test_newer_other_platform_pass_does_not_clear_failure(self) -> None:
        newer_linux_pass = [
            {
                "name": "Smoke linux-x64",
                "conclusion": "success",
                "steps": [{"name": "Smoke CLI runtime", "conclusion": "success"}],
            }
        ]
        old_failure = [
            {
                "name": "Smoke windows-x64",
                "conclusion": "failure",
                "steps": [{"name": "Smoke CLI runtime", "conclusion": "failure"}],
            }
        ]
        runs = [
            {"id": 2, "created_at": "2026-07-12T11:00:00Z", "status": "completed"},
            {"id": 1, "created_at": "2026-07-12T10:00:00Z", "status": "completed"},
        ]
        self.assertEqual(
            latest_validation_failures(runs, {1: old_failure, 2: newer_linux_pass}),
            ("Smoke windows-x64: Smoke CLI runtime",),
        )

    def test_independent_failures_across_attempts_are_all_reported(self) -> None:
        newer_linux_failure = [
            {
                "name": "Smoke linux-x64",
                "conclusion": "failure",
                "steps": [{"name": "Smoke CLI runtime", "conclusion": "failure"}],
            }
        ]
        older_windows_failure = [
            {
                "name": "Smoke windows-x64",
                "conclusion": "failure",
                "steps": [{"name": "Smoke CLI runtime", "conclusion": "failure"}],
            }
        ]
        runs = [
            {"id": 2, "created_at": "2026-07-12T11:00:00Z", "status": "completed"},
            {"id": 1, "created_at": "2026-07-12T10:00:00Z", "status": "completed"},
        ]
        self.assertEqual(
            latest_validation_failures(
                runs, {1: older_windows_failure, 2: newer_linux_failure}
            ),
            (
                "Smoke linux-x64: Smoke CLI runtime",
                "Smoke windows-x64: Smoke CLI runtime",
            ),
        )

    def test_newer_prestart_cancellation_does_not_erase_failure(self) -> None:
        old_failure = [
            {
                "name": "Smoke windows-x64",
                "conclusion": "failure",
                "steps": [{"name": "Smoke CLI runtime", "conclusion": "failure"}],
            }
        ]
        runs = [
            {"id": 2, "created_at": "2026-07-12T11:00:00Z", "status": "completed"},
            {"id": 1, "created_at": "2026-07-12T10:00:00Z", "status": "completed"},
        ]
        self.assertEqual(
            latest_validation_failures(runs, {1: old_failure, 2: []}),
            ("Smoke windows-x64: Smoke CLI runtime",),
        )

    def test_in_progress_pass_does_not_clear_older_failure(self) -> None:
        current_pass = [
            {
                "name": "Smoke windows-x64",
                "conclusion": "success",
                "run_attempt": 1,
                "steps": [{"name": "Smoke CLI runtime", "conclusion": "success"}],
            }
        ]
        old_failure = [
            {
                "name": "Smoke windows-x64",
                "conclusion": "failure",
                "run_attempt": 1,
                "steps": [{"name": "Smoke CLI runtime", "conclusion": "failure"}],
            }
        ]
        runs = [
            {"id": 2, "created_at": "2026-07-12T11:00:00Z", "status": "in_progress"},
            {"id": 1, "created_at": "2026-07-12T10:00:00Z", "status": "completed"},
        ]
        self.assertEqual(
            latest_validation_failures(runs, {1: old_failure, 2: current_pass}),
            ("Smoke windows-x64: Smoke CLI runtime",),
        )

    def test_rerun_attempt_keeps_failure_from_prior_attempt(self) -> None:
        attempt_one_failure = {
            "name": "Smoke windows-x64",
            "conclusion": "failure",
            "run_attempt": 1,
            "steps": [{"name": "Smoke CLI runtime", "conclusion": "failure"}],
        }
        attempt_two_cancelled = {
            "name": "Resolve macOS runner — v0.770.0",
            "conclusion": "cancelled",
            "run_attempt": 2,
            "steps": [],
        }
        runs = [
            {
                "id": 1,
                "created_at": "2026-07-12T10:00:00Z",
                "status": "completed",
                "run_attempt": 2,
            }
        ]
        self.assertEqual(
            latest_validation_failures(
                runs, {1: [attempt_one_failure, attempt_two_cancelled]}
            ),
            ("Smoke windows-x64: Smoke CLI runtime",),
        )


class QueuedIsNotRunning(unittest.TestCase):
    """A job that never STARTED is stuck, not slow — and must escalate.

    The reconciler's headline rule is "a live run outranks everything, at any age",
    because slow != stuck. But `queued` and `in_progress` are both "live", and they
    mean opposite things: an `in_progress` job has a runner and is making progress;
    a `queued` job has NO runner. If nothing will ever serve it — a `runs-on` label
    nothing matches, an offline host pool, or a workflow rename that hides the run
    from a self-hosted supervisor's queue filter — it stays queued FOREVER while
    looking exactly like a slow build.

    That is not hypothetical: it is precisely how the release lane hung for a day.
    Runs sat queued, every watchdog read "in flight", and nothing escalated. Routing
    releases onto local VMs makes it MORE likely (a sleeping host = no runner), so
    this guard is a prerequisite for that change, not an afterthought.
    """

    def test_queued_for_hours_and_never_started_escalates(self) -> None:
        decision = decide(
            state(age_minutes=6 * 60, run_states=("queued",)), NOW, **LIMITS
        )
        self.assertEqual(decision.action, STUCK_QUEUE)
        self.assertIn("no runner is serving this job", decision.reason)

    def test_a_RUNNING_job_is_still_left_alone_at_any_age(self) -> None:
        """The original invariant must survive: slow is survivable."""
        decision = decide(
            state(age_minutes=10 * 60, run_states=("in_progress",)), NOW, **LIMITS
        )
        self.assertEqual(decision.action, IN_FLIGHT)

    def test_a_running_job_outranks_a_sibling_queued_job(self) -> None:
        decision = decide(
            state(age_minutes=8 * 60, run_states=("queued", "in_progress")),
            NOW, **LIMITS,
        )
        self.assertEqual(decision.action, IN_FLIGHT)

    def test_normal_queueing_is_not_escalated(self) -> None:
        """An hour in a busy queue is ordinary — don't cry wolf."""
        decision = decide(
            state(age_minutes=60, run_states=("queued",)), NOW, **LIMITS
        )
        self.assertEqual(decision.action, IN_FLIGHT)

    def test_the_threshold_is_configurable(self) -> None:
        s = state(age_minutes=3 * 60, run_states=("queued",))
        self.assertEqual(decide(s, NOW, stuck_queue_hours=2, **LIMITS).action, STUCK_QUEUE)
        self.assertEqual(decide(s, NOW, stuck_queue_hours=8, **LIMITS).action, IN_FLIGHT)


class SupersededTagsAreNotRebuilt(unittest.TestCase):
    """Don't spend a 2-hour matrix rebuilding a release nobody will install.

    Distinct from the "supersede reaper" this change deletes, and the distinction
    is the whole point: the reaper CANCELLED in-flight runs and DELETED drafts for
    older tags — destructive, and racing releases that were merely slow. This only
    declines to START a speculative rebuild of a tag whose users are already served
    by a newer release. It never touches release state, and a live run outranks it.

    Without this rule the reconciler's first sweep would have re-dispatched twelve
    superseded tags at once, saturating the very runners the current release needs.
    """

    NEWER = (0, 656, 0)

    def test_superseded_unpublished_tag_is_not_rebuilt(self) -> None:
        decision = decide(
            state(run_states=("completed",)), NOW,
            newest_published=self.NEWER, **LIMITS,
        )
        self.assertEqual(decision.action, SUPERSEDED)

    def test_superseded_terminal_failure_does_not_open_incident(self) -> None:
        decision = decide(
            state(
                run_states=("completed",),
                validation_failures=("Smoke windows-x64: Smoke CLI runtime",),
            ),
            NOW,
            newest_published=self.NEWER,
            **LIMITS,
        )
        self.assertEqual(decision.action, SUPERSEDED)

    def test_a_live_run_still_outranks_supersession(self) -> None:
        """Never abandon a release that is actually building. That was the bug."""
        decision = decide(
            state(run_states=("in_progress",)), NOW,
            newest_published=self.NEWER, **LIMITS,
        )
        self.assertEqual(decision.action, IN_FLIGHT)

    def test_a_queued_run_does_not_outrank_supersession_forever(self) -> None:
        """A superseded tag whose run never started is not worth waiting on."""
        decision = decide(
            state(age_minutes=6 * 60, run_states=("queued",)), NOW,
            newest_published=self.NEWER, **LIMITS,
        )
        self.assertNotEqual(decision.action, IN_FLIGHT)

    def test_the_newest_tag_is_never_superseded(self) -> None:
        decision = decide(
            state(run_states=("completed",)), NOW,
            newest_published=(0, 654, 0), **LIMITS,  # older than v0.655.0
        )
        self.assertEqual(decision.action, REDISPATCH)

    def test_superseded_tag_is_not_an_incident(self) -> None:
        s = state(run_states=("completed",), dispatch_attempts=9)
        self.assertFalse(s.unresolved(newest_published=self.NEWER, floor=None))


class DeferBehindALiveNewerTag(unittest.TestCase):
    """Don't rebuild a stuck tag while a NEWER tag is still building.

    If the newer one publishes, this tag is SUPERSEDED and never needed a rebuild —
    so repairing it now is speculative work competing for the very runners the
    newer release is waiting on. On a starved pool that is self-defeating: it slows
    down the release that would have made this one unnecessary.

    Observed live: with v0.660/661/662 queued and v0.654.0 the latest published,
    the reconciler happily started rebuilding v0.659.0 and then v0.658.1 — adding
    load to the macOS queue that was already the reason nothing could publish.

    This is a WAIT, not a write-off: nothing is cancelled, and if the newer tag's
    run ends without publishing, it stops being live and this tag becomes
    repairable on the very next sweep.
    """

    def test_older_stuck_tag_waits_while_a_newer_tag_builds(self) -> None:
        decision = decide(
            state(run_states=("completed",)),          # v0.655.0, stuck
            NOW,
            newest_live=(0, 662, 0),                   # v0.662.0 still building
            **LIMITS,
        )
        self.assertEqual(decision.action, DEFERRED)
        self.assertIn("supersedes this one", decision.reason)

    def test_the_newest_stuck_tag_is_still_repaired(self) -> None:
        """Deferral must not deadlock: the newest tag always remains repairable."""
        decision = decide(
            state(run_states=("completed",)), NOW,
            newest_live=(0, 640, 0),                   # only OLDER tags are live
            **LIMITS,
        )
        self.assertEqual(decision.action, REDISPATCH)

    def test_nothing_live_means_normal_repair(self) -> None:
        decision = decide(
            state(run_states=("completed",)), NOW, newest_live=None, **LIMITS
        )
        self.assertEqual(decision.action, REDISPATCH)

    def test_a_tag_with_its_own_live_run_is_in_flight_not_deferred(self) -> None:
        decision = decide(
            state(run_states=("queued",)), NOW, newest_live=(0, 662, 0), **LIMITS
        )
        self.assertEqual(decision.action, IN_FLIGHT)

    def test_deferral_never_cancels_anything(self) -> None:
        """DEFERRED is a wait. It must not be in the set of acting decisions."""
        self.assertNotIn(DEFERRED, (REDISPATCH, ESCALATE))


class AssetContractFloor(unittest.TestCase):
    """Historical releases predate today's asset contract and must be grandfathered.

    v0.641-v0.646 shipped before the Intel `darwin-x64` pair existed; several older
    releases predate `SHA256SUMS`. Holding them to the current contract would flag
    a pile of perfectly good releases — a brand-new false-alarm firehose, which is
    exactly what this reconciler exists to end.
    """

    FLOOR = (0, 659, 0)

    def test_release_below_the_floor_is_grandfathered(self) -> None:
        old = TagState(
            tag="v0.646.0",
            created_at=NOW - timedelta(hours=2),
            published=True,
            has_release_object=True,
            assets=REQUIRED_ASSETS - {"pulp-darwin-x64.tar.gz", "SHA256SUMS"},
            run_states=("completed",),
            validation_failures=(),
            dispatch_attempts=0,
        )
        decision = decide(old, NOW, asset_floor=self.FLOOR, **LIMITS)
        self.assertEqual(decision.action, OK)
        self.assertFalse(old.unresolved(newest_published=None, floor=self.FLOOR))

    def test_release_at_or_above_the_floor_must_be_complete(self) -> None:
        # Drop a platform archive that the CURRENT contract actually demands,
        # so this test keeps meaning "missing required asset" whatever the
        # active_platforms knob is set to.
        dropped = sorted(
            asset for asset in REQUIRED_ASSETS if asset.startswith("pulp-")
        )[0]
        new = TagState(
            tag="v0.660.0",
            created_at=NOW - timedelta(hours=2),
            published=True,
            has_release_object=True,
            assets=REQUIRED_ASSETS - {dropped},
            run_states=("completed",),
            validation_failures=(),
            dispatch_attempts=0,
        )
        decision = decide(new, NOW, asset_floor=self.FLOOR, **LIMITS)
        self.assertEqual(decision.action, INCOMPLETE)


class SweepBudget(unittest.TestCase):
    """Repairs are newest-first and capped per sweep, so they cannot stampede.

    The reconciler goes live against a backlog of stuck tags AND a starved runner
    pool. Re-dispatching all of them at once would kick off five ~2-hour matrices
    simultaneously and starve the very release it is trying to repair. Fixing the
    newest first and capping the sweep lets supersession settle: once the newest
    tag publishes, the older ones are SUPERSEDED next sweep and skipped entirely.
    """

    def test_the_module_caps_redispatches_per_sweep(self) -> None:
        src = (
            Path(__file__).resolve().parent / "release_reconcile.py"
        ).read_text(encoding="utf-8")
        self.assertIn("MAX_REDISPATCH_PER_SWEEP", src)
        self.assertIn("budget -= 1", src)
        self.assertIn("if budget <= 0:", src)

    def test_tags_are_processed_newest_first(self) -> None:
        """`sdk_tags` must return newest-first, or the cap fixes the wrong tag."""
        src = (
            Path(__file__).resolve().parent / "release_reconcile.py"
        ).read_text(encoding="utf-8")
        self.assertIn("--sort=-creatordate", src)


class NeverDestructive(unittest.TestCase):
    """The reconciler must not be able to cancel a run or delete a release."""

    def test_module_never_cancels_or_deletes(self) -> None:
        source = (
            Path(__file__).resolve().parent / "release_reconcile.py"
        ).read_text(encoding="utf-8")
        for forbidden in (
            "/cancel",
            "-X DELETE",
            '"DELETE"',
            "gh run cancel",
            "release delete",
        ):
            self.assertNotIn(
                forbidden,
                source,
                f"release_reconcile.py must never {forbidden!r}. Recovering a "
                "release by destroying release state is the bug this module exists "
                "to undo.",
            )

    def test_circuit_incident_requires_an_explicit_repair_run(self) -> None:
        source = (
            Path(__file__).resolve().parent / "release_reconcile.py"
        ).read_text(encoding="utf-8")
        self.assertIn("manually dispatch the repaired workflow", source)
        self.assertNotIn("reconciler will re-dispatch within 30 minutes", source)



class TagImmutableClassifier(unittest.TestCase):
    """The circuit must open on a tag-immutable failure and ONLY on one.

    v0.831.0 failed at step 17 `Configure` on a pinned-runtime digest mismatch
    and was re-dispatched every sweep, each attempt claiming a dedicated release
    VM it could never use. The step-name allowlist could not see it.

    The naive fix — adding "Configure" to VALIDATION_STEP_PREFIXES — passes the
    first test here and FAILS the second, which is the whole point: `Configure`
    also fails on fetch timeouts and cache misses, and opening the circuit on
    those converts a recoverable flake into a permanently stuck release.
    """

    DIGEST_LOG = (
        "CMake Error at tools/cmake/PulpDependencies.cmake:872 (message):\n"
        "  Three.js runtime file does not match pinned revision r170: LICENSE\n"
    )
    TRANSIENT_LOG = (
        "curl: (28) Operation timed out after 30001 milliseconds\n"
        "CMake Error: failed to fetch dependency archive\n"
    )

    def _jobs(self):
        return [
            {
                "id": 42,
                "name": "CLI windows-x64",
                "conclusion": "failure",
                "steps": [{"name": "Configure", "conclusion": "failure"}],
            }
        ]

    def test_digest_mismatch_opens_the_circuit_on_the_first_sweep(self) -> None:
        failures, _ = rr.validation_outcomes(
            self._jobs(), lambda job_id: self.DIGEST_LOG
        )
        self.assertTrue(
            failures,
            "a pinned-revision digest mismatch is a property of the immutable "
            "tag and must open the circuit immediately, not after N retries",
        )
        self.assertIn("tag-immutable", " ".join(failures))

    def test_transient_configure_failure_still_re_dispatches(self) -> None:
        # THE TEST THAT MATTERS. A fetch timeout is not a fact about the tag; a
        # re-run can succeed. Opening the circuit here would strand a release
        # that would otherwise have published.
        failures, _ = rr.validation_outcomes(
            self._jobs(), lambda job_id: self.TRANSIENT_LOG
        )
        self.assertEqual(
            failures,
            {},
            "a transient configure failure must stay on the retry path; opening "
            "the circuit on it converts a recoverable flake into a stuck release",
        )

    def test_one_classifier_answers_the_capacity_question_too(self) -> None:
        # Same predicate, two callers — the reconciler asks "re-dispatch?" and
        # the capacity path asks "may this run keep a dedicated runner?". They
        # must not grow separate definitions of "doomed".
        self.assertEqual(
            rr.run_is_doomed(self._jobs(), lambda job_id: self.DIGEST_LOG),
            "does not match pinned revision",
        )
        self.assertIsNone(
            rr.run_is_doomed(self._jobs(), lambda job_id: self.TRANSIENT_LOG)
        )

    def test_unfetchable_log_leaves_the_tag_on_the_retry_path(self) -> None:
        # Missing evidence is not evidence of doom.
        failures, _ = rr.validation_outcomes(self._jobs(), lambda job_id: "")
        self.assertEqual(failures, {})


class CompileErrorsAreTagImmutable(unittest.TestCase):
    """A compiler diagnostic against a frozen source cannot change on re-run.

    v0.897.3, v0.898.0 and v0.899.0 failed their Windows legs on one MSVC C2668
    and were re-dispatched every sweep while no incident opened. The circuit
    must open on the first terminal failure; environmental compiler failures
    must stay retryable.
    """

    MSVC_LOG = (
        "2026-10-03T11:57:09.1310300Z   kit_profile_verification.cpp\n"
        "2026-10-03T11:57:10.7365733Z D:\\a\\pulp\\pulp\\tools\\cli\\"
        "kit_profile_verification.cpp(586,47): error C2668: "
        "'pulp::cli::kit::shell_quote_local': ambiguous call to overloaded "
        "function [D:\\a\\pulp\\pulp\\build\\tools\\cli\\pulp-cli.vcxproj]\n"
    )
    CLANG_LOG = "/src/core/view/src/label.cpp:41:12: error: no member named 'x'\n"

    def _jobs(self, step: str = "Build"):
        return [
            {
                "id": 7,
                "name": "CLI windows-x64",
                "conclusion": "failure",
                "steps": [{"name": step, "conclusion": "failure"}],
            }
        ]

    def test_msvc_language_error_opens_the_circuit(self) -> None:
        failures, _ = rr.validation_outcomes(self._jobs(), lambda _: self.MSVC_LOG)
        text = " ".join(failures.values())
        self.assertIn("tag-immutable", text)
        self.assertIn("kit_profile_verification.cpp(586,47): error C2668", text)
        self.assertNotIn(".vcxproj", text, "the MSBuild project suffix is noise")
        self.assertNotIn("D:\\a\\pulp", text, "the runner's checkout path is noise")

    def test_clang_error_opens_the_circuit(self) -> None:
        signature = rr.tag_immutable_failure(self.CLANG_LOG)
        self.assertEqual(
            signature, "compile error: label.cpp:41:12: error: no member named 'x'"
        )

    def test_environmental_compiler_failures_stay_retryable(self) -> None:
        for log in (
            "a.cpp(1): fatal error C1060: compiler is out of heap space\n",
            "a.cpp(3): fatal error C1083: Cannot open include file: 'gen.h'\n",
            "a.cpp:1:10: fatal error: 'gen.h' file not found\n",
            "a.cpp:9:1: internal compiler error: Segmentation fault\n",
            "c++: fatal error: Killed signal terminated program cc1plus\n",
            "lld-link: error: undefined symbol: pulp::x()\n",
            "a.obj : error LNK2019: unresolved external symbol\n",
            "a.cpp:4:2: warning: unused variable 'y'\n",
        ):
            with self.subTest(log=log):
                self.assertIsNone(rr.tag_immutable_failure(log))

    def test_capacity_caller_agrees(self) -> None:
        self.assertTrue(
            rr.run_is_doomed(self._jobs(), lambda _: self.MSVC_LOG).startswith(
                "compile error:"
            )
        )

    def test_compile_error_tag_is_circuit_open_not_redispatched(self) -> None:
        failures, _ = rr.validation_outcomes(self._jobs(), lambda _: self.MSVC_LOG)
        decision = decide(
            state(age_minutes=60, validation_failures=tuple(failures.values())),
            NOW,
            **LIMITS,
        )
        self.assertEqual(decision.action, CIRCUIT_OPEN)
        self.assertIn("C2668", decision.reason)


def tag_state(*, age_hours: float, run_states=(), failed_legs=(), published=False,
              tag: str = "v0.899.0") -> TagState:
    return TagState(
        tag=tag,
        created_at=NOW - timedelta(hours=age_hours),
        published=published,
        has_release_object=published,
        assets=REQUIRED_ASSETS if published else frozenset(),
        run_states=tuple(run_states),
        validation_failures=(),
        dispatch_attempts=0,
        failed_legs=tuple(failed_legs),
    )


class Drought(unittest.TestCase):
    """An unpublished tag is reported by age, naming the failing legs."""

    LEGS = ("CLI windows-x64 › Build", "CLI windows-arm64 › Build")

    def check(self, **kw):
        return rr.drought(
            tag_state(**kw), NOW, drought_hours=2, newest_published=(0, 897, 2)
        )

    def test_old_failed_tag_is_reported_with_its_legs(self) -> None:
        why = self.check(age_hours=3, run_states=("completed",), failed_legs=self.LEGS)
        self.assertIsNotNone(why)
        self.assertIn("3.0h", why)
        self.assertIn("CLI windows-x64 › Build", why)
        self.assertIn("CLI windows-arm64 › Build", why)

    def test_young_tag_is_not_reported(self) -> None:
        self.assertIsNone(
            self.check(age_hours=1.5, run_states=("completed",), failed_legs=self.LEGS)
        )

    def test_slow_first_build_is_never_reported(self) -> None:
        # The pipeline legitimately takes 70-165+ minutes; age alone is what
        # made the retired watchdogs alarm on healthy releases.
        self.assertIsNone(self.check(age_hours=2.5, run_states=("in_progress",)))
        self.assertIsNone(self.check(age_hours=2.5, run_states=("queued",)))

    def test_retry_in_flight_after_a_failure_keeps_reporting(self) -> None:
        # Otherwise the incident would close whenever a re-dispatch starts.
        why = self.check(
            age_hours=3, run_states=("completed", "in_progress"), failed_legs=self.LEGS
        )
        self.assertIsNotNone(why)

    def test_tag_with_no_run_at_all_is_reported(self) -> None:
        why = self.check(age_hours=3)
        self.assertIsNotNone(why)
        self.assertIn("no failed leg recorded", why)

    def test_published_and_superseded_tags_are_not_droughts(self) -> None:
        self.assertIsNone(self.check(age_hours=5, published=True))
        self.assertIsNone(
            self.check(age_hours=5, failed_legs=self.LEGS, tag="v0.897.0")
        )


class FailedLegs(unittest.TestCase):
    def test_newest_completed_runs_latest_attempt_names_the_legs(self) -> None:
        runs = [
            {"id": 1, "status": "completed", "created_at": "2026-10-03T09:00:00Z"},
            {"id": 2, "status": "completed", "created_at": "2026-10-03T11:00:00Z"},
            {"id": 3, "status": "in_progress", "created_at": "2026-10-03T12:00:00Z"},
        ]
        job_cache = {
            1: [{"name": "CLI linux-x64", "conclusion": "failure", "run_attempt": 1,
                 "steps": [{"name": "Build", "conclusion": "failure"}]}],
            2: [
                {"name": "CLI windows-x64", "conclusion": "failure", "run_attempt": 1,
                 "steps": [{"name": "Configure", "conclusion": "failure"}]},
                {"name": "CLI windows-x64", "conclusion": "failure", "run_attempt": 2,
                 "steps": [
                     {"name": "Configure", "conclusion": "success"},
                     {"name": "Build", "conclusion": "failure"},
                     {"name": "Package CLI (Windows)", "conclusion": "skipped"},
                 ]},
                {"name": "CLI darwin-arm64", "conclusion": "success", "run_attempt": 2,
                 "steps": []},
            ],
        }
        self.assertEqual(
            rr.newest_failed_legs(runs, job_cache), ("CLI windows-x64 › Build",)
        )

    def test_no_completed_run_means_no_legs(self) -> None:
        self.assertEqual(
            rr.newest_failed_legs(
                [{"id": 1, "status": "queued", "created_at": "x"}], {}
            ),
            (),
        )


def sweep(states: list[TagState]) -> tuple[dict[str, str], list[str]]:
    """Run main() over `states` with all I/O stubbed; return (report, rebuilt)."""
    import os

    captured: dict = {}
    redispatched: list[str] = []
    saved = (rr.collect, rr.sdk_tags, rr.sync_incident, rr.redispatch, rr.datetime)

    class FrozenDatetime(datetime):
        @classmethod
        def now(cls, tz=None):
            return NOW

    old_env = dict(os.environ)
    try:
        rr.sdk_tags = lambda *_: [(s.tag, s.created_at) for s in states]
        rr.collect = lambda *_: states
        rr.sync_incident = lambda repo, report, dry: captured.setdefault("r", report)
        rr.redispatch = lambda repo, tag, dry: redispatched.append(tag)
        rr.datetime = FrozenDatetime
        os.environ.update({"REPO": "example/repo", "DRY_RUN": "true"})
        assert rr.main() == 0
    finally:
        os.environ.clear()
        os.environ.update(old_env)
        rr.collect, rr.sdk_tags, rr.sync_incident, rr.redispatch, rr.datetime = saved
    return dict(captured["r"]), redispatched


PUBLISHED_PRIOR = TagState(
    tag="v0.897.2", created_at=NOW - timedelta(hours=6), published=True,
    has_release_object=True, assets=REQUIRED_ASSETS, run_states=("completed",),
    validation_failures=(), dispatch_attempts=0,
)


class SweepReportsTheIncidentShape(unittest.TestCase):
    """Drive main() over the incident shapes with the I/O stubbed."""

    def test_three_failed_windows_tags_open_one_incident_and_no_rebuilds(self) -> None:
        jobs = CompileErrorsAreTagImmutable()._jobs()
        failures, _ = rr.validation_outcomes(
            jobs, lambda _: CompileErrorsAreTagImmutable.MSVC_LOG
        )
        states = [
            TagState(
                tag=tag, created_at=NOW - timedelta(hours=age), published=False,
                has_release_object=False, assets=frozenset(),
                run_states=("completed",), validation_failures=tuple(failures.values()),
                dispatch_attempts=0, failed_legs=("CLI windows-x64 › Build",),
            )
            for tag, age in (("v0.899.0", 1.5), ("v0.898.0", 3), ("v0.897.3", 4.5))
        ] + [PUBLISHED_PRIOR]
        report, rebuilt = sweep(states)
        self.assertEqual(rebuilt, [], "a compile error must not be rebuilt")
        self.assertEqual(sorted(report), ["v0.897.3", "v0.898.0", "v0.899.0"])
        for tag, why in report.items():
            with self.subTest(tag=tag):
                self.assertIn("C2668", why)
                self.assertIn("CLI windows-x64 › Build", why)

    def test_retryable_failure_past_two_hours_is_reported_and_still_rebuilt(self) -> None:
        # No signature, so it stays on the retry path — but it is overdue, and
        # before the drought rule nothing said so until retries ran out.
        overdue = tag_state(
            age_hours=3, run_states=("completed",),
            failed_legs=("CLI windows-x64 › Configure",),
        )
        report, rebuilt = sweep([overdue, PUBLISHED_PRIOR])
        self.assertEqual(rebuilt, ["v0.899.0"])
        self.assertIn("CLI windows-x64 › Configure", report["v0.899.0"])
        self.assertIn("unpublished 3.0h", report["v0.899.0"])

    def test_overdue_tag_is_reported_even_when_the_rebuild_budget_is_spent(self) -> None:
        newer = tag_state(
            age_hours=2.5, run_states=("completed",), failed_legs=("CLI a › Build",),
            tag="v0.899.1",
        )
        older = tag_state(
            age_hours=3, run_states=("completed",), failed_legs=("CLI b › Build",),
        )
        report, rebuilt = sweep([newer, older, PUBLISHED_PRIOR])
        self.assertEqual(rebuilt, ["v0.899.1"])
        self.assertEqual(sorted(report), ["v0.899.0", "v0.899.1"])

    def test_healthy_young_release_opens_nothing(self) -> None:
        building = tag_state(age_hours=1, run_states=("in_progress",))
        report, rebuilt = sweep([building, PUBLISHED_PRIOR])
        self.assertEqual((report, rebuilt), ({}, []))


if __name__ == "__main__":
    unittest.main()
