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
from typing import Callable, Iterable, Sequence

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from release_failure_diagnosis import (  # noqa: E402
    Diagnosis,
    diagnose,
    first_error_line,
    release_asset_coordinates,
)

SEMVER_TAG = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")
# Pulls the tag out of a job name like "Resolve macOS runner — v0.659.0".
SEMVER_IN_TEXT = re.compile(r"v\d+\.\d+\.\d+")

# A run in any of these states has not finished.
LIVE_RUN_STATES = {"queued", "in_progress", "requested", "waiting", "pending"}
# ...but only THIS one means a runner actually picked the job up. The distinction
# is load-bearing: a run that is merely QUEUED has no runner, and if no runner will
# ever serve it (a bad `runs-on`, a label nothing matches, an offline host pool) it
# stays queued forever. "Slow" and "never started" look identical in
# LIVE_RUN_STATES, and conflating them is how a release can hang indefinitely with
# nothing ever escalating.
WORKING_RUN_STATES = {"in_progress"}

# The long-lived incident used before stall episodes. An open one is still
# updated and closed like an episode, so the switch loses nothing in flight.
INCIDENT_TITLE = "Release reconciler: tags stuck after repeated re-dispatch"
# One issue per stall episode: opened when a tag first becomes unresolved,
# updated while anything stays unresolved, closed on recovery, never reopened.
EPISODE_TITLE_PREFIX = "Release stalled: "
INCIDENT_LABEL = "release-guard"


# ── decisions ────────────────────────────────────────────────────────────────

OK = "ok"                    # published — nothing to do
IN_FLIGHT = "in-flight"      # a run is working on it; leave it alone
GRACE = "grace"              # too young to judge
TOO_OLD = "too-old"          # outside the reconciliation window
SUPERSEDED = "superseded"    # a newer version already shipped; not worth a rebuild
STUCK_QUEUE = "stuck-queue"  # queued for hours and never started — no runner is coming
DEFERRED = "deferred"        # a NEWER tag is still building; wait and see
REDISPATCH = "redispatch"    # stuck, and we still have budget to retry
ESCALATE = "escalate"        # stuck, and out of budget — a human must look
CIRCUIT_OPEN = "circuit-open"  # product validation failed; do not retry the tag

# These steps validate immutable release content. Failures in bootstrap, artifact
# transfer, attestation, or publication are workflow failures and retain the
# bounded retry path; a failure here is evidence that repeating the same tag would
# only repeat runner load.
VALIDATION_STEP_PREFIXES = (
    "Smoke CLI",
    "Verify release archive product matrix",
)


# Failures that are a property of the IMMUTABLE TAG rather than of the attempt.
# The step-name allowlist above cannot express these: they surface from steps
# that also fail transiently, so the step name alone is not evidence.
#
# `Configure` is the motivating case and the reason this is signature-based
# rather than a sixth prefix. v0.831.0 failed at step 17 `Configure` on a
# pinned-runtime digest mismatch and was re-dispatched every sweep, each attempt
# claiming a dedicated release VM it could never use. But `Configure` also fails
# for fetch timeouts and cache misses, and opening the circuit on THOSE would
# convert a recoverable flake into a permanently stuck release — strictly worse
# than the bug being fixed. So match the failure, not the step.
#
# A pinned-revision digest mismatch is a fact about the tag's tree: the pin and
# the bytes it names are both frozen at the tag, so every retry recomputes the
# same digest and fails identically. Re-running provably cannot change it.
TAG_IMMUTABLE_FAILURE_SIGNATURES = (
    "does not match pinned revision",
)

# A compiler diagnostic against a source file is the same kind of fact: the
# sources are frozen at the tag, so every re-run compiles the same bytes and
# rejects them the same way. v0.897.3, v0.898.0 and v0.899.0 each failed their
# Windows legs on one MSVC C2668 in a CLI source and were re-dispatched on every
# sweep — three full release matrices apiece, ~50 minutes each, for an outcome no
# re-run could change.
#
# Matched narrowly, so an environmental failure keeps the retry path:
#   * MSVC `file(line[,col]): error C2xxx-C9xxx:` — the language errors. C1xxx
#     are the fatal ones (C1060 out of heap space, C1083 cannot open include
#     file), which a resource limit or an incomplete fetch can cause.
#   * Clang/GCC `file:line:col: error:` — a plain `error:` only. `fatal error:`
#     (missing include) and `internal compiler error:` do not match.
# Linker errors are deliberately absent: an undefined symbol can come from an
# interrupted dependency build as easily as from the tree.
_SOURCE = r"[^\\/\s:()]+\.(?:c|cc|cpp|cxx|h|hh|hpp|hxx|inl|ipp|m|mm)"
TAG_IMMUTABLE_COMPILE_ERRORS = (
    re.compile(_SOURCE + r"\(\d+(?:,\d+)?\): error C[2-9]\d{3}: [^\r\n]*"),
    re.compile(_SOURCE + r":\d+:\d+: error: [^\r\n]*"),
)
# MSBuild appends the project file to every diagnostic; it says nothing a reader
# needs and pushes the useful part out of a one-line summary.
_MSBUILD_PROJECT_SUFFIX = re.compile(r"\s*\[[^\]]*\.vcxproj\]\s*$")
_SIGNATURE_LIMIT = 200


