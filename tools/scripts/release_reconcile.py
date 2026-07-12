#!/usr/bin/env python3
"""Drive every recent SDK tag to a published GitHub Release.

Used by .github/workflows/release-reconcile.yml. The decision logic lives in
`decide()`, which is pure — it takes a snapshot of the world and returns what
should happen — so the interesting cases are unit-testable without touching the
GitHub API. `main()` is the thin I/O shell around it.

The one invariant that matters: this module never cancels a run and never
deletes a release. Recovering a release by destroying release state is what
broke the pipeline in the first place. The only corrective action available here
is to re-dispatch release-cli.yml, which is idempotent — its finalizer no-ops on
an already-published tag.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Iterable, Sequence

SEMVER_TAG = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")

# Terminal states. A run in any other state is still doing something.
LIVE_RUN_STATES = {"queued", "in_progress", "requested", "waiting", "pending"}

INCIDENT_TITLE = "Release reconciler: tags stuck after repeated re-dispatch"
INCIDENT_LABEL = "release-guard"


# ── decisions ────────────────────────────────────────────────────────────────

OK = "ok"                    # published — nothing to do
IN_FLIGHT = "in-flight"      # a run is working on it; leave it alone
GRACE = "grace"              # too young to judge
TOO_OLD = "too-old"          # outside the reconciliation window
SUPERSEDED = "superseded"    # a newer version already shipped; not worth a rebuild
DEFERRED = "deferred"        # a NEWER tag is still building; wait and see
REDISPATCH = "redispatch"    # stuck, and we still have budget to retry
ESCALATE = "escalate"        # stuck, and out of budget — a human must look


INCOMPLETE = "incomplete"    # published, but missing assets — unfixable by us

# What every Pulp release must carry. Mirrors `required_assets` in
# release-cli.yml's finalizer; SHA256SUMS is added by that step.
REQUIRED_ASSETS = frozenset(
    {
        "appcast.xml",
        "SHA256SUMS",
        "pulp-darwin-arm64.tar.gz",
        "pulp-darwin-x64.tar.gz",
        "pulp-linux-arm64.tar.gz",
        "pulp-linux-x64.tar.gz",
        "pulp-windows-arm64.zip",
        "pulp-windows-x64.zip",
        "pulp-sdk-darwin-arm64.tar.gz",
        "pulp-sdk-darwin-x64.tar.gz",
        "pulp-sdk-linux-arm64.tar.gz",
        "pulp-sdk-linux-x64.tar.gz",
        "pulp-sdk-windows-arm64.tar.gz",
        "pulp-sdk-windows-x64.tar.gz",
    }
)


@dataclass(frozen=True)
class TagState:
    """Everything we know about one tag."""

    tag: str
    created_at: datetime
    published: bool          # a non-draft release exists
    has_release_object: bool  # a release exists at all (draft or published)
    assets: frozenset[str]   # asset names on that release
    run_states: tuple[str, ...]  # states of every release-cli run for this tag
    dispatch_attempts: int   # how many times we have already re-dispatched

    @property
    def missing_assets(self) -> frozenset[str]:
        return REQUIRED_ASSETS - self.assets

    def unresolved(self, *, newest_published: tuple | None, floor: tuple | None) -> bool:
        """Is this tag a standing incident?

        Deliberately independent of whether a run happens to be live right now: an
        escalated tag that a human is mid-way through repairing is still
        unresolved, and closing its incident the moment a retry starts would make
        the incident flap.

        A superseded tag is NOT an incident — users install the newer release.
        """
        if superseded(self.tag, newest_published):
            return False
        if not self.published:
            return True
        return bool(self.missing_assets) and at_or_above_floor(self.tag, floor)


@dataclass(frozen=True)
class Decision:
    action: str
    reason: str


def version_of(tag: str) -> tuple | None:
    m = SEMVER_TAG.match(tag)
    return tuple(int(g) for g in m.groups()) if m else None


def superseded(tag: str, newest_published: tuple | None) -> bool:
    """Has a strictly newer version already published?

    NOTE the difference from the "supersede reaper" this whole change exists to
    delete. The reaper CANCELLED in-flight runs and DELETED drafts for older tags —
    destructive, and racing with releases that were merely slow. This only declines
    to START a speculative rebuild of a tag whose users are already served by a
    newer release. It never touches release state, and a live run always outranks
    it. Rebuilding a superseded tag costs a ~2-hour matrix and starves the runners
    that the CURRENT release needs.
    """
    v = version_of(tag)
    return bool(v and newest_published and v < newest_published)


def at_or_above_floor(tag: str, floor: tuple | None) -> bool:
    """Is this tag subject to the current asset contract?

    Releases from before the contract legitimately lack assets that did not exist
    yet — the Intel `darwin-x64` pair, `SHA256SUMS`. Holding them to today's
    contract would flag a pile of perfectly good historical releases, which is the
    false-alarm behaviour this reconciler replaced.
    """
    v = version_of(tag)
    return bool(v and floor and v >= floor)


def decide(
    state: TagState,
    now: datetime,
    *,
    grace_minutes: int,
    max_age_hours: int,
    max_attempts: int,
    newest_published: tuple | None = None,
    newest_live: tuple | None = None,
    asset_floor: tuple | None = None,
) -> Decision:
    """Decide what to do about one tag. Pure."""
    age = now - state.created_at

    if state.published:
        # "Published implies complete" is an invariant of release-cli's finalizer
        # (it publishes only after an --exact-required asset check) — but VERIFY it
        # rather than assume it. A release published by another path (a human, a
        # legacy run) can be incomplete, and a published GitHub release is
        # IMMUTABLE: we cannot add the missing assets. A rebuild would not help, so
        # this goes straight to a human.
        missing = state.missing_assets
        if missing and at_or_above_floor(state.tag, asset_floor):
            return Decision(
                INCOMPLETE,
                f"published but MISSING {sorted(missing)} — a published release "
                f"is immutable, so this needs a new patch tag",
            )
        return Decision(OK, f"{state.tag} is published")

    if age > timedelta(hours=max_age_hours):
        return Decision(TOO_OLD, f"{state.tag} is older than {max_age_hours}h")

    # A live run outranks every other signal, at ANY age. A release that has been
    # building for four hours is slow, not stuck — and the whole reason releases
    # were disappearing is that automation kept deciding otherwise. Re-dispatching
    # here would also stampede.
    if any(s in LIVE_RUN_STATES for s in state.run_states):
        return Decision(IN_FLIGHT, f"{state.tag} has a release-cli run in flight")

    # No live run and no release yet — but the tag may simply have been pushed
    # seconds ago and the run not registered.
    if age < timedelta(minutes=grace_minutes) and not state.has_release_object:
        return Decision(GRACE, f"{state.tag} is younger than {grace_minutes}m")

    if superseded(state.tag, newest_published):
        return Decision(
            SUPERSEDED,
            f"{state.tag} never published, but a newer release already shipped — "
            f"not spending a rebuild on it",
        )

    # A NEWER tag is still building. Wait for it.
    #
    # If it publishes, this tag becomes SUPERSEDED and never needed a rebuild at
    # all — so repairing it now is speculative work that competes for the very
    # runners the newer release is waiting on. That is self-defeating on a starved
    # pool: we would be slowing down the release that makes this one unnecessary.
    #
    # This is a WAIT, not a write-off. Nothing is cancelled or deleted, and if the
    # newer tag's run ends without publishing, it stops being live and this tag
    # becomes repairable on the very next sweep.
    if version_of(state.tag) and newest_live and version_of(state.tag) < newest_live:
        return Decision(
            DEFERRED,
            f"{state.tag} is stuck, but a NEWER tag is still building — waiting, "
            f"since publishing that one supersedes this one",
        )

    if state.dispatch_attempts >= max_attempts:
        return Decision(
            ESCALATE,
            f"still unpublished after {state.dispatch_attempts} re-dispatches — "
            f"this is a real failure, not a flake",
        )

    detail = (
        "a draft was left behind (the finalizer did not complete)"
        if state.has_release_object
        else "no release was created"
    )
    return Decision(REDISPATCH, f"{state.tag}: {detail}")


# ── github i/o ───────────────────────────────────────────────────────────────


def gh_json(args: Sequence[str]) -> object:
    out = subprocess.run(
        ["gh", *args], capture_output=True, text=True, check=True
    ).stdout
    return json.loads(out) if out.strip() else []


def parse_ts(value: str) -> datetime:
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


def sdk_tags(max_age_hours: int, now: datetime) -> list[tuple[str, datetime]]:
    """Recent `vX.Y.Z` tags, newest first. Excludes `plugin-v*`."""
    out = subprocess.run(
        [
            "git", "for-each-ref", "--sort=-creatordate",
            "--format=%(refname:short) %(creatordate:iso-strict)",
            "refs/tags/v*",
        ],
        capture_output=True, text=True, check=True,
    ).stdout
    tags: list[tuple[str, datetime]] = []
    for line in out.splitlines():
        name, _, created = line.partition(" ")
        if not SEMVER_TAG.match(name):
            continue
        created_at = parse_ts(created.strip())
        if now - created_at > timedelta(hours=max_age_hours):
            break  # sorted newest-first, so everything after is older too
        tags.append((name, created_at))
    return tags


def collect(
    repo: str, tags: Iterable[tuple[str, datetime]], since: datetime
) -> list[TagState]:
    # `gh api --paginate` emits one JSON document PER PAGE, so the concatenation is
    # not valid JSON. `--slurp` wraps the pages in an array. Omitting it is the bug
    # that made release-health.yml silently treat EVERY tag as unreleased once the
    # repo passed 100 releases (its json.loads threw, and it fell back to `[]`).
    releases = gh_json(
        ["api", "--paginate", "--slurp", f"repos/{repo}/releases?per_page=100"]
    )
    flat = [rel for page in releases for rel in page]
    by_tag = {rel["tag_name"]: rel for rel in flat}

    # Bound the run query to the reconciliation window. Unbounded `--paginate` here
    # walks EVERY release-cli run ever recorded — hundreds of pages, every 30
    # minutes, for the handful of recent runs we actually care about.
    created = since.strftime("%Y-%m-%d")
    runs = gh_json(
        [
            "api", "--paginate", "--slurp",
            f"repos/{repo}/actions/workflows/release-cli.yml/runs"
            f"?per_page=100&created=%3E%3D{created}",
        ]
    )
    flat_runs = [r for page in runs for r in page["workflow_runs"]]

    states: list[TagState] = []
    for tag, created_at in tags:
        tag_runs = [r for r in flat_runs if runs_this_tag(r, tag)]
        rel = by_tag.get(tag)
        states.append(
            TagState(
                tag=tag,
                created_at=created_at,
                published=bool(rel) and not rel["draft"],
                has_release_object=rel is not None,
                assets=frozenset(
                    a["name"] for a in (rel or {}).get("assets", [])
                ),
                run_states=tuple(r["status"] for r in tag_runs),
                # A tag push produces one `push` run; every run beyond that is a
                # re-dispatch, ours or a human's.
                dispatch_attempts=sum(
                    1 for r in tag_runs if r["event"] == "workflow_dispatch"
                ),
            )
        )
    return states


def runs_this_tag(run: dict, tag: str) -> bool:
    """Does this workflow run belong to `tag`?

    `head_branch` is NOT sufficient. For a tag push it is the tag, but for a
    workflow_dispatch it is the ref the run was dispatched FROM — `main` — because
    we deliberately dispatch main's (fixed) workflow to build an old tag's source.
    Matching on head_branch alone therefore makes every REPAIR run invisible to
    the reconciler, which would then see zero attempts, never escalate, and fire a
    fresh re-dispatch every 30 minutes forever.

    release-cli.yml sets `run-name: Release <tag>` for both trigger types, which
    surfaces as `display_title`. That is the reliable link. head_branch is kept as
    a fallback so runs predating that run-name still match.
    """
    return run.get("display_title") == f"Release {tag}" or run.get("head_branch") == tag


def redispatch(repo: str, tag: str, dry_run: bool) -> None:
    """Rebuild and republish `tag` from the CURRENT workflow on main.

    `--ref main` runs main's (fixed) workflow file; `version=<tag>` makes it
    check out and build the TAG's source. `make_latest=true` only *permits* the
    latest pointer — release-cli's finalizer still refuses to move
    /releases/latest backward, so this is safe for an out-of-order backfill.
    """
    cmd = [
        "gh", "workflow", "run", "release-cli.yml",
        "--repo", repo, "--ref", "main",
        "-f", f"version={tag}",
        "-f", "make_latest=true",
    ]
    if dry_run:
        print(f"    DRY RUN: would run: {' '.join(cmd)}")
        return
    subprocess.run(cmd, check=True)
    print(f"    re-dispatched release-cli for {tag}")


def sync_incident(repo: str, report: list[tuple[str, str]], dry_run: bool) -> None:
    """Maintain exactly ONE incident issue across its whole lifecycle.

    Deliberately not one-issue-per-tag-per-run: that pattern produced 413 issues in
    two weeks. Three things make "one issue" actually hold, all of which the old
    release-health.yml got wrong:

      - The title is CONSTANT — no counts, no tag names. release-health embedded an
        escalating count, so its issues were not even title-identical to each other.
      - The label EXISTS. release-health labelled with `release-health`, which is
        not a label in this repo; the labelled create failed, an unlabelled create
        fell through, and its own dedupe (filtered by that label) could never find
        what it had filed.
      - The lookup searches `--state all` and REOPENS. Searching only open issues
        means every recurrence after a recovery mints a fresh issue, which is just
        the firehose with extra steps.
    """
    found = gh_json(
        [
            "issue", "list", "--repo", repo, "--state", "all", "--limit", "50",
            "--label", INCIDENT_LABEL, "--search", f"in:title {INCIDENT_TITLE}",
            "--json", "number,title,state",
        ]
    )
    match = next((i for i in found if i["title"] == INCIDENT_TITLE), None)

    if not report:
        if match and match["state"].lower() == "open" and not dry_run:
            subprocess.run(
                ["gh", "issue", "close", str(match["number"]), "--repo", repo,
                 "--comment", "Every recent tag is published and complete. Closing."],
                check=True,
            )
            print(f"  closed incident #{match['number']} — all clear")
        return

    body = (
        "Tags the release reconciler could not get published on its own. It "
        "re-dispatches automatically, so anything listed here has already "
        "exhausted its retry budget or cannot be fixed by a rebuild — this is a "
        "real failure, not a flake.\n\n"
        + "\n".join(f"- `{tag}` — {why}" for tag, why in report)
        + "\n\nUsually the build is broken at that tag: open the failing "
        "`release-cli.yml` run, land the fix on `main`, and the reconciler will "
        "re-dispatch within 30 minutes. A release that is published but missing "
        "assets cannot be repaired in place (published releases are immutable) — "
        "ship a new patch tag.\n\nMaintained in place by "
        "`.github/workflows/release-reconcile.yml`; it closes itself once every "
        "recent tag is published and complete."
    )
    if dry_run:
        print(f"  DRY RUN: would report {len(report)} unresolved tag(s)")
        return
    if match:
        subprocess.run(
            ["gh", "issue", "edit", str(match["number"]), "--repo", repo,
             "--body", body],
            check=True,
        )
        if match["state"].lower() != "open":
            subprocess.run(
                ["gh", "issue", "reopen", str(match["number"]), "--repo", repo],
                check=True,
            )
            print(f"  reopened incident #{match['number']}")
        print(f"  updated incident #{match['number']} ({len(report)} unresolved)")
    else:
        subprocess.run(
            ["gh", "issue", "create", "--repo", repo, "--title", INCIDENT_TITLE,
             "--body", body, "--label", INCIDENT_LABEL],
            check=True,
        )
        print(f"  opened incident ({len(report)} unresolved)")


def main() -> int:
    repo = os.environ["REPO"]
    dry_run = os.environ.get("DRY_RUN", "false") == "true"
    grace = int(os.environ.get("GRACE_MINUTES", "45"))
    max_age = int(os.environ.get("MAX_AGE_HOURS", "72"))
    max_attempts = int(os.environ.get("MAX_ATTEMPTS", "3"))
    now = datetime.now(timezone.utc)

    # Look slightly further back for RUNS than for tags: a tag at the edge of the
    # window may have been built by a run that started before it.
    since = now - timedelta(hours=max_age + 24)
    states = collect(repo, sdk_tags(max_age, now), since)
    if not states:
        print("No SDK tags in the reconciliation window.")
        return 0

    newest_published = max(
        (version_of(s.tag) for s in states if s.published and version_of(s.tag)),
        default=None,
    )
    # The newest tag that currently has a release-cli run in flight. Anything older
    # than this waits: if that run publishes, the older tags are superseded and
    # never needed a rebuild.
    newest_live = max(
        (
            version_of(s.tag)
            for s in states
            if version_of(s.tag)
            and any(r in LIVE_RUN_STATES for r in s.run_states)
        ),
        default=None,
    )
    # Only tags at or above this version are held to the CURRENT asset contract.
    # Older releases legitimately predate the Intel pair and SHA256SUMS.
    floor_raw = os.environ.get("ASSET_CONTRACT_FLOOR", "").strip()
    asset_floor = version_of(floor_raw) if floor_raw else None

    # Repair NEWEST-FIRST, and only so many per sweep.
    #
    # `states` is newest-first, so the tag users actually want is fixed first. The
    # cap then lets supersession settle: once the newest tag publishes, the older
    # unpublished ones become SUPERSEDED on the next sweep and are skipped
    # entirely, instead of every one of them kicking off its own ~2-hour matrix at
    # the same moment. That matters most in exactly the situation this reconciler
    # is introduced into — a backlog of stuck tags and a starved runner pool, where
    # a five-way stampede would starve the release it is trying to repair.
    budget = int(os.environ.get("MAX_REDISPATCH_PER_SWEEP", "1"))

    report: list[tuple[str, str]] = []
    for state in states:
        decision = decide(
            state, now,
            grace_minutes=grace, max_age_hours=max_age, max_attempts=max_attempts,
            newest_published=newest_published, newest_live=newest_live,
            asset_floor=asset_floor,
        )
        print(f"  [{decision.action:11}] {decision.reason}")
        if decision.action == REDISPATCH:
            if budget <= 0:
                print("    (deferred — re-dispatch budget for this sweep is spent)")
                continue
            budget -= 1
            redispatch(repo, state.tag, dry_run)
        elif decision.action in (ESCALATE, INCOMPLETE):
            report.append((state.tag, decision.reason))
        elif (
            state.unresolved(newest_published=newest_published, floor=asset_floor)
            and state.dispatch_attempts >= max_attempts
        ):
            # Already escalated on a previous sweep and STILL not published — a
            # human is presumably mid-repair, so it has a live run and is not
            # ESCALATE this time round. It stays in the incident until it actually
            # publishes; otherwise the incident would close the moment a retry
            # started and reopen the moment it failed, flapping on every sweep.
            report.append((state.tag, "still unpublished; repair in progress"))

    sync_incident(repo, report, dry_run)
    # Reporting a problem is not itself a failure — exiting non-zero here would
    # just mint another red X for the watchdogs to shout about.
    return 0


if __name__ == "__main__":
    sys.exit(main())
