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

from release_reconcile import (  # noqa: E402
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
    runs_this_tag,
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

    def test_orphan_draft_is_redispatched_not_deleted(self) -> None:
        """A draft left behind by a half-finished finalizer must be re-driven.

        The old reaper DELETED such drafts. Re-dispatch is idempotent (release-cli's
        finalizer re-uploads assets onto the existing draft and publishes it), so
        recovery never has to destroy release state.
        """
        decision = decide(
            state(has_release_object=True, run_states=("completed",)), NOW, **LIMITS
        )
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
            dispatch_attempts=0,
        )
        decision = decide(old, NOW, asset_floor=self.FLOOR, **LIMITS)
        self.assertEqual(decision.action, OK)
        self.assertFalse(old.unresolved(newest_published=None, floor=self.FLOOR))

    def test_release_at_or_above_the_floor_must_be_complete(self) -> None:
        new = TagState(
            tag="v0.660.0",
            created_at=NOW - timedelta(hours=2),
            published=True,
            has_release_object=True,
            assets=REQUIRED_ASSETS - {"pulp-darwin-x64.tar.gz"},
            run_states=("completed",),
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


if __name__ == "__main__":
    unittest.main()