def tag_immutable_failure(log_text: str) -> str | None:
    """Return the signature proving a failure is a property of the tag, if any.

    One classifier, two callers: the reconciler asks "should I re-dispatch?" and
    the capacity path asks "may this run keep a dedicated runner?". Both mean the
    same thing — can a re-run change the outcome — so they must not grow separate
    definitions of "doomed" that can later disagree.

    A compile error is returned as `compile error: <first diagnostic>`, so the
    incident names the file and the error without anyone opening the log.
    """
    for signature in TAG_IMMUTABLE_FAILURE_SIGNATURES:
        if signature in log_text:
            return signature
    for pattern in TAG_IMMUTABLE_COMPILE_ERRORS:
        match = pattern.search(log_text)
        if match:
            diagnostic = _MSBUILD_PROJECT_SUFFIX.sub("", match.group(0)).strip()
            return f"compile error: {diagnostic[:_SIGNATURE_LIMIT]}"
    return None


def run_is_doomed(
    jobs: Iterable[dict],
    log_fetcher: "Callable[[int], str] | None" = None,
) -> str | None:
    """Return a signature if no re-run of these jobs could succeed.

    The capacity-side caller. A run whose failure a re-run COULD fix keeps its
    claim on a dedicated runner; only a tag-immutable failure releases it. This
    deliberately does not treat "some leg failed" as doomed — a failed leg can be
    re-run, and cancelling to reclaim a VM would trade a recoverable release for
    a runner slot.
    """
    if log_fetcher is None:
        return None
    for job in jobs:
        if job.get("conclusion") != "failure":
            continue
        signature = tag_immutable_failure(log_fetcher(job.get("id")))
        if signature:
            return signature
    return None


INCOMPLETE = "incomplete"    # published, but missing assets — unfixable by us


def _required_assets() -> frozenset[str]:
    """What every Pulp release must carry.

    Derived from release_product_matrix.json's active_platforms — the same
    field release-cli.yml's finalizer derives its `--exact-required` contract
    from — so pausing or reactivating a platform cannot desync the reconciler
    from the publisher. Platform-independent assets (appcast, SHA256SUMS) are
    always demanded; SHA256SUMS is added by the finalizer itself.

    A release published while MORE platforms were active than today carries
    extra assets, which is never flagged (only missing assets are). A release
    published while FEWER were active can briefly flag INCOMPLETE after the
    set grows back — until the next release publishes and supersedes it, which
    at the current tag cadence is hours, not days.
    """
    from release_artifact_contents import DEFAULT_MATRIX, release_asset_names

    return frozenset({"appcast.xml", "SHA256SUMS", *release_asset_names(DEFAULT_MATRIX)})


REQUIRED_ASSETS = _required_assets()


@dataclass(frozen=True)
class TagState:
    """Everything we know about one tag."""

    tag: str
    created_at: datetime
    published: bool          # a non-draft release exists
    has_release_object: bool  # a release exists at all (draft or published)
    assets: frozenset[str]   # asset names on that release
    run_states: tuple[str, ...]  # states of every release-cli run for this tag
    validation_failures: tuple[str, ...]  # failed product-validation step names
    dispatch_attempts: int   # how many times we have already re-dispatched
    # "<job> › <first failed step>" for each failed job of the newest completed
    # run, so an incident names the leg without anyone opening the run.
    failed_legs: tuple[str, ...] = ()
    # The root-cause failure of the tag's release, explained: first error line,
    # cause class, exact fix. Taken from the newest run that failed for a reason
    # OTHER than a mis-targeted re-dispatch, so a broken repair attempt cannot
    # mask the failure it was meant to repair.
    diagnoses: tuple[Diagnosis, ...] = ()
    # Failed jobs of the tag's newest `Sign and Release` run, if it failed.
    sign_failures: tuple[Diagnosis, ...] = ()

    @property
    def circuit_open(self) -> bool:
        return bool(self.validation_failures)

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
    false-alarm behavior this reconciler replaced.
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
    stuck_queue_hours: int = 4,
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

    # A run that is actually RUNNING outranks every other signal, at ANY age. A
    # release that has been building for four hours is slow, not stuck — and the
    # whole reason releases were disappearing is that automation kept deciding
    # otherwise. Re-dispatching here would also stampede.
    if any(s in WORKING_RUN_STATES for s in state.run_states):
        return Decision(IN_FLIGHT, f"{state.tag} has a release-cli run in flight")

    # QUEUED IS NOT THE SAME AS RUNNING, and treating them alike is a blind spot.
    # A queued job has no runner. If nothing will ever serve it — a `runs-on` no
    # runner matches, a label typo, an offline host pool, or a workflow rename that
    # hides the run from a self-hosted supervisor's queue filter — it stays queued
    # FOREVER while looking, to a naive check, exactly like a slow build. That is
    # precisely how the release lane hung for a day: runs sat queued, every watchdog
    # saw "in flight", and nothing ever escalated.
    #
    # So: still queued, hours after the tag was cut, means no runner is coming.
    # Escalate to a human — re-dispatching would just mint another queued run on the
    # same broken route.
    queued = [s for s in state.run_states if s in LIVE_RUN_STATES]
    if queued and age <= timedelta(hours=stuck_queue_hours):
        return Decision(IN_FLIGHT, f"{state.tag} has a release-cli run in flight")

    # A release already replaced by a newer published version needs neither an
    # incident nor more runner time. A genuinely running job still outranks this
    # branch, but a stale queue entry or terminal result does not.
    if superseded(state.tag, newest_published):
        return Decision(
            SUPERSEDED,
            f"{state.tag} never published, but a newer release already shipped — "
            f"not spending a rebuild on it",
        )

    if queued:
        return Decision(
            STUCK_QUEUE,
            f"queued for over {stuck_queue_hours}h and never started — no runner is "
            f"serving this job (check `runs-on` labels and the runner pool)",
        )

    # A NEWER tag is still building. Wait for it. If it publishes, this tag is
    # superseded and never needed an incident or rebuild; if it fails, this tag is
    # evaluated normally on the next sweep.
    if version_of(state.tag) and newest_live and version_of(state.tag) < newest_live:
        return Decision(
            DEFERRED,
            f"{state.tag} is stuck, but a NEWER tag is still building — waiting, "
            f"since publishing that one supersedes this one",
        )

    # The source selected by release-cli is an immutable tag. A failed product
    # validation step is evidence that dispatching the same tag again would only
    # repeat runner load. Workflow/infrastructure failures do not set this flag
    # and retain the bounded retry path.
    if state.circuit_open:
        observed = "; ".join(sorted(set(state.validation_failures)))
        return Decision(
            CIRCUIT_OPEN,
            f"{state.tag} failed immutable product validation: {observed}; circuit "
            "open — immutable-tag failures are not retried",
        )

    # No live run and no release yet — but the tag may simply have been pushed
    # seconds ago and the run not registered.
    if age < timedelta(minutes=grace_minutes) and not state.has_release_object:
        return Decision(GRACE, f"{state.tag} is younger than {grace_minutes}m")

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


def drought(
    state: TagState,
    now: datetime,
    *,
    drought_hours: float,
    newest_published: tuple | None,
) -> str | None:
    """Return why an unpublished tag is a standing incident by age alone, or None.

    The other report paths fire only once retries are exhausted or a validation
    circuit opens. Between those, a tag whose release keeps failing is silently
    re-dispatched — three tags sat unpublished for hours that way with no issue
    open. So a tag still unpublished `drought_hours` after it was cut is
    reported too, naming the legs that failed.

    It does NOT fire on a first build that is merely slow: the pipeline takes
    70-165+ minutes, and an age threshold alone is exactly what made the
    retired watchdogs alarm on healthy releases. A live run with no failed leg
    behind it is left alone; a live run that is a re-dispatch of a failed one
    is not, so the incident does not flap while a retry is in flight.
    """
    if state.published or superseded(state.tag, newest_published):
        return None
    age = now - state.created_at
    if age < timedelta(hours=drought_hours):
        return None
    live = any(s in LIVE_RUN_STATES for s in state.run_states)
    if live and not state.failed_legs:
        return None
    hours = age.total_seconds() / 3600
    return f"unpublished {hours:.1f}h after tagging; {describe_legs(state)}"


def describe_legs(state: TagState) -> str:
    if state.failed_legs:
        return "failing leg(s): " + "; ".join(state.failed_legs)
    return "no failed leg recorded (no release run completed)"


def failed_legs(jobs: Iterable[dict]) -> tuple[str, ...]:
    """Name each failed job and the first step it failed in."""
    legs: list[str] = []
    for job in jobs:
        if job.get("conclusion") != "failure":
            continue
        step = next(
            (
                st.get("name", "")
                for st in job.get("steps", [])
                if st.get("conclusion") == "failure"
            ),
            "",
        )
        name = job.get("name", "unknown job")
        legs.append(f"{name} › {step}" if step else name)
    return tuple(legs)


# A re-dispatch from the wrong ref fails this guard before building anything.
# Its failure says nothing about the release; reporting it as the failing leg
# hid a pinned-asset digest mismatch behind five identical guard failures.
DISPATCH_GUARD_STEP = "Dispatch must run on the tag it publishes"


def _first_failed_step(job: dict) -> str:
    return next(
        (st.get("name", "") for st in job.get("steps", [])
         if st.get("conclusion") == "failure"),
        "",
    )


def root_cause_run(
    runs: Iterable[dict], job_cache: dict[int, list[dict]]
) -> tuple[dict | None, list[dict]]:
    """The newest completed run's failed jobs (latest attempt), preferring one
    that did not fail only on the dispatch-ref guard."""
    completed = sorted(
        (r for r in runs if r.get("status") == "completed"),
        key=lambda r: r.get("created_at", ""),
        reverse=True,
    )
    fallback: tuple[dict | None, list[dict]] = (None, [])
    for run in completed:
        jobs = job_cache.get(run["id"], [])
        if not jobs:
            continue
        latest = max(int(j.get("run_attempt", 1)) for j in jobs)
        current = [j for j in jobs if int(j.get("run_attempt", 1)) == latest]
        failed = [j for j in current if j.get("conclusion") == "failure"]
        if failed and all(_first_failed_step(j) == DISPATCH_GUARD_STEP for j in failed):
            if fallback[0] is None:
                fallback = (run, current)
            continue
        return run, current
    return fallback


def newest_failed_legs(
    runs: Iterable[dict], job_cache: dict[int, list[dict]]
) -> tuple[str, ...]:
    """Failed legs of the root-cause run (see `root_cause_run`)."""
    _, jobs = root_cause_run(runs, job_cache)
    return failed_legs(jobs)


def diagnose_run(
    workflow: str,
    tag: str,
    run: dict | None,
    jobs: Iterable[dict],
    log_fetcher: Callable[[int | None], str],
    *,
    manifest_text: str = "",
    upstream_digest: Callable[[str], str | None] = lambda _url: None,
    limit: int = 3,
) -> tuple[Diagnosis, ...]:
    """Explain each failed job of `run` (at most `limit`)."""
    if run is None:
        return ()
    out: list[Diagnosis] = []
    for job in jobs:
        if job.get("conclusion") != "failure":
            continue
        if len(out) >= limit:
            break
        log = log_fetcher(job.get("id"))
        url_match = re.search(
            r"url:\s*(https://github\.com/\S+/releases/download/\S+)", log
        )
        live = upstream_digest(url_match.group(1)) if url_match else None
        out.append(
            diagnose(
                workflow=workflow, tag=tag,
                run_url=job.get("html_url") or run.get("html_url", ""),
                job=job.get("name", "unknown job"), step=_first_failed_step(job),
                log_text=log, manifest_text=manifest_text, upstream_digest=live,
            )
        )
    return tuple(out)


def render_diagnoses(state: "TagState") -> str:
    """The actionable detail appended under a tag's incident entry."""
    lines: list[str] = []
    for diag in (*state.diagnoses, *state.sign_failures):
        lines.extend(diag.render())
    return ("\n" + "\n".join(lines)) if lines else ""


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

    # Job details serve two purposes: dispatch runs carry their tag in a job name,
    # and failed runs carry the step-level evidence that distinguishes immutable
    # product validation from retryable workflow/infrastructure failure.
    job_cache: dict[int, list[dict]] = {}
    # Logs are fetched only for jobs that already FAILED, and memoised per job,
    # so a healthy sweep costs no extra API calls. A log that cannot be fetched
    # yields "" — i.e. "no signature" — which leaves the tag on the retry path
    # rather than opening the circuit on missing evidence.
    log_cache: dict[int, str] = {}

    def log_fetcher(job_id: int | None) -> str:
        if job_id is None:
            return ""
        if job_id not in log_cache:
            text, error = fetch_job_log(repo, job_id)
            if error:
                print(f"  job {job_id}: log not fetched — {error}")
            log_cache[job_id] = text
        return log_cache[job_id]
    for r in flat_runs:
        if not needs_job_details(r):
            continue
        try:
            result = gh_json(
                [
                    "api",
                    f"repos/{repo}/actions/runs/{r['id']}/jobs?filter=all&per_page=100",
                ]
            )
            job_cache[r["id"]] = (result or {}).get("jobs", [])
        except subprocess.CalledProcessError:
            job_cache[r["id"]] = []

    # For a dispatch run, head_branch is `main`, so resolve its tag from its first
    # job's name (release-cli.yml embeds it there).
    dispatch_tag: dict[int, str | None] = {}
    for r in flat_runs:
        if r.get("event") != "workflow_dispatch":
            continue
        found = None
        for j in job_cache.get(r["id"], []):
            m = SEMVER_IN_TEXT.search(j.get("name", ""))
            if m:
                found = m.group(0)
                break
        dispatch_tag[r["id"]] = found

    # Sign and Release runs once per tag push, independently of Release CLI. A
    # failure there never stops release-cli from publishing, so it is invisible
    # to every published/unpublished check and has to be looked for directly.
    sign_runs = gh_json(
        [
            "api", "--paginate", "--slurp",
            f"repos/{repo}/actions/workflows/sign-and-release.yml/runs"
            f"?per_page=100&event=push&created=%3E%3D{created}",
        ]
    )
    flat_sign_runs = [r for page in sign_runs for r in page["workflow_runs"]]

    manifest_text = read_pin_file()
    digest_cache: dict[str, str | None] = {}

    def upstream_digest(url: str) -> str | None:
        """The digest the upstream release asset carries right now."""
        if url not in digest_cache:
            digest_cache[url] = None
            coords = release_asset_coordinates(url)
            if coords:
                owner_repo, rel_tag, asset = coords
                try:
                    rel = gh_json(["api", f"repos/{owner_repo}/releases/tags/{rel_tag}"])
                    for a in (rel or {}).get("assets", []):
                        if a.get("name") == asset and a.get("digest"):
                            digest_cache[url] = a["digest"].removeprefix("sha256:")
                except (subprocess.CalledProcessError, ValueError):
                    pass
        return digest_cache[url]

    states: list[TagState] = []
    for tag, created_at in tags:
        tag_runs = [
            r for r in flat_runs
            if runs_this_tag(r, tag, dispatch_tag.get(r["id"]))
        ]
        rel = by_tag.get(tag)
        root_run, root_jobs = root_cause_run(tag_runs, job_cache)
        diagnoses = diagnose_run(
            "Release CLI", tag, root_run, root_jobs, log_fetcher,
            manifest_text=manifest_text, upstream_digest=upstream_digest,
        )
        sign_failures = sign_and_release_failures(
            repo, tag, flat_sign_runs, log_fetcher,
            manifest_text=manifest_text, upstream_digest=upstream_digest,
        )
        # An upstream-confirmed pinned-asset digest mismatch is a fact about the
        # tag's tree: re-dispatching it would rebuild the whole matrix to fail
        # the same fetch. Open the circuit instead.
        immutable = tuple(
            f"{d.job}: {d.cause} (tag-immutable; the upstream asset changed bytes)"
            for d in diagnoses if d.tag_immutable
        )
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
                validation_failures=latest_validation_failures(
                    tag_runs, job_cache, log_fetcher=log_fetcher
                ) + immutable + repeated_package_failures(
                    tag_runs, job_cache, log_fetcher
                ),
                # A tag push produces one `push` run; every run beyond that is a
                # re-dispatch, ours or a human's.
                dispatch_attempts=sum(
                    1 for r in tag_runs if r["event"] == "workflow_dispatch"
                ),
                failed_legs=newest_failed_legs(tag_runs, job_cache),
                diagnoses=diagnoses,
                sign_failures=sign_failures,
            )
        )
    return states


def read_pin_file(path: str = "tools/deps/manifest.json") -> str:
    """The pin file on the checked-out tree (main), for fix line numbers."""
    try:
        with open(path, encoding="utf-8") as handle:
            return handle.read()
    except OSError:
        return ""


def sign_and_release_failures(
    repo: str,
    tag: str,
    runs: Iterable[dict],
    log_fetcher: Callable[[int | None], str],
    **kwargs,
) -> tuple[Diagnosis, ...]:
    """Explain the tag's newest Sign and Release run, if it failed."""
    newest = max(
        (r for r in runs if r.get("head_branch") == tag),
        key=lambda r: r.get("created_at", ""),
        default=None,
    )
    if not newest or newest.get("status") != "completed" or newest.get("conclusion") != "failure":
        return ()
    try:
        result = gh_json(
            ["api", f"repos/{repo}/actions/runs/{newest['id']}/jobs?per_page=100"]
        )
        jobs = (result or {}).get("jobs", [])
    except subprocess.CalledProcessError:
        jobs = []
    found = diagnose_run("Sign and Release", tag, newest, jobs, log_fetcher, **kwargs)
    if found:
        return found
    # The run failed but no job did (e.g. the workflow file was rejected):
    # still report it rather than read the failure as healthy.
    return (
        diagnose(
            workflow="Sign and Release", tag=tag, run_url=newest.get("html_url", ""),
            job="(no failed job)", step="(none)", log_text="",
        ),
    )


def needs_job_details(run: dict) -> bool:
    """Inspect dispatches and every terminal run that did not fully succeed."""
    if run.get("event") == "workflow_dispatch":
        return True
    return run.get("status") == "completed" and run.get("conclusion") != "success"


def validation_failures(jobs: Iterable[dict]) -> tuple[str, ...]:
    """Return failed steps that establish an immutable product defect."""
    failures, _ = validation_outcomes(jobs)
    return tuple(failures.values())


def validation_outcomes(
    jobs: Iterable[dict],
    log_fetcher: "Callable[[int], str] | None" = None,
) -> tuple[dict[str, str], set[str]]:
    """Return failed and passed product gates, keyed by platform and gate.

    `log_fetcher` is optional so every existing caller keeps working; when it is
    supplied, a failed step outside the name allowlist is additionally checked
    against the tag-immutable signatures. It is consulted only for jobs that
    already failed, so a healthy sweep fetches no logs.
    """
    failures: dict[str, str] = {}
    passed: set[str] = set()
    for job in jobs:
        job_name = job.get("name", "unknown job")
        if log_fetcher is not None and job.get("conclusion") == "failure":
            signature = tag_immutable_failure(log_fetcher(job.get("id")))
            if signature:
                failures[f"{job_name}|tag-immutable"] = (
                    f"{job_name}: {signature} (tag-immutable; a re-run cannot change it)"
                )
        for step in job.get("steps", []):
            name = step.get("name", "")
            prefix = next(
                (
                    candidate
                    for candidate in VALIDATION_STEP_PREFIXES
                    if name.startswith(candidate)
                ),
                None,
            )
            if prefix is None:
                continue
            gate = f"{job_name}|{prefix}"
            if step.get("conclusion") == "failure":
                failures[gate] = f"{job_name}: {name}"
            elif step.get("conclusion") == "success":
                passed.add(gate)
    return failures, passed


def latest_validation_failures(
    runs: Iterable[dict],
    job_cache: dict[int, list[dict]],
    log_fetcher: Callable[[int | None], str] | None = None,
) -> tuple[str, ...]:
    """Return current validation evidence, allowing a later pass to clear it."""
    attempts: list[tuple[str, int, bool, list[dict]]] = []
    for run in runs:
        jobs_by_attempt: dict[int, list[dict]] = {}
        for job in job_cache.get(run["id"], []):
            jobs_by_attempt.setdefault(int(job.get("run_attempt", 1)), []).append(job)
        latest_attempt = int(run.get("run_attempt", max(jobs_by_attempt, default=1)))
        for attempt, jobs in jobs_by_attempt.items():
            # Earlier attempts are necessarily terminal. The current attempt may
            # contribute failure evidence while running, but its passes clear old
            # evidence only after the workflow itself reaches a terminal state.
            may_clear = attempt < latest_attempt or run.get("status") == "completed"
            attempts.append((run.get("created_at", ""), attempt, may_clear, jobs))

    newest_first = sorted(attempts, key=lambda item: (item[0], item[1]), reverse=True)
    cleared: set[str] = set()
    active: dict[str, str] = {}
    for _, _, may_clear, jobs in newest_first:
        failures, passed = validation_outcomes(jobs, log_fetcher)
        for gate, why in failures.items():
            if gate not in cleared and gate not in active:
                active[gate] = why
        if may_clear:
            cleared.update(passed)
    return tuple(active.values())


def incident_stays_open(
    state: TagState,
    *,
    newest_published: tuple | None,
    asset_floor: tuple | None,
    max_attempts: int,
) -> bool:
    """Keep an escalated incident stable while a human repair is running."""
    return state.unresolved(
        newest_published=newest_published, floor=asset_floor
    ) and (state.circuit_open or state.dispatch_attempts >= max_attempts)


def runs_this_tag(run: dict, tag: str, dispatch_tag: str | None = None) -> bool:
    """Does this workflow run belong to `tag`?

    `head_branch` is NOT sufficient. For a tag push it is the tag, but for a
    workflow_dispatch it is the ref the run was dispatched FROM — `main` — because
    we deliberately dispatch main's (fixed) workflow to build an old tag's source.
    Matching on head_branch alone makes every REPAIR run invisible: zero attempts
    counted, escalation never fires, and a fresh re-dispatch goes out every 30
    minutes forever.

    So a dispatch run is attributed by the tag embedded in its FIRST JOB's name
    (`dispatch_tag`, read from the jobs API by `collect()`).

    It is NOT attributed by a `run-name`, and release-cli.yml must never set one.
    GitHub returns `run-name` as `workflow_run.name` — REPLACING the workflow name —
    and the self-hosted tartci supervisor picks up its work with
    `select(.name == "Release CLI")`. A run-name therefore hides every release run
    from the supervisor, which then reports `queued=0` and never boots a macOS VM.
    No runner, no release, ever. Job names are a separate API field and are safe.
    """
    if run.get("head_branch") == tag:
        return True
    return dispatch_tag is not None and dispatch_tag == tag


def redispatch(repo: str, tag: str, dry_run: bool) -> None:
    """Rebuild and republish `tag` from the tag itself.

    `--ref <tag>` is required: release-cli's "Dispatch must run on the tag it
    publishes" guard refuses any dispatch whose run commit is not the tag
    commit, because the build provenance would otherwise name a different
    commit than the one consumers pin. A dispatch from `main` therefore fails
    in seconds without building anything — and, worse, its failure used to be
    reported as the tag's failing leg, hiding the real cause. If the tag's own
    workflow is broken, the fix is a new tag, not a dispatch from `main`.
    `make_latest=true` only *permits* the latest pointer — release-cli's
    finalizer still refuses to move /releases/latest backward.
    """
    cmd = [
        "gh", "workflow", "run", "release-cli.yml",
        "--repo", repo, "--ref", tag,
        "-f", f"version={tag}",
        "-f", "make_latest=true",
    ]
    if dry_run:
        print(f"    DRY RUN: would run: {' '.join(cmd)}")
        return
    subprocess.run(cmd, check=True)
    print(f"    re-dispatched release-cli for {tag}")


# Job logs carry ANSI colour codes. Newer gh refuses to print a response that
# contains terminal escape sequences unless told to, and exits 1 — which the
# runner's gh started doing while this host's older gh did not. Every log then
# read as "", so no failure could be classified and every tag-immutable
# circuit stayed shut. Ask for the bytes; fall back for a gh too old to know
# the flag.
_ALLOW_ESCAPES = "--allow-escape-sequences"


def fetch_job_log(
    repo: str, job_id: int, *, run: "Callable[..., subprocess.CompletedProcess]" = None
) -> tuple[str, str]:
    """Return (log text, "") or ("", why it could not be fetched).

    A failure is returned, never swallowed: an unfetched log means "no
    evidence", and the caller must be able to say so rather than report
    "no error line in the job log" for a log it never saw.
    """
    run = run or subprocess.run
    base = ["gh", "api", f"repos/{repo}/actions/jobs/{job_id}/logs"]
    error = ""
    for cmd in ([*base, _ALLOW_ESCAPES], base):
        try:
            completed = run(
                cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, errors="replace",
                timeout=60, check=False,
            )
        except (subprocess.SubprocessError, OSError) as exc:
            return "", f"{exc!r}"[:300]
        if completed.returncode == 0:
            return completed.stdout, ""
        error = (completed.stderr or "").strip()[:300] or f"gh exited {completed.returncode}"
        if _ALLOW_ESCAPES not in error and "unknown flag" not in error:
            break
    return "", error


# A `Package CLI` step that fails the same way on two separate runs of one tag
# is a property of the tag: the packager and everything it reads are frozen in
# the tag's tree, and marker-era tags refuse current-main overlays. v0.963.0 to
# v0.965.0 each failed `Package CLI (Unix)` with "wgpu native library not found"
# and were re-dispatched hourly — seven ~2-hour matrices for an outcome no
# re-run could change. One failure stays on the retry path (a runner can die
# mid-package); an identical repeat opens the circuit.
PACKAGE_STEP_PREFIX = "Package CLI"


def repeated_package_failures(
    runs: Iterable[dict],
    job_cache: dict[int, list[dict]],
    log_fetcher: Callable[[int | None], str],
) -> tuple[str, ...]:
    """Signatures of package failures that repeated across runs of one tag."""
    seen: dict[tuple[str, str, str], set[int]] = {}
    for run in runs:
        if run.get("status") != "completed":
            continue
        jobs = job_cache.get(run["id"], [])
        if not jobs:
            continue
        latest = max(int(j.get("run_attempt", 1)) for j in jobs)
        for job in jobs:
            if int(job.get("run_attempt", 1)) != latest or job.get("conclusion") != "failure":
                continue
            step = _first_failed_step(job)
            if not step.startswith(PACKAGE_STEP_PREFIX):
                continue
            # Without the log there is no evidence the two failures are the same.
            first = first_error_line(log_fetcher(job.get("id")))
            if not first:
                continue
            seen.setdefault((job.get("name", "unknown job"), step, first), set()).add(run["id"])
    return tuple(
        f"{job}: {step} failed identically on {len(ids)} runs of this tag — {first} "
        "(tag-immutable; the packager is frozen in the tag)"
        for (job, step, first), ids in sorted(seen.items())
        if len(ids) >= 2
    )


def episode_title(report: list[tuple[str, str]], now: datetime) -> str:
    """Name a stall episode after its oldest unresolved tag and its start date."""
    tags = [tag for tag, _ in report]
    oldest = min(tags, key=lambda t: version_of(t) or (0,))
    return f"{EPISODE_TITLE_PREFIX}{oldest} unpublished ({now:%Y-%m-%d})"


def is_incident(issue: dict) -> bool:
    title = issue.get("title", "")
    return title == INCIDENT_TITLE or title.startswith(EPISODE_TITLE_PREFIX)


def sync_incident(
    repo: str, report: list[tuple[str, str]], dry_run: bool, *, now: datetime | None = None
) -> None:
    """Keep exactly one OPEN incident per stall episode.

    A stall that began yesterday must read as news. The previous design kept one
    issue for the reconciler's whole lifetime and reopened it: #6880 had been
    reopened and closed many times since July, so its reopen for v0.963.0 at
    2026-10-13 22:31Z read as routine and a stuck release went unnoticed for
    hours. Now:

      - A new episode (nothing open) creates a NEW issue whose title names the
        oldest unresolved tag and the date. A closed issue is never reopened.
      - While an episode is open it is edited in place, with one comment per
        tag that joins it, so the issue count stays one per episode, not one
        per sweep or per tag (release-health once filed 413 in two weeks).
      - The label exists and is part of the lookup, and only OPEN issues match,
        so the lookup cannot mint a duplicate during an episode.
    """
    now = now or datetime.now(timezone.utc)
    found = gh_json(
        [
            "issue", "list", "--repo", repo, "--state", "open", "--limit", "50",
            "--label", INCIDENT_LABEL, "--search", "in:title Release",
            "--json", "number,title,state",
        ]
    )
    match = next(
        (i for i in found or []
         if is_incident(i) and i.get("state", "").lower() == "open"),
        None,
    )

    if not report:
        if match and not dry_run:
            subprocess.run(
                ["gh", "issue", "close", str(match["number"]), "--repo", repo,
                 "--comment", "Every recent tag is published and complete. Closing."],
                check=True,
            )
            print(f"  closed incident #{match['number']} — all clear")
        return

    body = incident_body(report)
    if dry_run:
        title = match["title"] if match else episode_title(report, now)
        print(f"  DRY RUN: would report {len(report)} unresolved tag(s) as {title!r}")
        print("  DRY RUN: incident body follows\n" + body)
        return
    _write_incident(repo, match, report, body, episode_title(report, now))


# GitHub rejects an issue body over 65,536 characters, and a rejected edit fails
# every sweep. Stay well under it.
INCIDENT_BODY_LIMIT = 60_000
# The newest tags keep their full diagnosis; older ones get one line each.
INCIDENT_FULL_ENTRIES = 5
_ENTRY_DETAIL_LIMIT = 8_000
_SUMMARY_LIMIT = 240

_INCIDENT_FOOTER = (
    "\n\nOpen the failing `release-cli.yml` run and land the fix on `main`. "
        "A product-validation circuit stays open until a later run passes the "
        "failed gate: manually dispatch the repaired workflow for the same tag "
        "when a pipeline overlay can fix it, or ship a new patch tag when product "
        "source must change. A published release missing assets also requires a "
        "new patch tag because published releases are immutable.\n\nMaintained "
        "in place by "
        "`.github/workflows/release-reconcile.yml`; it closes itself once every "
        "recent tag is published and complete."
)
_INCIDENT_HEADER = (
    "Tags the release reconciler could not get published on its own: each "
    "one exhausted its retry budget, failed in a way a rebuild cannot fix, "
    "or is still unpublished long after it was cut. Every entry names the "
    "release legs that failed.\n\n"
)


def incident_body(
    report: list[tuple[str, str]],
    *,
    full_entries: int = INCIDENT_FULL_ENTRIES,
    limit: int = INCIDENT_BODY_LIMIT,
) -> str:
    """Render the incident, bounded so GitHub always accepts the edit.

    `report` is newest-first. The newest `full_entries` tags keep their full
    diagnosis (each still capped); older tags collapse to one line. If even
    that overflows, the oldest one-liners are folded into a single line that
    still names every tag, so `newly_reported` never mistakes them for new.
    """
    entries: list[str] = []
    for index, (tag, why) in enumerate(report):
        if index < full_entries:
            text = why if len(why) <= _ENTRY_DETAIL_LIMIT else (
                why[:_ENTRY_DETAIL_LIMIT].rsplit("\n", 1)[0]
                + "\n  - _(diagnosis truncated; open the linked run)_"
            )
        else:
            first = why.split("\n", 1)[0]
            text = first if len(first) <= _SUMMARY_LIMIT else first[:_SUMMARY_LIMIT] + "…"
            text += " _(older tag: summarised)_"
        entries.append(f"- `{tag}` — {text}")

    def assemble(kept: list[str], folded: list[str]) -> str:
        lines = list(kept)
        if folded:
            lines.append("- older tags folded to save space: "
                         + ", ".join(f"`{t}`" for t in folded))
        return _INCIDENT_HEADER + "\n".join(lines) + _INCIDENT_FOOTER

    kept, folded = entries, []
    body = assemble(kept, folded)
    while len(body) > limit and len(kept) > min(full_entries, len(kept)) and len(kept) > 1:
        kept = kept[:-1]
        folded = [report[len(kept)][0]] + folded
        body = assemble(kept, folded)
    return body


def _write_incident(
    repo: str, match: dict | None, report: list[tuple[str, str]], body: str, title: str
) -> None:
    if match:
        # Editing a long-open issue notifies nobody, which is how five broken
        # releases sat in an already-open incident unnoticed. Comment whenever
        # a tag joins the incident, so each new failure is an event.
        previous = gh_json(
            ["issue", "view", str(match["number"]), "--repo", repo, "--json", "body"]
        )
        new_tags = newly_reported((previous or {}).get("body", ""), report)
        subprocess.run(
            ["gh", "issue", "edit", str(match["number"]), "--repo", repo,
             "--body", body],
            check=True,
        )
        if new_tags:
            subprocess.run(
                ["gh", "issue", "comment", str(match["number"]), "--repo", repo,
                 "--body", "Newly unresolved: "
                 + ", ".join(f"`{t}`" for t in new_tags)
                 + ". Each entry in the issue body names the failing run, job and "
                 "step, the first error line, the cause class, the fix, and how "
                 "to verify it."],
                check=True,
            )
        print(f"  updated incident #{match['number']} ({len(report)} unresolved)")
    else:
        subprocess.run(
            ["gh", "issue", "create", "--repo", repo, "--title", title,
             "--body", body, "--label", INCIDENT_LABEL],
            check=True,
        )
        print(f"  opened incident {title!r} ({len(report)} unresolved)")


_TAG_IN_ENTRY = re.compile(r"`(v\d+\.\d+\.\d+)`")


def newly_reported(previous_body: str, report: list[tuple[str, str]]) -> list[str]:
    """Tags in `report` that the incident body did not already list.

    Only column-0 entry lines count (indented diagnosis lines mention tags in
    commands); an entry's own tag comes first, and the folded line lists many.
    """
    seen: set[str] = set()
    for line in previous_body.splitlines():
        if line.startswith("- older tags folded"):
            seen.update(_TAG_IN_ENTRY.findall(line))
        elif line.startswith("- `"):
            found = _TAG_IN_ENTRY.search(line)
            if found:
                seen.add(found.group(1))
    return [tag for tag, _ in report if tag not in seen]


def main(argv: Sequence[str] | None = None) -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--tag", action="append", default=[],
        help="Evaluate only these tags (repeatable). Implies a dry run that "
             "prints the incident body; for reproducing a past incident.",
    )
    args = parser.parse_args(argv if argv is not None else [])
    repo = os.environ["REPO"]
    dry_run = os.environ.get("DRY_RUN", "false") == "true"
    grace = int(os.environ.get("GRACE_MINUTES", "45"))
    drought_hours = float(os.environ.get("DROUGHT_HOURS", "2"))
    max_age = int(os.environ.get("MAX_AGE_HOURS", "72"))
    max_attempts = int(os.environ.get("MAX_ATTEMPTS", "3"))
    now = datetime.now(timezone.utc)

    # Look slightly further back for RUNS than for tags: a tag at the edge of the
    # window may have been built by a run that started before it.
    since = now - timedelta(hours=max_age + 24)
    if args.tag:
        dry_run = True
        tags = [(t, tag_created_at(t)) for t in args.tag]
        since = min(created for _, created in tags) - timedelta(hours=24)
    else:
        tags = sdk_tags(max_age, now)
    states = collect(repo, tags, since)
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
            stuck_queue_hours=int(os.environ.get("STUCK_QUEUE_HOURS", "4")),
        )
        print(f"  [{decision.action:11}] {decision.reason}")
        dry_spell = drought(
            state, now, drought_hours=drought_hours, newest_published=newest_published
        )
        if decision.action == REDISPATCH:
            # Report before the budget check: a tag whose re-dispatch is
            # deferred this sweep is no less overdue.
            if dry_spell:
                report.append((state.tag, f"{dry_spell}; re-dispatching"))
            if budget <= 0:
                print("    (deferred — re-dispatch budget for this sweep is spent)")
            else:
                budget -= 1
                redispatch(repo, state.tag, dry_run)
        elif decision.action in (CIRCUIT_OPEN, ESCALATE, INCOMPLETE, STUCK_QUEUE):
            reason = decision.reason
            if decision.action != INCOMPLETE and state.failed_legs:
                reason = f"{reason}; {describe_legs(state)}"
            report.append((state.tag, reason))
        elif incident_stays_open(
            state,
            newest_published=newest_published,
            asset_floor=asset_floor,
            max_attempts=max_attempts,
        ):
            # Already escalated on a previous sweep and STILL not published — a
            # human is presumably mid-repair, so it has a live run and is not
            # ESCALATE this time round. It stays in the incident until it actually
            # publishes; otherwise the incident would close the moment a retry
            # started and reopen the moment it failed, flapping on every sweep.
            report.append(
                (state.tag, f"still unpublished; repair in progress; {describe_legs(state)}")
            )
        elif dry_spell:
            report.append((state.tag, dry_spell))

        if state.sign_failures and not superseded(state.tag, newest_published):
            if not any(tag == state.tag for tag, _ in report):
                report.append((state.tag, "`Sign and Release` failed for this tag"))

    # Every entry carries its root-cause explanation, so the incident says what
    # to change and how to verify it without anyone opening a log.
    by_tag = {s.tag: s for s in states}
    report = [(tag, why + render_diagnoses(by_tag[tag])) if tag in by_tag else (tag, why)
              for tag, why in report]
    sync_incident(repo, report, dry_run)
    # Reporting a problem is not itself a failure — exiting non-zero here would
    # just mint another red X for the watchdogs to shout about.
    return 0


def tag_created_at(tag: str) -> datetime:
    out = subprocess.run(
        ["git", "for-each-ref", "--format=%(creatordate:iso-strict)", f"refs/tags/{tag}"],
        capture_output=True, text=True, encoding="utf-8", check=True,
    ).stdout.strip()
    if not out:
        raise SystemExit(f"release_reconcile: unknown tag {tag} (fetch tags first)")
    return parse_ts(out)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
