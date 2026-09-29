#!/usr/bin/env python3
"""Attribute a failed merge_group batch to the pull request that owns its failing tests.

A merge_group batch is NAMED for one pull request but CONTAINS every entry
ahead of it, so the branch name in the run title is not the culprit. Reading it
as one is a misattribution that sends people to fix an innocent branch while
the real break sits untouched, and a merge queue repeats the mistake on every
batch it re-forms.

The signal that works: a failing ctest name usually maps to a file, and the
pull request that adds or touches that file owns the failure.
`prepush-cannot-measure` maps to `tools/scripts/test_prepush_cannot_measure.py`,
which maps to the one pull request adding it.

That signal is blind to a gate that reports a COUNT rather than a file, and the
consumption census is exactly one: its public header lists are walked live from
each target's exported include roots, so one header added under a root a target
already exports drifts the census while sharing no token with the gate's name.
The header's own pull request then scores zero everywhere and the batch reads as
"pre-existing on main" -- a phantom that sends people to re-measure a census
that was already true. Census gates therefore get their own ownership rule,
driven by the exported include roots the committed census itself names.

The threshold matters as much as the mapping. Incidental token overlap scores
low, and reporting a low score as a culprit is a false accusation -- worse than
no attribution, because someone acts on it. Below the confidence threshold this
reports a likely pre-existing break on main instead of naming anyone.

Scoring is scoped to the batch's ACTUAL members. A batch holds every entry
ahead of the one it is named for, and an open pull request that is not in it can
still touch the same surface -- so scoring the whole open list lets an innocent
branch outrank the real owner. Membership is read from the queue ref the run was
built on; when it cannot be read, that is said out loud rather than assumed.

A second consumer reads the same evidence as a machine: Shipyard's
`queue-arm-guard` refuses a same-head re-enqueue after a `failed_checks`
ejection, because re-queueing a head that broke a batch ejects its innocent
batch-mates all over again. `--certify` answers the narrower question the guard
asks -- did THIS batch's failure implicate this head -- and certifies only on
POSITIVE evidence that it did not.

The strongest such evidence is the batch's chain ancestry: the queue ref names
the commit the entry was stacked on, so a parent that passed makes the head the
culprit, and a parent that failed every test this batch failed makes the head a
neighbour of the entry that broke it. See "Chain ancestry" below.

"No test failure was found, therefore infrastructure" is NOT that evidence, and
is the rule to resist: a compile or link error is the most common way a head
breaks a batch and it produces no ctest block at all. So certification is
per-failing-step and exhaustive. Every failing step of every failing job must be
positively explained by something that cannot be this head; one unexplained step
refuses the whole verdict.

Read-only. Prints findings; mutates nothing.
"""

from __future__ import annotations

import argparse
import io
import json
import os
import posixpath
import re
import shutil
import subprocess
import sys
import zipfile
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Callable, Iterable

DEFAULT_REPO = "Generous-Corp/pulp"

# An exact test-name/file-stem match is decisive; containment is strong;
# incidental multi-token overlap is weak and must not name a culprit alone.
WEIGHT_EXACT_STEM = 100
WEIGHT_PATH_CONTAINS = 50
MIN_INCIDENTAL_TOKENS = 2

# Below this total strength, no pull request is named.
CONFIDENT = 50

VERDICT_CULPRIT = "culprit"
VERDICT_PRE_EXISTING = "pre-existing-on-main"
VERDICT_UNOWNED = "unowned"

# The census gates real drift takes down. The drift gate compares the measured
# graph against the committed census; the negative contract opens with a control
# that feeds UNMODIFIED inputs, so live drift fails it too. The rest of the
# census family is deliberately absent: the schema and description checks read
# the committed census with no build tree, and the contract control note stages
# its own drift and still passes when the tree is already drifted.
CENSUS_TESTS = frozenset(
    {
        "consumption-census-drift",
        "consumption-census-negative-contract",
    }
)

CENSUS_RELPATH = "docs/status/consumption-profiles.json"

# The census counts these and only these under an exported include root.
HEADER_SUFFIXES = (".h", ".hpp")

# Editing the census, its schema, its generator or its facts dump owns a census
# failure directly; adding or deleting a counted header owns it by moving a
# number nothing in the diff names.
CENSUS_ARTIFACTS = frozenset(
    {
        CENSUS_RELPATH,
        "docs/status/consumption-profiles.schema.json",
        "tools/scripts/consumption_census.py",
        "tools/scripts/consumption_census_contract.py",
        "test/cmake/consumption_census_tests.cmake",
    }
)

# A header the census counts, added or deleted, IS the cause of a count change,
# so this is as decisive as an exact stem match. Touching a census artifact is
# ownership too, but of a gate that could have failed several ways, so it lands
# at the containment weight rather than above it.
WEIGHT_CENSUS_HEADER = 100
WEIGHT_CENSUS_ARTIFACT = 50


def gh_cli() -> str:
    """The GitHub CLI every API read goes through.

    `PULP_GH_CLI` wins: it is the single override every CI helper reads, and a
    GitHub runner sets it to `gh`, where no App wrapper exists. Otherwise
    `ghapp`, when PATH resolves it.

    Shipyard's queue-arm guard runs `--certify` with PATH reduced to the system
    directories, so the `ghapp` installed under ~/.local/bin is not on it. The
    wrapper that runs the guard exports `GHAPP_REAL_GH`, the real `gh` binary,
    and `GH_TOKEN`, the App token it minted for this command, so that binary
    answers the same reads under the same identity.
    """
    override = (os.environ.get("PULP_GH_CLI") or "").strip()
    if override:
        return override
    if shutil.which("ghapp"):
        return "ghapp"
    real = (os.environ.get("GHAPP_REAL_GH") or "").strip()
    if real and os.environ.get("GH_TOKEN") and os.access(real, os.X_OK):
        return real
    return "ghapp"


def run_gh(args: list[str], **kwargs) -> subprocess.CompletedProcess:
    """Run the GitHub CLI; a CLI that cannot be found is an unusable invocation.

    Exits 2 with a message rather than a traceback, so a caller that reads only
    the exit status and stderr (the queue-arm guard) sees why nothing ruled.
    """
    cli = gh_cli()
    try:
        return subprocess.run([cli, *args], **kwargs)
    except FileNotFoundError:
        print(
            f"queue_batch_attribute: GitHub CLI {cli!r} is not on PATH "
            f"({os.environ.get('PATH', '')}); set PULP_GH_CLI to a gh-compatible CLI",
            file=sys.stderr,
        )
        raise SystemExit(2) from None


def gh(
    path: str,
    jq: str | None = None,
    repo_cwd: str | None = None,
    paginate: bool = False,
) -> str | None:
    args = ["api", path]
    if paginate:
        args.append("--paginate")
    args += ["--jq", jq] if jq else []
    proc = run_gh(args, capture_output=True, text=True, cwd=repo_cwd)
    if proc.returncode != 0:
        return None
    return proc.stdout.strip()


def slug(test_name: str) -> list[str]:
    """A ctest name -> tokens likely to appear in the owning file path."""
    return [t for t in re.split(r"[^a-z0-9]+", test_name.lower()) if len(t) > 3]


def canonical(test_name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", test_name.lower()).strip("_")


def file_stem(path: str) -> str:
    stem = path.lower().rsplit("/", 1)[-1].rsplit(".", 1)[0]
    return re.sub(r"^test_", "", stem)


def score_match(test_name: str, path: str) -> int:
    """How strongly `path` looks like the owner of `test_name`."""
    canon = canonical(test_name)
    lowered = path.lower()
    if canon and canon == file_stem(path):
        return WEIGHT_EXACT_STEM
    if canon and canon in lowered.replace("-", "_"):
        return WEIGHT_PATH_CONTAINS
    hits = sum(1 for tok in slug(test_name) if tok in lowered)
    return hits if hits >= MIN_INCIDENTAL_TOKENS else 0


@dataclass(frozen=True)
class ChangedFile:
    """One entry in a pull request's diff, with the part a census needs.

    `status` defaults to "modified" because an unknown status must not be able
    to claim census ownership: only ADDING or DELETING a counted header moves a
    header count, and inferring that from a bare path would be the false
    accusation this module exists to avoid.
    """

    path: str
    status: str = "modified"
    previous_path: str = ""


def as_changed(entry: ChangedFile | str) -> ChangedFile:
    """Accept a bare path so callers that have no diff status still work."""
    return entry if isinstance(entry, ChangedFile) else ChangedFile(path=entry)


def census_include_roots(source_root: Path) -> frozenset[str]:
    """Exported include roots the committed census counts headers under.

    Read out of the census rather than hardcoded. The roots ARE the published
    measurement, so a list kept alongside it would drift from it and attribute a
    drift to the wrong diff -- the failure mode this whole rule exists to end.
    """
    try:
        census = json.loads((source_root / CENSUS_RELPATH).read_text())
    except (OSError, ValueError):
        return frozenset()
    roots: set[str] = set()
    pending = [census]
    while pending:
        node = pending.pop()
        if isinstance(node, dict):
            headers = node.get("public_headers")
            if isinstance(headers, dict):
                for root in headers.get("roots") or ():
                    if isinstance(root, str) and root:
                        roots.add(posixpath.normpath(root))
            pending.extend(node.values())
        elif isinstance(node, list):
            pending.extend(node)
    return frozenset(roots)


def counted_header(path: str, roots: frozenset[str]) -> bool:
    """Would the census count `path` as public header surface?"""
    if not path.endswith(HEADER_SUFFIXES):
        return False
    normalized = posixpath.normpath(path)
    return any(
        normalized == root or normalized.startswith(root + "/") for root in roots
    )


def census_weight(test_name: str, change: ChangedFile, roots: frozenset[str]) -> int:
    """How strongly `change` looks like the cause of a census gate failure."""
    if test_name not in CENSUS_TESTS:
        return 0
    if posixpath.normpath(change.path) in CENSUS_ARTIFACTS:
        return WEIGHT_CENSUS_ARTIFACT
    if not roots:
        return 0
    now = counted_header(change.path, roots)
    if change.status in ("added", "removed"):
        return WEIGHT_CENSUS_HEADER if now else 0
    if change.status == "renamed":
        # A rename moves the count only when it crosses the counted set. With no
        # previous path there is no way to tell, so refuse rather than guess.
        if not change.previous_path:
            return 0
        before = counted_header(change.previous_path, roots)
        return WEIGHT_CENSUS_HEADER if now != before else 0
    return 0


@dataclass
class Attribution:
    """Who owns a batch's failing tests, and how sure we are."""

    tests: list[str]
    # pr number -> test name -> (weight, owning files)
    scores: dict[int, dict[str, tuple[int, set[str]]]] = field(default_factory=dict)
    strength: dict[int, int] = field(default_factory=dict)
    verdict: str = VERDICT_UNOWNED
    culprit: int | None = None
    # Which failing tests are census gates, and whether the roots needed to
    # judge them were actually loaded. An unevaluated rule must never read as
    # "nobody owns it".
    census_tests: list[str] = field(default_factory=list)
    census_roots_read: bool = True
    # Every candidate tied at the top strength. A single arbitrary pick among
    # equals is a coin toss presented as a finding.
    contenders: list[int] = field(default_factory=list)
    batch_members_known: bool = False
    scoped_out: list[int] = field(default_factory=list)

    @property
    def best_strength(self) -> int:
        return max(self.strength.values(), default=0)


def attribute(
    tests: list[str],
    pr_files: dict[int, list[ChangedFile | str]],
    census_roots: frozenset[str] = frozenset(),
    batch_members: frozenset[int] | None = None,
) -> Attribution:
    """Map failing tests onto the pull requests whose files own them."""
    result = Attribution(tests=list(tests))
    result.census_tests = [test for test in tests if test in CENSUS_TESTS]
    result.census_roots_read = bool(census_roots)
    result.batch_members_known = batch_members is not None
    candidates = pr_files
    if batch_members is not None:
        candidates = {n: f for n, f in pr_files.items() if n in batch_members}
        result.scoped_out = sorted(set(pr_files) - set(candidates))
    for test in tests:
        for number, entries in candidates.items():
            for entry in entries:
                change = as_changed(entry)
                weight = max(
                    score_match(test, change.path),
                    census_weight(test, change, census_roots),
                )
                if not weight:
                    continue
                prev_weight, prev_files = result.scores.setdefault(
                    number, {}
                ).setdefault(test, (0, set()))
                result.scores[number][test] = (
                    max(prev_weight, weight),
                    prev_files | {change.path},
                )

    result.strength = {
        number: sum(weight for weight, _ in per_test.values())
        for number, per_test in result.scores.items()
    }

    if not result.scores:
        result.verdict = VERDICT_UNOWNED
    elif result.best_strength < CONFIDENT:
        result.verdict = VERDICT_PRE_EXISTING
    else:
        result.verdict = VERDICT_CULPRIT
        best = result.best_strength
        result.contenders = sorted(
            number for number, total in result.strength.items() if total == best
        )
        result.culprit = result.contenders[0]
    return result


# A merge-queue ref names the LAST entry in the batch and the base it was built
# on: `gh-readonly-queue/main/pr-8726-<base sha>`.
BATCH_BRANCH_RE = re.compile(r"^gh-readonly-queue/[^/]+/pr-(\d+)-([0-9a-f]{40})$")
MERGED_PR_RE = re.compile(r"^Merge pull request #(\d+)\b")


def parse_batch_members(head_branch: str, subjects: list[str]) -> frozenset[int] | None:
    """The pull requests a merge_group batch actually contains.

    `None` means the membership could not be determined -- never an empty batch.
    The two must stay distinguishable: an empty set silently scopes every
    candidate out and reports nobody, which is the same phantom as blaming main.
    """
    match = BATCH_BRANCH_RE.match(head_branch.strip())
    if not match:
        return None
    members = {int(match.group(1))}
    # The range's merge subjects name the entries ahead of that one. A member's
    # own branch can also carry a "Merge pull request #N" subject, which admits N
    # as a candidate it is not; that over-admits at worst to the pre-scoping
    # behaviour, and only for a pull request whose files already own the failure.
    for subject in subjects:
        merged = MERGED_PR_RE.match(subject.strip())
        if merged:
            members.add(int(merged.group(1)))
    return frozenset(members)


def batch_members(repo: str, run_id: str) -> frozenset[int] | None:
    raw = gh(f"repos/{repo}/actions/runs/{run_id}", "[.head_branch,.head_sha]|@tsv")
    if not raw or "\t" not in raw:
        return None
    head_branch, head_sha = raw.split("\t", 1)
    match = BATCH_BRANCH_RE.match(head_branch.strip())
    if not match:
        return None
    subjects = gh(
        f"repos/{repo}/compare/{match.group(2)}...{head_sha.strip()}",
        '.commits[]|.commit.message|split("\\n")[0]',
    )
    return parse_batch_members(head_branch, (subjects or "").splitlines())


def run_log_zip(repo: str, run_id: str) -> bytes | None:
    """A run's logs as the zip GitHub serves, or None.

    The RUN-level endpoint, never the per-job one: `ghapp` withholds any response
    carrying terminal escape sequences, and a job log is full of them, so
    `actions/jobs/<id>/logs` returns zero bytes and every reader built on it sees
    an empty log rather than an error. The run endpoint serves a zip, which is
    binary and passes through.
    """
    proc = run_gh(
        ["api", f"repos/{repo}/actions/runs/{run_id}/logs"],
        capture_output=True,
    )
    if proc.returncode != 0 or not proc.stdout:
        return None
    return proc.stdout


def unpack_job_logs(archive: bytes) -> dict[str, str]:
    """Job name -> log text, from a run-log zip.

    Entries are named `<ordinal>_<job name>.txt`, and the ordinal is the job's
    position in THIS run, not a stable id: the same job is `2_macos.txt` in one
    run and `3_macos.txt` in the next. Keyed by the job name so a caller can ask
    for the job the jobs API named.
    """
    logs: dict[str, str] = {}
    try:
        with zipfile.ZipFile(io.BytesIO(archive)) as bundle:
            for entry in bundle.namelist():
                match = JOB_LOG_RE.match(entry)
                if not match:
                    continue
                try:
                    logs[match.group(1)] = bundle.read(entry).decode(
                        "utf-8", errors="replace"
                    )
                except (KeyError, OSError):
                    continue
    except (zipfile.BadZipFile, OSError):
        return {}
    return logs


def failing_tests(repo: str, run_id: str) -> list[str]:
    """Every ctest name that failed in a run, across all of its jobs."""
    archive = run_log_zip(repo, run_id)
    if not archive:
        return []
    names: list[str] = []
    for text in unpack_job_logs(archive).values():
        for name in parse_failing_tests(text):
            if name not in names:
                names.append(name)
    return names


# One entry of ctest's "The following tests FAILED:" block. A test name may
# contain spaces, so the name is everything up to the parenthesised result.
FAILED_TEST_LINE_RE = re.compile(r"\d+\s+-\s+(.+?)\s+\((Failed|Timeout|Subprocess aborted)\)")


def parse_failing_tests(log: str) -> list[str]:
    """Pull the ctest failure block out of a job log."""
    names: list[str] = []
    seen = False
    for line in log.splitlines():
        if "The following tests FAILED" in line:
            seen = True
            continue
        if not seen:
            continue
        match = FAILED_TEST_LINE_RE.search(line)
        if match:
            names.append(match.group(1).strip())
        elif "Errors while running CTest" in line and names:
            # ctest writes this to stderr, so in a merged log it can land
            # before the block's entries as well as after them.
            break
    return names


def open_prs(repo: str) -> list[int]:
    raw = gh(f"repos/{repo}/pulls?state=open&per_page=100", ".[].number")
    return [int(n) for n in raw.splitlines()] if raw else []


def pr_files(repo: str, number: int) -> list[ChangedFile]:
    """Every file in a pull request's diff, with its status.

    Paginated: a truncated first page hides exactly the kind of change census
    ownership turns on, and a header add missing from the diff reproduces the
    false negative under a different name.
    """
    raw = gh(
        f"repos/{repo}/pulls/{number}/files?per_page=100",
        '.[]|[.filename,.status,(.previous_filename//"")]|@tsv',
        paginate=True,
    )
    changes: list[ChangedFile] = []
    for line in (raw or "").splitlines():
        fields = line.split("\t")
        if not fields[0]:
            continue
        changes.append(
            ChangedFile(
                path=fields[0],
                status=fields[1] if len(fields) > 1 and fields[1] else "modified",
                previous_path=fields[2] if len(fields) > 2 else "",
            )
        )
    return changes


def latest_failed_merge_group(repo: str) -> str:
    raw = gh(
        f"repos/{repo}/actions/workflows/build.yml/runs?event=merge_group&per_page=20",
        '[.workflow_runs[]|select(.conclusion=="failure")]|.[0].id',
    )
    return (raw or "").strip()


def batch_notes(result: Attribution) -> list[str]:
    """Say what was in scope, because a finding is only as good as its field."""
    if not result.batch_members_known:
        return [
            "  ! this batch's membership could not be read, so EVERY open pull",
            "    request was scored: a name above may not be in the batch at all.",
        ]
    if result.scoped_out:
        return [
            f"  ({len(result.scoped_out)} other open PR(s) scored nothing: not in "
            "this batch)"
        ]
    return []


def census_notes(result: Attribution) -> list[str]:
    """Explain a census attribution, or say the rule could not be applied.

    A census gate reports a count, so nothing in its name points at the header
    that moved it: an unexplained census attribution reads as a non sequitur and
    gets discarded. And a census failure judged with no roots loaded is an
    UNEVALUATED rule, which must be said out loud rather than reported as an
    absence of owners.
    """
    if not result.census_tests:
        return []
    if not result.census_roots_read:
        return [
            "  ! a census gate failed but no exported include roots were read from",
            f"    {CENSUS_RELPATH}: header-count ownership was NOT evaluated, so",
            "    any 'pre-existing on main' reading above is unproven. Re-run",
            "    with --source-root pointing at a checkout.",
        ]
    return [
        "  note: a census gate counts headers, so its name matches no file by",
        "    design. It is owned by a header ADDED or DELETED under an exported",
        f"    include root named in {CENSUS_RELPATH} -- not by an in-place edit.",
    ]


def render(result: Attribution, run_id: str) -> list[str]:
    out = [f"batch run {run_id}: {len(result.tests)} failing test(s)"]
    if result.verdict == VERDICT_UNOWNED:
        out.append(
            "  NO OPEN PR OWNS THESE TESTS -> likely pre-existing on main, or an"
        )
        out.append(
            "  entry already merged. Do NOT blame the batch's branch name."
        )
        out += [f"    - {t}" for t in result.tests[:6]]
        return out + batch_notes(result) + census_notes(result)

    if result.verdict == VERDICT_PRE_EXISTING:
        out.append(
            f"  {len(result.tests)} failing test(s); NO open PR matches any of "
            f"them strongly (best score {result.best_strength})."
        )
        out.append("  => LIKELY PRE-EXISTING ON MAIN. Do not blame a batch member;")
        out.append("     check whether main's own suite is red before re-queueing.")
        ranked = sorted(
            result.scores.items(), key=lambda kv: (-result.strength[kv[0]], kv[0])
        )
        for number, per_test in ranked[:3]:
            test, (weight, files) = list(per_test.items())[0]
            out.append(f"       (weak, {weight}) #{number}: {test} ~ {sorted(files)[0]}")
        return out + batch_notes(result) + census_notes(result)

    ranked = sorted(
        result.scores.items(), key=lambda kv: (-result.strength[kv[0]], kv[0])
    )
    for number, per_test in ranked:
        out.append(
            f"  #{number} owns {len(per_test)} failing test(s), "
            f"match strength {result.strength[number]}:"
        )
        for test, (weight, files) in list(per_test.items())[:4]:
            out.append(f"      [{weight:>3}] {test}  <- {sorted(files)[0]}")
    if len(result.contenders) > 1:
        out.append(
            "  => CO-OWNERS: "
            + ", ".join(f"#{n}" for n in result.contenders)
            + " own it at equal strength. Fix every one; picking"
        )
        out.append("     between equals would be a guess, not a finding.")
    else:
        out.append(
            f"  => CULPRIT: #{result.culprit}. Do not re-arm it until fixed; it "
            "fails every batch it joins."
        )
    return out + batch_notes(result) + census_notes(result)


# ---------------------------------------------------------------------------
# Certification: the narrow question Shipyard's queue-arm-guard asks.
#
# The guard invokes this with `--certify --repo <o/n> --pr <n> --run-id <id>`
# and reads one JSON object off stdout. It treats the head as un-implicated only
# when `implicates_head` is exactly false AND `verdict` positively names why --
# `infrastructure` or `other_pull_request`. A verdict recording what was *not*
# found certifies nothing, by the guard's design and this module's.
# ---------------------------------------------------------------------------

# Certifying (a positive account of a cause that cannot be this head).
VERDICT_INFRASTRUCTURE = "infrastructure"
VERDICT_OTHER_PR = "other_pull_request"
# Non-certifying. `implicated` is positive evidence the head IS at fault;
# `unexplained` is the fail-closed default and the verdict most runs get.
VERDICT_IMPLICATED = "implicates_head"
VERDICT_UNEXPLAINED = "unexplained"

JOB_LOG_RE = re.compile(r"^\d+_(.+)\.txt$")

# A failing step is a candidate for "not this head" only when its ACTION is a
# package-manager fetch, an artifact move, a cache warm, or a runner-generated
# step. Every pattern is anchored and grounded in a step observed failing on a
# real ejecting batch -- `Install ccache (macOS)` (a `brew install`), `Install
# visual-analysis Python dependencies` (a PyPI 403 through the relay), `Upload
# exact GPU-audio SDK (macOS ARM64)` (`actions/upload-artifact`).
#
# Anchoring is the safety property, not decoration. An unanchored "install"
# would swallow a step that runs `cmake --install` over the tree, which builds
# and installs repository content and can fail because of the head. Anything
# unrecognised is content, so a new step name fails closed rather than opening a
# hole nobody reviewed.
#
# `Hydrate bounded GPU provenance commits` is deliberately ABSENT even though it
# is one of the observed failures. It runs `python3
# tools/scripts/hydrate_gpu_provenance_commits.py`: a repository script, over
# inputs this module cannot enumerate, so a head can break it and no pattern
# here could tell. Matching a name is never the whole test -- see
# `touches_runnable_ci_surface`, which every match is also conditioned on.
NON_CONTENT_STEP_PATTERNS = (
    re.compile(r"^install\s+ccache\b", re.I),
    re.compile(r"^install\b.*\bdependenc(?:y|ies)\b", re.I),
    re.compile(r"^(?:upload|download)\b", re.I),
    re.compile(r"^(?:restore|save)\b.*\bcache\b", re.I),
    re.compile(r"^checkout\b", re.I),
    re.compile(r"^set\s*up\s+job$", re.I),
    # `Set up wasi-sdk (pinned)`: a curl of a pinned external release plus a tar
    # extract, which ejected a pull request from the WebCLAP gate.
    re.compile(r"^set\s*up\b.*\b(?:sdk|toolchain)\b", re.I),
    re.compile(r"^complete\s+job$", re.I),
    re.compile(r"^post\s+run\b", re.I),
    re.compile(r"^free\s+disk\s+space\b", re.I),
)

# Nothing in a Pulp CI step is unconditionally beyond the reach of a diff: a
# workflow step IS repository content, and a head can rewrite the step that
# failed. So a step name is only half the test. The other half is per step class:
# which paths, if the head changed them, could have made THAT step fail.
#
# Scoped per class rather than globally, because a global surface is how this
# rule becomes decoration. A blanket list including `CMakeLists.txt` refuses
# every pull request that bumps a version -- while a `curl` of a pinned wasi-sdk
# release plainly cannot fail because a version string moved. The bound has to be
# what the step actually reads.
#
# Every allowlisted step is a step the workflow declares, so rewriting the
# workflow or a local composite action can break any of them.
WORKFLOW_SURFACE_DIRS = (".github/workflows/", ".github/actions/")

# A dependency install additionally reads the configured build tree: `Install
# visual-analysis Python dependencies` resolves its interpreter out of
# `$PULP_BUILD_DIR/CMakeCache.txt` and exits non-zero when the entry is missing,
# which a build-system change can cause.
BUILD_SURFACE_FILES = ("CMakeLists.txt",)
BUILD_SURFACE_SUFFIXES = (".cmake",)
BUILD_SENSITIVE_STEP_PATTERNS = (
    re.compile(r"^install\b.*\bdependenc(?:y|ies)\b", re.I),
)


def head_reaches_step(step: str, changes: list[ChangedFile]) -> str | None:
    """The first changed path that could have made `step` fail, or None.

    None is the only answer that lets a non-content step explain anything, so an
    unreadable diff must never reach here -- the caller refuses that case before
    asking, because "no paths were read" would otherwise look exactly like "no
    path can reach it".
    """
    build_sensitive = any(p.search(step.strip()) for p in BUILD_SENSITIVE_STEP_PATTERNS)
    for change in changes:
        path = posixpath.normpath(change.path)
        if path.startswith(WORKFLOW_SURFACE_DIRS):
            return change.path
        if not build_sensitive:
            continue
        if path.rsplit("/", 1)[-1] in BUILD_SURFACE_FILES:
            return change.path
        if path.endswith(BUILD_SURFACE_SUFFIXES):
            return change.path
    return None


# Steps that RUN the suite, so a ctest failure block in their job's log is the
# account of what broke. A reporting step that only surfaces an already-recorded
# failure counts too: it fails as a consequence of the same block.
TEST_STEP_PATTERNS = (
    re.compile(r"^test\b", re.I),
    re.compile(r"^(?:run\s+)?ctest\b", re.I),
    re.compile(r"^surface\s+ctest\s+failures\b", re.I),
)


def non_content_step(step: str) -> bool:
    """Can this step fail for a reason repository content cannot cause?"""
    name = step.strip()
    return bool(name) and any(p.search(name) for p in NON_CONTENT_STEP_PATTERNS)


def test_step(step: str) -> bool:
    """Does this step run or report the ctest suite?"""
    name = step.strip()
    return bool(name) and any(p.search(name) for p in TEST_STEP_PATTERNS)


@dataclass(frozen=True)
class StepFailure:
    """One failing step of one failing job.

    `step` is empty when the job failed with no step recorded as failing -- a
    lost runner, a mid-flight cancellation, or simply step data the API did not
    return. That is an absence, not a cause, so it is never an explanation.
    """

    job: str
    step: str = ""


def parse_failing_steps(jobs: list[dict]) -> list[StepFailure]:
    """Every failing step of every FAILING job.

    Scoped to `conclusion == "failure"`: a merge_group run is mostly `skipped`
    jobs whose conclusion is merely not "success", and reading those as failures
    would manufacture unexplained steps for every run.
    """
    failures: list[StepFailure] = []
    for job in jobs:
        if not isinstance(job, dict) or job.get("conclusion") != "failure":
            continue
        name = str(job.get("name") or "")
        steps = [
            str(step.get("name") or "")
            for step in (job.get("steps") or [])
            if isinstance(step, dict) and step.get("conclusion") == "failure"
        ]
        if not steps:
            failures.append(StepFailure(job=name))
            continue
        failures += [StepFailure(job=name, step=step) for step in steps]
    return failures


def failing_jobs(repo: str, run_id: str) -> list[dict] | None:
    """A run's jobs, or None when they could not be read.

    None and [] must stay distinguishable: an unreadable jobs list scored as an
    empty one would report "nothing failed" for a run that ejected a pull
    request, which certifies a head on a measurement that never happened.
    """
    raw = gh(f"repos/{repo}/actions/runs/{run_id}/jobs?per_page=100")
    if raw is None:
        return None
    try:
        payload = json.loads(raw)
    except ValueError:
        return None
    jobs = payload.get("jobs") if isinstance(payload, dict) else None
    return jobs if isinstance(jobs, list) else None


@dataclass
class Explanation:
    """Why each failing step cannot be the head's -- or that it could be."""

    reasons: list[str] = field(default_factory=list)
    unexplained: list[StepFailure] = field(default_factory=list)
    implicated_pr: int | None = None
    head_owns_a_test: bool = False
    # Why the head's content is within reach of a CI step, when it is. None means
    # it is out of reach, which is what the non-content-step rule needs.
    reachable: str | None = None


def explain_failures(
    pr: int,
    failures: list[StepFailure],
    ctest: Attribution | None,
    head_changes: list[ChangedFile] | None = None,
) -> Explanation:
    """Account for every failing step, or record that one is unaccounted for.

    Exhaustive on purpose. Explaining the steps that happen to be explainable
    and staying quiet about the rest is how "no test failure was found" becomes
    "infrastructure" -- the reading that re-queues a head whose own compile
    error ejected the batch.
    """
    out = Explanation()
    # An unreadable or empty diff is not an innocent diff. A queued pull request
    # always changes something, so an empty list means the read failed, and
    # scoring it as "reaches nothing" would certify on a measurement that never
    # happened.
    if not head_changes:
        out.reachable = "the head's own diff could not be read"
    if not failures:
        # Nothing observed is not the same as nothing wrong. A run that ejected a
        # pull request failed; a reading that finds no failing step measured the
        # wrong thing, so it must not become a certification.
        out.unexplained.append(StepFailure(job="<no failing job was read>"))
        return out

    owner: int | None = None
    if ctest is not None:
        if pr in ctest.scores:
            out.head_owns_a_test = True
        elif (
            ctest.verdict == VERDICT_CULPRIT
            and len(ctest.contenders) == 1
            and ctest.culprit is not None
            and ctest.culprit != pr
        ):
            owner = ctest.culprit

    for failure in failures:
        if non_content_step(failure.step) and head_changes:
            within = head_reaches_step(failure.step, head_changes)
            if within is None:
                out.reasons.append(
                    f"{failure.job}/{failure.step} fetches a tool, moves an "
                    f"artifact, or is runner-generated, and #{pr} changes nothing "
                    "that step reads"
                )
                continue
            out.reachable = (
                f"#{pr} changes {within}, which {failure.step} reads"
            )
        if test_step(failure.step) and owner is not None and not out.head_owns_a_test:
            out.reasons.append(
                f"{failure.job}/{failure.step} failed on ctest cases owned by "
                f"#{owner}: {', '.join(sorted(ctest.scores[owner]))}"
            )
            out.implicated_pr = owner
            continue
        out.unexplained.append(failure)
    return out


@dataclass
class Certification:
    """The guard's contract, as data."""

    run_id: int
    pr: int
    verdict: str
    implicates_head: bool | None
    evidence: str
    implicated_pr: int | None = None
    # The chain rule's reading of the batch (`ChainVerdict.as_json`), when one
    # was made. Additive: the guard reads only the fields above.
    chain: dict | None = None

    def as_json(self) -> dict:
        payload: dict = {
            "run_id": self.run_id,
            "pr": self.pr,
            "verdict": self.verdict,
            "implicates_head": self.implicates_head,
            "evidence": self.evidence,
        }
        if self.implicated_pr is not None:
            payload["implicated_pr"] = self.implicated_pr
        if self.chain is not None:
            payload["chain"] = self.chain
        return payload


def certify(run_id: int, pr: int, explanation: Explanation) -> Certification:
    """Turn an accounting of failing steps into the guard's verdict.

    `implicates_head` is `false` only when every failing step was accounted for.
    It is `true` only on positive evidence the head is at fault, and `null`
    otherwise -- because "we could not tell" is neither, and the guard refuses a
    null exactly as it refuses a true.
    """
    if explanation.head_owns_a_test:
        return Certification(
            run_id=run_id,
            pr=pr,
            verdict=VERDICT_IMPLICATED,
            implicates_head=True,
            evidence=(
                f"#{pr}'s own diff owns failing ctest cases in this batch, so the "
                "batch implicates it."
            ),
        )
    if explanation.unexplained:
        unaccounted = ", ".join(
            f"{f.job}/{f.step}" if f.step else f"{f.job} (no failing step recorded)"
            for f in explanation.unexplained
        )
        return Certification(
            run_id=run_id,
            pr=pr,
            verdict=VERDICT_UNEXPLAINED,
            implicates_head=None,
            evidence=(
                f"{len(explanation.unexplained)} failing step(s) are unaccounted "
                f"for, so nothing rules #{pr} out: {unaccounted}."
                + (
                    f" Also {explanation.reachable}."
                    if explanation.reachable
                    else ""
                )
                + " A step that compiles or runs repository content can fail "
                "because of this head, and no evidence here says it did not."
            ),
        )
    accounted = "; ".join(explanation.reasons)
    if explanation.implicated_pr is not None:
        return Certification(
            run_id=run_id,
            pr=pr,
            verdict=VERDICT_OTHER_PR,
            implicates_head=False,
            evidence=(
                f"Every failing step is accounted for and none by #{pr}: {accounted}."
            ),
            implicated_pr=explanation.implicated_pr,
        )
    return Certification(
        run_id=run_id,
        pr=pr,
        verdict=VERDICT_INFRASTRUCTURE,
        implicates_head=False,
        evidence=f"Every failing step is accounted for and none by #{pr}: {accounted}.",
    )


def chain_blocker(
    pr: int,
    chain: ChainVerdict,
    jobs: list[dict] | None,
    required: frozenset[str] | None,
    head_changes: list[ChangedFile] | None,
) -> str | None:
    """Why a chain verdict that clears the head must not certify it, or None.

    The chain rule compares only the `macos` gate, so another failing REQUIRED
    job is outside what it measured. And a head whose own diff owns one of the
    failing tests is not cleared by its parent having failed the same test: both
    can break it.
    """
    if jobs is None:
        return "the run's jobs could not be read"
    if not head_changes:
        return f"#{pr}'s own diff could not be read"
    for failure in parse_failing_steps(jobs):
        if failure.job == REQUIRED_JOB:
            continue
        if required is None or failure.job in required:
            return (
                f"required job {failure.job!r} also failed, which the chain rule "
                "does not compare"
                if required is not None
                else "the required contexts could not be read and "
                f"{failure.job!r} also failed"
            )
    for test in chain.failed:
        for change in head_changes:
            if score_match(test, change.path) >= CONFIDENT:
                return f"#{pr}'s own diff owns failing test {test} ({change.path})"
    return None


def certify_with_chain(
    base: Certification,
    chain: ChainVerdict | None,
    jobs: list[dict] | None,
    required: frozenset[str] | None,
    head_changes: list[ChangedFile] | None,
) -> Certification:
    """Let the chain rule decide when it can, else keep the step-level verdict.

    The chain rule only speaks about the entry the group is named for; for any
    other pull request the step-level accounting stands.
    """
    if chain is None:
        return base
    base.chain = chain.as_json()
    if chain.pr != base.pr or chain.implicates_head is None:
        return base
    if base.implicates_head is False:
        # Every failing step was already positively accounted for (a tool
        # fetch, an artifact move): that is stronger than any ancestry reading.
        return base
    if chain.implicates_head:
        return Certification(
            run_id=base.run_id,
            pr=base.pr,
            verdict=VERDICT_IMPLICATED,
            implicates_head=True,
            evidence=f"Chain rule: {chain.evidence}.",
            chain=base.chain,
        )
    blocker = chain_blocker(base.pr, chain, jobs, required, head_changes)
    if blocker is None and chain.classification == CHAIN_NEIGHBOUR:
        if chain.implicated_pr is None or chain.implicated_pr == base.pr:
            blocker = "the chain names no other pull request that owns the break"
    if blocker is not None:
        base.evidence = (
            f"{base.evidence} The chain rule read {chain.classification} "
            f"({chain.evidence}), but {blocker}."
        )
        return base
    if chain.classification == CHAIN_NEIGHBOUR:
        return Certification(
            run_id=base.run_id,
            pr=base.pr,
            verdict=VERDICT_OTHER_PR,
            implicates_head=False,
            evidence=f"Chain rule: {chain.evidence}.",
            implicated_pr=chain.implicated_pr,
            chain=base.chain,
        )
    return Certification(
        run_id=base.run_id,
        pr=base.pr,
        verdict=VERDICT_INFRASTRUCTURE,
        implicates_head=False,
        evidence=f"Chain rule ({chain.classification}): {chain.evidence}.",
        chain=base.chain,
    )


def certification_for(repo: str, run_id: str, pr: int, source_root: Path) -> Certification:
    """Read a failed batch and rule on whether it implicates `pr`."""
    chain = chain_verdict_for(repo, run_id)
    jobs = failing_jobs(repo, run_id)
    head_changes = pr_files(repo, pr)
    required = required_contexts(repo) if chain is not None else None
    return certify_with_chain(
        step_certification(repo, run_id, pr, source_root, jobs, head_changes),
        chain,
        jobs,
        required,
        head_changes,
    )


def step_certification(
    repo: str,
    run_id: str,
    pr: int,
    source_root: Path,
    jobs: list[dict] | None,
    head_changes: list[ChangedFile] | None,
) -> Certification:
    """The per-failing-step accounting, which rules whenever the chain rule cannot."""
    if jobs is None:
        return Certification(
            run_id=int(run_id),
            pr=pr,
            verdict=VERDICT_UNEXPLAINED,
            implicates_head=None,
            evidence=(
                f"the jobs of run {run_id} could not be read, so no failing step "
                "was measured at all."
            ),
        )
    failures = parse_failing_steps(jobs)
    ctest: Attribution | None = None
    if any(test_step(failure.step) for failure in failures):
        tests = failing_tests(repo, run_id)
        if tests:
            members = batch_members(repo, run_id)
            opened = open_prs(repo)
            in_batch = opened if members is None else [n for n in opened if n in members]
            ctest = attribute(
                tests,
                {n: pr_files(repo, n) for n in in_batch},
                census_roots=census_include_roots(source_root),
                batch_members=members,
            )
    return certify(
        int(run_id),
        pr,
        explain_failures(pr, failures, ctest, head_changes),
    )


# --------------------------------------------------------------------------
# Membership history: who separates the failing batches from the passing ones.
# --------------------------------------------------------------------------
#
# File ownership is blind to a pull request that breaks a test it never names:
# a change to a node header that moves an ABI size, or a new test file a tier
# audit has not heard of. The queue's own history is not. A merge_group batch is
# a controlled experiment -- the same base plus a known set of entries -- and
# over a few hours the queue runs many of them with overlapping memberships. The
# entry whose presence exactly separates the batches that failed a test from the
# batches that ran it green is the culprit, whatever its diff looks like.
#
# The rule is deliberately exact. One passing batch that contained the entry
# clears it, and one failing batch without it clears it, because either is a
# direct observation that the entry is neither necessary nor sufficient. Only
# groups inside the span the entry was queued for count: the same test red on
# main hours before the entry existed is a different break. A test
# that no entry separates, failing at a low rate across unrelated memberships or
# rescued by a retry, is a flake and is named as one rather than pinned on
# whichever entry it happened to eject.

# The job whose ctest results decide the merge.
REQUIRED_JOB = "macos"

# A separator seen in only one failing batch is reported but never named: one
# ejection is equally explained by a flake that happened to land there.
MIN_SEPARATING_FAILURES = 2

# At or below this failure rate, among batches that ran the test, a test with
# no separating entry reads as a flake rather than a break.
FLAKE_MAX_RATE = 0.34

HISTORY_CULPRIT = "culprit"
HISTORY_AMBIGUOUS = "ambiguous"
HISTORY_WEAK = "weak"
HISTORY_FLAKE = "flake"
HISTORY_UNATTRIBUTED = "unattributed"
HISTORY_INPUTS = "touched-inputs"

# Declared inputs for script-driven ctests (tools/scripts/script_test_inputs.py).
SCRIPT_INPUTS_LIST = "test/ctest_script_inputs.json"
# The drift check compares every listed script's inputs, so any of them, or
# the list itself, is an input to it.
DRIFT_TEST = "script-test-inputs-drift"

# One ctest result line, as ctest prints it during the run (padded with dots).
CTEST_RESULT_RE = re.compile(
    r"Test\s+#\d+:\s+(?P<name>.+?)\s+\.+\s*"
    r"(?P<result>Passed|\*\*\*Failed|\*\*\*Timeout|\*\*\*Exception|Subprocess aborted)"
)


@dataclass(frozen=True)
class GroupObservation:
    """What one merge_group batch showed about its tests.

    `passed is None` means the required job's test step succeeded, so every test
    it runs passed; otherwise only the tests seen passing count as run, because a
    failed test step can stop before most of the suite ran, and a test that never
    ran is no evidence either way.
    """

    run_id: str
    members: frozenset[int] | None
    host: str = ""
    created: str = ""
    failed: frozenset[str] = frozenset()
    passed: frozenset[str] | None = None
    retried: frozenset[str] = frozenset()
    # Required contexts that concluded on this group, when read; failures
    # outside the ctest suite are named `[context] step` and ran wherever their
    # context concluded.
    required_ran: frozenset[str] | None = None
    # Reds outside the required set. The queue never ejects on these.
    advisory_failed: frozenset[str] = frozenset()
    # Pull request -> the head sha it entered this group at, when read.
    heads: tuple[tuple[int, str], ...] = ()

    def entries(self) -> frozenset[tuple[int, str]]:
        """Each member as (pull request, head); head is "" when unread."""
        head = dict(self.heads)
        return frozenset((pr, head.get(pr, "")) for pr in self.members or ())

    def ran(self, test: str) -> bool:
        if test in self.failed:
            return True
        context = item_context(test)
        if context is not None:
            return self.required_ran is not None and context in self.required_ran
        return self.passed is None or test in self.passed


@dataclass
class Separator:
    pr: int
    with_fail: int
    with_pass: int
    without_fail: int
    alone_fail: int
    head: str = ""
    failed_runs: frozenset[str] = frozenset()

    @property
    def exact(self) -> bool:
        return self.with_fail > 0 and self.with_pass == 0 and self.without_fail == 0


@dataclass
class TestHistory:
    test: str
    failed_runs: list[str] = field(default_factory=list)
    ran_runs: list[str] = field(default_factory=list)
    retried_runs: list[str] = field(default_factory=list)
    unknown_membership_runs: list[str] = field(default_factory=list)
    separators: list[Separator] = field(default_factory=list)
    verdict: str = HISTORY_UNATTRIBUTED
    # One per episode: the same test can break twice for unrelated reasons in
    # one window, and each break has its own culprit.
    culprits: list[int] = field(default_factory=list)

    @property
    def culprit(self) -> int | None:
        return self.culprits[0] if len(self.culprits) == 1 else None

    @property
    def failure_rate(self) -> float:
        return len(self.failed_runs) / len(self.ran_runs) if self.ran_runs else 0.0

    def as_json(self) -> dict:
        return {
            "test": self.test,
            "verdict": self.verdict,
            "culprits": self.culprits,
            "failed_runs": self.failed_runs,
            "ran": len(self.ran_runs),
            "retried_runs": self.retried_runs,
            "unknown_membership_runs": self.unknown_membership_runs,
            "separators": [
                {
                    "pr": s.pr,
                    "with_fail": s.with_fail,
                    "with_pass": s.with_pass,
                    "without_fail": s.without_fail,
                    "alone_fail": s.alone_fail,
                }
                for s in self.separators
            ],
        }


def parse_ctest_results(log: str) -> tuple[frozenset[str], frozenset[str], frozenset[str]]:
    """(final failures, tests seen passing, tests that failed then passed).

    The final "tests FAILED" block is the verdict; a failure line for a test that
    is absent from it was a retry that passed, which is flake evidence.
    """
    final = frozenset(parse_failing_tests(log))
    passed: set[str] = set()
    failed_once: set[str] = set()
    for match in CTEST_RESULT_RE.finditer(log):
        name = match.group("name").strip()
        if match.group("result") == "Passed":
            passed.add(name)
        else:
            failed_once.add(name)
    retried = frozenset((failed_once & passed) - final)
    return final, frozenset(passed - final), retried


def history_attribution(
    observations: list[GroupObservation], tests: list[str] | None = None
) -> list[TestHistory]:
    """For each failing test, the entries whose presence separates its batches."""
    if tests is None:
        names: list[str] = []
        for obs in observations:
            for name in sorted(obs.failed | obs.retried):
                if name not in names:
                    names.append(name)
        tests = names
    observations = sorted(observations, key=chronological)
    candidates = sorted({key for obs in observations for key in obs.entries()})
    # An entry can only cause a failure while it is being validated, so a failure
    # outside the span of groups it appeared in is a different episode (main red
    # before it was queued, another entry after it left) and does not clear it.
    # An entry is a pull request AT ONE HEAD: the fixed head that later merges
    # green is a different entry, and must not clear the broken one.
    span: dict[tuple[int, str], tuple[int, int]] = {}
    for position, obs in enumerate(observations):
        for key in obs.entries():
            first, _ = span.get(key, (position, position))
            span[key] = (first, position)
    histories: list[TestHistory] = []
    for test in tests:
        hist = TestHistory(test=test)
        known: list[GroupObservation] = []
        for position, obs in enumerate(observations):
            if test in obs.retried:
                hist.retried_runs.append(obs.run_id)
            if not obs.ran(test):
                continue
            hist.ran_runs.append(obs.run_id)
            if test in obs.failed:
                hist.failed_runs.append(obs.run_id)
            if obs.members is None:
                hist.unknown_membership_runs.append(obs.run_id)
            else:
                known.append((position, obs))
        for key in candidates:
            pr, head = key
            sep = Separator(
                pr=pr, head=head, with_fail=0, with_pass=0, without_fail=0, alone_fail=0
            )
            first, last = span[key]
            for position, obs in known:
                if not first <= position <= last:
                    continue
                failed = test in obs.failed
                entries = obs.entries()
                present = key in entries
                if present and failed:
                    sep.with_fail += 1
                    sep.failed_runs = sep.failed_runs | {obs.run_id}
                    if entries == {key}:
                        sep.alone_fail += 1
                elif present:
                    sep.with_pass += 1
                elif failed:
                    sep.without_fail += 1
            if sep.exact:
                hist.separators.append(sep)
        hist.separators.sort(key=lambda s: (-s.with_fail, -s.alone_fail, s.pr, s.head))
        classify_history(hist)
        histories.append(hist)
    return histories


def chronological(obs: GroupObservation) -> tuple[str, int]:
    """Creation time, then run id, which GitHub assigns in increasing order."""
    return (obs.created, int(obs.run_id) if obs.run_id.isdigit() else 0)


def classify_history(hist: TestHistory) -> None:
    strong = [s for s in hist.separators if s.with_fail >= MIN_SEPARATING_FAILURES]
    if strong:
        # A separator whose failing groups another separator's strictly contain
        # is dominated: the wider one explains everything it does and more (a
        # later group holding only the wider entry). What remains is one set of
        # failing groups per episode. Entries that always travelled together tie
        # on that set and picking one is a coin toss, so a tied or overlapping
        # episode names nobody; disjoint episodes each name their own culprit.
        dominant = [
            s for s in strong if not any(o.failed_runs > s.failed_runs for o in strong)
        ]
        episodes: dict[frozenset[str], list[int]] = {}
        for sep in dominant:
            if sep.pr not in episodes.setdefault(sep.failed_runs, []):
                episodes[sep.failed_runs].append(sep.pr)
        clean = [
            runs
            for runs in episodes
            if all(other is runs or not (other & runs) for other in episodes)
        ]
        hist.culprits = sorted(
            {episodes[runs][0] for runs in clean if len(episodes[runs]) == 1}
        )
        hist.verdict = HISTORY_CULPRIT if hist.culprits else HISTORY_AMBIGUOUS
        return
    if not hist.failed_runs:
        hist.verdict = HISTORY_FLAKE if hist.retried_runs else HISTORY_UNATTRIBUTED
        return
    if hist.retried_runs or hist.failure_rate <= FLAKE_MAX_RATE:
        hist.verdict = HISTORY_FLAKE
    elif hist.separators:
        hist.verdict = HISTORY_WEAK
    else:
        hist.verdict = HISTORY_UNATTRIBUTED


def declared_inputs(test: str, document: dict) -> tuple[str, ...] | None:
    """The checkout paths (files or directories) a script-driven test reads."""
    tests = document.get("tests") or {}
    if test == DRIFT_TEST:
        inputs = {SCRIPT_INPUTS_LIST}
        for entry in tests.values():
            inputs.update(entry.get("inputs") or [])
        return tuple(sorted(inputs))
    entry = tests.get(test)
    return tuple(entry.get("inputs") or []) if entry else None


def touches(paths: Iterable[str], inputs: tuple[str, ...]) -> bool:
    return any(p == i or p.startswith(i.rstrip("/") + "/") for p in paths for i in inputs)


def attribute_by_inputs(
    histories: list[TestHistory],
    observations: list[GroupObservation],
    document: dict,
    files_of: Callable[[int], list[str]],
) -> None:
    """Name the entries that touched a failing test's declared inputs.

    A test that failed in a few groups and passed in the rest reads as a flake
    to the separator rule when the groups that broke it held DIFFERENT pull
    requests. When every failing group holds at least one entry whose diff
    touched that test's declared inputs, the failure is deterministic and
    those entries are its cause, so they are named instead of a flake. A group
    with no such entry, or one whose membership is unknown, keeps the old
    verdict: then the inputs cannot explain every failure.
    """
    by_run = {obs.run_id: obs for obs in observations}
    cache: dict[int, list[str]] = {}

    def files(pr: int) -> list[str]:
        if pr not in cache:
            cache[pr] = files_of(pr)
        return cache[pr]

    for hist in histories:
        if hist.verdict == HISTORY_CULPRIT or not hist.failed_runs:
            continue
        inputs = declared_inputs(hist.test, document)
        if not inputs:
            continue
        named: set[int] = set()
        for run_id in hist.failed_runs:
            obs = by_run.get(run_id)
            if obs is None or obs.members is None:
                break
            touching = {pr for pr in obs.members if touches(files(pr), inputs)}
            if not touching:
                break
            named |= touching
        else:
            hist.verdict = HISTORY_INPUTS
            hist.culprits = sorted(named)


def likely_culprits(histories: list[TestHistory]) -> dict[int, list[str]]:
    """Pull request -> the tests the history names it the culprit for."""
    named: dict[int, list[str]] = {}
    for hist in histories:
        if hist.verdict in (HISTORY_CULPRIT, HISTORY_INPUTS):
            for pr in hist.culprits:
                named.setdefault(pr, []).append(hist.test)
    return named


def render_history(histories: list[TestHistory]) -> list[str]:
    if not histories:
        return ["no failing test in the observed merge groups"]
    lines: list[str] = []
    for hist in histories:
        lines.append(
            f"{hist.test}: {hist.verdict}"
            + ("".join(f" -> #{pr}" for pr in hist.culprits))
            + f"  (failed {len(hist.failed_runs)} of {len(hist.ran_runs)} groups that ran it"
            + (f", retry-rescued in {len(hist.retried_runs)}" if hist.retried_runs else "")
            + ")"
        )
        for sep in hist.separators:
            lines.append(
                f"    #{sep.pr}: in {sep.with_fail} failing group(s)"
                f" ({sep.alone_fail} alone), 0 passing with it, 0 failing without it"
            )
        if hist.unknown_membership_runs:
            lines.append(
                "    membership unreadable for: "
                + ", ".join(hist.unknown_membership_runs)
            )
    return lines


# First-parent steps read per group: deeper than any batch the queue builds.
CHAIN_DEPTH = 12

# The default history window, and the most runs one pass reads. The cap keeps a
# cold pass (no cache) inside the detector workflow's timeout on a busy day.
DEFAULT_HISTORY_SINCE = "24h"
DEFAULT_HISTORY_MAX_RUNS = 60
HISTORY_WORKERS = 6
CACHE_SCHEMA = 4

SINCE_RE = re.compile(r"^(\d+)([mhd])$")


def parse_since(text: str, now: datetime) -> datetime:
    """`24h`, `90m`, `2d`, or an ISO-8601 instant, as an aware UTC datetime."""
    match = SINCE_RE.match(text.strip())
    if match:
        unit = {"m": "minutes", "h": "hours", "d": "days"}[match.group(2)]
        return now - timedelta(**{unit: int(match.group(1))})
    moment = datetime.fromisoformat(text.strip().replace("Z", "+00:00"))
    return moment if moment.tzinfo else moment.replace(tzinfo=timezone.utc)


def members_from_chain(chain: list[tuple], stop: str) -> frozenset[int] | None:
    """The entries of a first-parent chain above `stop`, or None if it never gets there.

    Stopping short of `stop` (a commit that is not a queue merge, or a chain
    read too shallow) is unknown membership, never a partial one: a partial set
    would clear every entry it missed.
    """
    members: set[int] = set()
    for sha, pr, *_ in chain:
        if sha == stop:
            return frozenset(members) if members else None
        members.add(pr)
    return None


def chain_heads(chain: list[tuple]) -> tuple[tuple[int, str], ...]:
    """(pull request, entry head) for every queue merge the chain read."""
    return tuple(
        (int(step[1]), str(step[2])) for step in chain if len(step) > 2 and step[1]
    )


def group_chain(repo: str, head_sha: str, depth: int = CHAIN_DEPTH) -> list[tuple[str, int, str]]:
    """(commit, pull request, entry head) by first parent from a group head, while each is a queue merge.

    Immutable once read, so it is what the cache keeps; which of these entries
    still count as the group's members depends on where main is now.
    """
    chain: list[tuple[str, int, str]] = []
    sha = head_sha.strip()
    for _ in range(depth):
        raw = gh(
            f"repos/{repo}/commits/{sha}",
            '[(.commit.message|split("\\n")[0]),(.parents[0].sha//""),'
            '(.parents[1].sha//"")]|@tsv',
        )
        if not raw or "\t" not in raw:
            break
        subject, parent, entry_head = (raw.split("\t") + ["", ""])[:3]
        merged = MERGED_PR_RE.match(subject.strip())
        if not merged:
            break
        # The queue merge's second parent is the head the entry was queued at.
        chain.append((sha, int(merged.group(1)), entry_head.strip()))
        sha = parent.strip()
    # The first commit below the last merge closes the chain, so a stop at it
    # is recognised.
    chain.append((sha, 0, ""))
    return chain


def group_members(
    repo: str,
    head_branch: str,
    head_sha: str,
    base: str = "main",
    chain: list[tuple] | None = None,
) -> frozenset[int] | None:
    """Every entry a merge_group batch contains, from its commit chain.

    The ref's embedded sha is the commit the entry was stacked on, which is the
    previous entry's group commit rather than the branch base, so a compare from
    it sees only the last entry. The chain is walked by first parent from the
    group head down to its merge base with `base`; each step is one entry's
    "Merge pull request #N" commit. An entry whose group commit later landed is
    part of `base` by then and correctly drops out; a group that itself landed
    is walked down to the commit it was stacked on instead.
    """
    ref = BATCH_BRANCH_RE.match(head_branch.strip())
    if not ref:
        return None
    stop = gh(f"repos/{repo}/compare/{base}...{head_sha}", ".merge_base_commit.sha")
    if not stop:
        return None
    if stop.strip() == head_sha.strip():
        # The group landed, so its whole chain is on `base` now. Every stacked
        # entry below it landed from its own passing group, so this group adds
        # only the entry it is named for: walk down to the commit it was
        # stacked on.
        stop = ref.group(2)
    if chain is None:
        chain = group_chain(repo, head_sha)
    return members_from_chain(chain, stop.strip())


def required_contexts(repo: str, base: str = "main") -> frozenset[str] | None:
    """The status contexts `base` requires, from branch protection and rulesets.

    Read rather than hard-coded: the merge queue ejects on exactly this set, and
    a red outside it (the hosted Linux job) never removes an entry. None when
    neither source could be read.
    """
    names: set[str] = set()
    classic = gh(f"repos/{repo}/branches/{base}/protection/required_status_checks", ".contexts[]")
    if classic:
        names.update(line for line in classic.splitlines() if line)
    rules = gh(
        f"repos/{repo}/rules/branches/{base}",
        '.[]|select(.type=="required_status_checks")'
        "|.parameters.required_status_checks[].context",
    )
    if rules:
        names.update(line for line in rules.splitlines() if line)
    return frozenset(names) or None


JOB_URL_RE = re.compile(r"/actions/runs/(\d+)/job/(\d+)")


def latest_check_runs(repo: str, sha: str) -> dict[str, dict]:
    """Check-run name -> its latest check run on `sha`, across every workflow."""
    raw = gh(
        f"repos/{repo}/commits/{sha}/check-runs?per_page=100",
        ".check_runs[]|@json",
        paginate=True,
    )
    if raw is None:
        raise RuntimeError(f"check runs for {sha} could not be read")
    latest: dict[str, dict] = {}
    for line in raw.splitlines():
        try:
            check = json.loads(line)
        except ValueError:
            continue
        name = str(check.get("name") or "")
        if name and int(check.get("id") or 0) >= int(latest.get(name, {}).get("id") or 0):
            latest[name] = check
    return latest


def failing_step_names(repo: str, job_id: str) -> list[str]:
    raw = gh(
        f"repos/{repo}/actions/jobs/{job_id}",
        '.steps[]|select(.conclusion=="failure")|.name',
    )
    return [line for line in (raw or "").splitlines() if line]


def context_item(context: str, name: str) -> str:
    """A failure outside the macos ctest suite, named by its context."""
    return f"[{context}] {name}" if name else f"[{context}]"


def item_context(item: str) -> str | None:
    return item[1 : item.index("]")] if item.startswith("[") and "]" in item else None


def read_group(repo: str, run: dict, required: frozenset[str]) -> dict:
    """The immutable facts of one completed merge_group run, as a cache record.

    Every REQUIRED context on the group's head is read, whichever workflow
    reported it: the macos job's ctest results by test name, any other required
    failure by its failing step. A red outside the required set is recorded as
    advisory and never as a failure, because the queue does not eject on it.
    `evidence` is false when no required context concluded.
    """
    run_id = str(run.get("id"))
    record: dict = {
        "schema": CACHE_SCHEMA,
        "run_id": run_id,
        "head_branch": str(run.get("head_branch") or ""),
        "head_sha": str(run.get("head_sha") or ""),
        "created": str(run.get("created_at") or ""),
        "evidence": False,
        "required": sorted(required),
        "required_ran": [],
        "advisory_failed": [],
        "failed": [],
        "passed": [],
        "retried": [],
    }
    checks = latest_check_runs(repo, record["head_sha"])
    failed: list[str] = []
    for name, check in sorted(checks.items()):
        conclusion = check.get("conclusion")
        if name not in required:
            if conclusion == "failure":
                record["advisory_failed"].append(name)
            continue
        if conclusion not in ("success", "failure"):
            continue
        record["required_ran"].append(name)
        job = JOB_URL_RE.search(str(check.get("details_url") or ""))
        if name == REQUIRED_JOB and job:
            detail = json.loads(gh(f"repos/{repo}/actions/jobs/{job.group(2)}") or "{}")
            record["host"] = str(detail.get("runner_name") or "")
            if conclusion == "success":
                # A hosted placeholder concludes `macos` green without running
                # the suite; only a green test step means every test passed.
                ran_tests = any(
                    test_step(str(step.get("name") or ""))
                    and step.get("conclusion") == "success"
                    for step in detail.get("steps") or []
                )
                record["passed"] = None if ran_tests else []
        if conclusion == "success":
            continue
        if name == REQUIRED_JOB and job:
            archive = run_log_zip(repo, job.group(1))
            if archive is None:
                raise RuntimeError(f"logs for run {job.group(1)} could not be read")
            tests, passed, retried = parse_ctest_results(
                unpack_job_logs(archive).get(REQUIRED_JOB, "")
            )
            record["passed"] = sorted(passed)
            record["retried"] = sorted(retried)
            if tests:
                failed += sorted(tests)
                continue
        steps = failing_step_names(repo, job.group(2)) if job else []
        failed += [context_item(name, step) for step in steps] or [context_item(name, "")]
    record["failed"] = failed
    record["evidence"] = bool(record["required_ran"])
    if record["evidence"]:
        record["chain"] = [list(step) for step in group_chain(repo, record["head_sha"])]
    return record


def observation_from_record(
    record: dict, members: frozenset[int] | None
) -> GroupObservation:
    passed = record.get("passed")
    return GroupObservation(
        run_id=str(record["run_id"]),
        members=members,
        host=str(record.get("host") or ""),
        created=str(record.get("created") or ""),
        failed=frozenset(record.get("failed") or ()),
        passed=None if passed is None else frozenset(passed),
        retried=frozenset(record.get("retried") or ()),
        required_ran=frozenset(record.get("required_ran") or ()),
        advisory_failed=frozenset(record.get("advisory_failed") or ()),
        heads=chain_heads(record.get("chain") or []),
    )


def default_cache_dir(repo: str) -> Path:
    override = (os.environ.get("PULP_QUEUE_HISTORY_CACHE") or "").strip()
    root = Path(override) if override else Path.home() / ".cache/pulp/queue-history"
    return root / repo.replace("/", "__")


def load_record(cache_dir: Path | None, run_id: str) -> dict | None:
    if cache_dir is None:
        return None
    try:
        record = json.loads((cache_dir / f"{run_id}.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return record if record.get("schema") == CACHE_SCHEMA else None


def store_record(cache_dir: Path | None, record: dict) -> None:
    if cache_dir is None:
        return
    try:
        cache_dir.mkdir(parents=True, exist_ok=True)
        path = cache_dir / f"{record['run_id']}.json"
        tmp = path.with_suffix(".tmp")
        tmp.write_text(json.dumps(record, sort_keys=True), encoding="utf-8")
        tmp.replace(path)
    except OSError:
        pass


@dataclass
class HistoryRead:
    """What one pass over the window saw, including what it could not read."""

    observations: list[GroupObservation] = field(default_factory=list)
    since: str = ""
    listed: int = 0
    from_cache: int = 0
    fetched: int = 0
    unreadable: list[str] = field(default_factory=list)
    capped: bool = False
    required: list[str] = field(default_factory=list)
    required_unread: bool = False


def list_group_runs(repo: str, since: datetime, max_runs: int) -> tuple[list[dict], bool]:
    """Completed merge_group runs created since `since`, newest first, and whether the cap cut it."""
    stamp = since.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    runs: list[dict] = []
    page = 1
    while True:
        raw = gh(
            f"repos/{repo}/actions/workflows/build.yml/runs?event=merge_group"
            f"&status=completed&per_page=100&page={page}&created=%3E%3D{stamp}"
        )
        try:
            batch = json.loads(raw or "{}").get("workflow_runs", [])
        except ValueError:
            batch = []
        for run in batch:
            if len(runs) >= max_runs:
                return runs, True
            runs.append(run)
        if len(batch) < 100:
            return runs, False
        page += 1


def observe_history(
    repo: str,
    since: str = DEFAULT_HISTORY_SINCE,
    max_runs: int = DEFAULT_HISTORY_MAX_RUNS,
    cache_dir: Path | None = None,
    now: datetime | None = None,
) -> HistoryRead:
    """Every merge group in the window as an observation.

    A completed run never changes, so its jobs, ctest results and commit chain
    are cached per run and a repeated pass reads only new runs. Membership is
    not cached: it depends on where main is now.
    """
    start = parse_since(since, now or datetime.now(timezone.utc))
    runs, capped = list_group_runs(repo, start, max_runs)
    read = HistoryRead(since=start.isoformat(), listed=len(runs), capped=capped)
    required = required_contexts(repo)
    if required is None:
        # Unreadable protection: judge the one context known to gate the merge,
        # and say so rather than guess the rest.
        required = frozenset({REQUIRED_JOB})
        read.required_unread = True
    read.required = sorted(required)

    def one(run: dict) -> tuple[dict | None, bool]:
        run_id = str(run.get("id"))
        cached = load_record(cache_dir, run_id)
        # Which reds count depends on the required set, so a record read under
        # another set is stale.
        if cached is not None and cached.get("required") == sorted(required):
            return cached, True
        try:
            record = read_group(repo, run, required)
        except (RuntimeError, ValueError):
            return None, False
        store_record(cache_dir, record)
        return record, False

    def members(record: dict) -> frozenset[int] | None:
        chain = [tuple(step) for step in record.get("chain") or []]
        return group_members(
            repo, record["head_branch"], record["head_sha"], chain=chain or None
        )

    with ThreadPoolExecutor(max_workers=HISTORY_WORKERS) as pool:
        results = list(pool.map(one, runs))
        evidence = []
        for run, (record, hit) in zip(runs, results):
            if record is None:
                read.unreadable.append(str(run.get("id")))
                continue
            read.from_cache += int(hit)
            read.fetched += int(not hit)
            if record.get("evidence"):
                evidence.append(record)
        memberships = list(pool.map(members, evidence))
    read.observations = [
        observation_from_record(record, group)
        for record, group in zip(evidence, memberships)
    ]
    return read


def advisory_only(observations: list[GroupObservation]) -> list[GroupObservation]:
    """Groups red only outside the required set: the queue ejected nobody for these."""
    return [o for o in observations if o.advisory_failed and not o.failed]


def render_advisory(observations: list[GroupObservation]) -> list[str]:
    groups = advisory_only(observations)
    if not groups:
        return []
    lines = ["advisory-only reds (not required, never an ejection cause):"]
    for obs in groups:
        lines.append(f"    {obs.run_id}: " + ", ".join(sorted(obs.advisory_failed)))
    return lines


# --------------------------------------------------------------------------
# Chain ancestry: culprit, neighbour, known flake, or a red main.
# --------------------------------------------------------------------------
#
# A merge_group ref is `gh-readonly-queue/main/pr-<N>-<parent>`, where <parent>
# is the commit the entry was stacked on: main's tip, or the previous entry's
# group commit. That makes each batch a one-entry experiment against its
# parent, which answers the question file ownership cannot:
#
#   * parent passed (it is on main, or its own group's `macos` went green) and
#     this group's `macos` failed -- the one entry this group adds broke it, so
#     the head is the culprit, whatever its diff looks like;
#   * parent failed and every test this group failed, the parent failed too --
#     this group inherited the break, the head is a neighbour, and the owner is
#     the ancestor whose own parent passed;
#   * parent failed but this group failed something the parent did not -- the
#     head added a failure of its own, so it is a culprit again.
#
# Tests on the known-flake list are set aside before any comparison: a flake
# landing in a child but not its parent is not a failure the head added. A
# group whose only failures are known flakes is named as a flake. A group whose
# failures main's own tip also fails is pre-existing on main.
#
# Only the required `macos` gate is compared -- the queue ejects on required
# contexts, and the hosted Linux leg fails on an unrelated census often enough
# that reading it would make every group look like its own culprit.

CHAIN_CULPRIT = "culprit"
CHAIN_NEIGHBOUR = "neighbour"
CHAIN_KNOWN_FLAKE = "known-flake"
CHAIN_PRE_EXISTING = "pre-existing-on-main"
CHAIN_UNKNOWN = "unknown"

KNOWN_FLAKES_PATH = Path(__file__).resolve().parents[2] / "tools/scripts/queue_known_flakes.json"
KNOWN_FLAKES_SCHEMA = "pulp-queue-known-flakes/v1"


def load_known_flakes(path: Path = KNOWN_FLAKES_PATH) -> frozenset[str]:
    """The ctest names treated as known flakes, from the committed list.

    An unreadable or malformed list is an empty one: it can only cost a flake
    certification, never grant one.
    """
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return frozenset()
    if not isinstance(payload, dict) or payload.get("schema") != KNOWN_FLAKES_SCHEMA:
        return frozenset()
    names = {
        str(entry.get("test") or "").strip()
        for entry in payload.get("flakes") or ()
        if isinstance(entry, dict)
    }
    return frozenset(name for name in names if name)


@dataclass(frozen=True)
class GateFacts:
    """The required gate's result on one commit (a group head or a main commit).

    `failed` is the gate's failure signature: its ctest failure names, or, when
    it failed before ctest ran, its failing step names as `[macos] <step>`.
    None means the signature could not be read, which is never an empty one.
    """

    sha: str
    pr: int | None = None
    first_parent: str = ""
    conclusion: str | None = None
    failed: frozenset[str] | None = None
    run_id: str = ""


@dataclass
class ChainVerdict:
    classification: str
    pr: int
    evidence: str
    parent_sha: str = ""
    parent_pr: int | None = None
    parent_state: str = ""
    implicated_pr: int | None = None
    failed: list[str] = field(default_factory=list)
    parent_failed: list[str] = field(default_factory=list)
    flakes: list[str] = field(default_factory=list)

    @property
    def implicates_head(self) -> bool | None:
        if self.classification == CHAIN_CULPRIT:
            return True
        if self.classification in (CHAIN_NEIGHBOUR, CHAIN_KNOWN_FLAKE, CHAIN_PRE_EXISTING):
            return False
        return None

    def as_json(self) -> dict:
        payload: dict = {
            "classification": self.classification,
            "implicates_head": self.implicates_head,
            "evidence": self.evidence,
            "parent_sha": self.parent_sha,
            "parent_state": self.parent_state,
            "failed_tests": self.failed,
            "parent_failed_tests": self.parent_failed,
            "known_flakes": self.flakes,
        }
        if self.parent_pr is not None:
            payload["parent_pr"] = self.parent_pr
        if self.implicated_pr is not None:
            payload["implicated_pr"] = self.implicated_pr
        return payload


class ChainSource:
    """What the chain rule reads. The live source asks GitHub; tests hand in facts."""

    def gate(self, sha: str) -> GateFacts:  # pragma: no cover - interface
        raise NotImplementedError

    def on_main(self, sha: str) -> bool | None:  # pragma: no cover - interface
        raise NotImplementedError

    def main_tip(self) -> str | None:  # pragma: no cover - interface
        raise NotImplementedError


PARENT_PASSED = "passed"
PARENT_FAILED = "failed"


def parent_state(source: ChainSource, parent: str) -> tuple[str, GateFacts | None]:
    """passed / failed / why it is unknown, plus the parent's gate facts."""
    on_main = source.on_main(parent)
    facts = source.gate(parent)
    if on_main:
        return PARENT_PASSED, facts
    if facts.conclusion == "success":
        return PARENT_PASSED, facts
    if facts.conclusion == "failure":
        return PARENT_FAILED, facts
    if on_main is None:
        return "unreadable", facts
    return f"not on main, gate {facts.conclusion or 'absent'}", facts


def suite_tests(signature: frozenset[str]) -> frozenset[str]:
    """The ctest names in a failure signature, without its `[macos] <step>` entries."""
    return frozenset(name for name in signature if item_context(name) != REQUIRED_JOB)


def classify_chain(
    pr: int,
    head_sha: str,
    parent_sha: str,
    source: ChainSource,
    flakes: frozenset[str],
    depth: int = CHAIN_DEPTH,
) -> ChainVerdict:
    """Rule on one failed group from its parent's result and failing tests."""
    head = source.gate(head_sha)
    verdict = ChainVerdict(
        classification=CHAIN_UNKNOWN, pr=pr, evidence="", parent_sha=parent_sha
    )
    if head.conclusion != "failure":
        verdict.evidence = (
            f"the group's {REQUIRED_JOB} gate did not fail "
            f"({head.conclusion or 'no result'}), so the chain rule has nothing to rule on"
        )
        return verdict
    if head.failed is None:
        verdict.evidence = f"the group's {REQUIRED_JOB} failures could not be read"
        return verdict
    verdict.failed = sorted(head.failed)
    verdict.flakes = sorted(head.failed & flakes)
    own = head.failed - flakes
    if head.failed and not own:
        verdict.classification = CHAIN_KNOWN_FLAKE
        verdict.evidence = (
            "every failing test is a known flake: " + ", ".join(verdict.flakes)
        )
        return verdict

    tip = source.main_tip()
    if tip and own:
        tip_gate = source.gate(tip)
        if (
            tip_gate.conclusion == "failure"
            and tip_gate.failed is not None
            and own <= tip_gate.failed
        ):
            verdict.classification = CHAIN_PRE_EXISTING
            verdict.evidence = (
                f"main's tip {tip[:12]} fails the same {REQUIRED_JOB} tests: "
                + ", ".join(sorted(own))
            )
            return verdict

    state, parent = parent_state(source, parent_sha)
    verdict.parent_state = state
    verdict.parent_pr = parent.pr if parent else None
    if state == PARENT_PASSED:
        verdict.classification = CHAIN_CULPRIT
        verdict.evidence = (
            f"the parent {parent_sha[:12]} passed, and this group adds only #{pr}, "
            f"whose group failed: " + (", ".join(sorted(own)) or "no failure named")
        )
        return verdict
    if not own:
        verdict.evidence = (
            f"the parent {parent_sha[:12]} is {state}, and this group's failure "
            "names nothing to compare against it"
        )
        return verdict

    # A parent that failed before its suite ran (a compile error, a lost step)
    # observed none of the tests this group failed, so it can neither clear nor
    # convict the head. Look through it to the nearest ancestor whose suite
    # ran -- but only for a group that failed tests: a group that itself failed
    # at `Build` compares directly with a parent that failed at `Build`.
    skipped: list[GateFacts] = []
    ancestor = parent
    budget = depth
    while (
        state == PARENT_FAILED
        and ancestor is not None
        and ancestor.failed is not None
        and not suite_tests(ancestor.failed)
        and suite_tests(own)
        and ancestor.first_parent
        and budget > 1
    ):
        skipped.append(ancestor)
        budget -= 1
        state, ancestor = parent_state(source, ancestor.first_parent)
    unobserved = ", ".join(f"#{g.pr}" for g in skipped)
    if skipped and state == PARENT_PASSED:
        verdict.evidence = (
            f"the parent {parent_sha[:12]} failed before its suite ran and the "
            f"first ancestor past {unobserved} passed, so the tests this group "
            f"failed ({', '.join(sorted(own))}) may belong to {unobserved} or to #{pr}"
        )
        return verdict
    if state != PARENT_FAILED or ancestor is None:
        verdict.evidence = f"the parent {parent_sha[:12]}'s result is {state}"
        return verdict
    if ancestor.failed is None:
        verdict.evidence = f"the ancestor {ancestor.sha[:12]}'s failures could not be read"
        return verdict
    ancestor_own = ancestor.failed - flakes
    verdict.parent_failed = sorted(ancestor.failed)
    extra = own - ancestor_own
    if extra:
        if skipped:
            verdict.evidence = (
                f"the tests {', '.join(sorted(extra))} fail here but not in "
                f"{ancestor.sha[:12]}, and {unobserved} in between never ran its "
                f"suite, so they may belong to {unobserved} or to #{pr}"
            )
            return verdict
        verdict.classification = CHAIN_CULPRIT
        verdict.evidence = (
            f"the parent {parent_sha[:12]} failed too, but not on "
            + ", ".join(sorted(extra))
            + f", which #{pr} added"
        )
        return verdict

    verdict.classification = CHAIN_NEIGHBOUR
    origin = ancestor.pr
    if budget > 1 and ancestor.first_parent and ancestor.pr is not None:
        above = classify_chain(
            ancestor.pr, ancestor.sha, ancestor.first_parent, source, flakes, budget - 1
        )
        if above.classification == CHAIN_NEIGHBOUR and above.implicated_pr:
            origin = above.implicated_pr
    if origin == pr:
        origin = None
    verdict.implicated_pr = origin
    through = f" (looking through {unobserved}, which never ran its suite)" if skipped else ""
    verdict.evidence = (
        f"the ancestor {ancestor.sha[:12]} (#{ancestor.pr}) failed every test this "
        f"group failed ({', '.join(sorted(own))}){through}, so #{pr} inherited "
        "the break" + (f" from #{origin}" if origin else "")
    )
    return verdict


class LiveChainSource(ChainSource):
    """The chain rule's reads against GitHub, each commit read at most once."""

    def __init__(self, repo: str, base: str = "main") -> None:
        self.repo = repo
        self.base = base
        self._gates: dict[str, GateFacts] = {}
        self._tip: str | None = None
        self._tip_read = False

    def gate(self, sha: str) -> GateFacts:
        if sha not in self._gates:
            self._gates[sha] = self._read_gate(sha)
        return self._gates[sha]

    def _read_gate(self, sha: str) -> GateFacts:
        raw = gh(
            f"repos/{self.repo}/commits/{sha}",
            '[(.commit.message|split("\\n")[0]),(.parents[0].sha//"")]|@tsv',
        )
        pr: int | None = None
        first_parent = ""
        if raw and "\t" in raw:
            subject, first_parent = raw.split("\t", 1)
            merged = MERGED_PR_RE.match(subject.strip())
            pr = int(merged.group(1)) if merged else None
            first_parent = first_parent.strip()
        try:
            check = latest_check_runs(self.repo, sha).get(REQUIRED_JOB)
        except RuntimeError:
            return GateFacts(sha=sha, pr=pr, first_parent=first_parent)
        if not check:
            return GateFacts(sha=sha, pr=pr, first_parent=first_parent)
        conclusion = check.get("conclusion")
        job = JOB_URL_RE.search(str(check.get("details_url") or ""))
        facts = GateFacts(
            sha=sha,
            pr=pr,
            first_parent=first_parent,
            conclusion=conclusion,
            failed=frozenset(),
            run_id=job.group(1) if job else "",
        )
        if conclusion != "failure":
            return facts
        if not job:
            return GateFacts(sha=sha, pr=pr, first_parent=first_parent, conclusion=conclusion)
        archive = run_log_zip(self.repo, job.group(1))
        tests = (
            parse_failing_tests(unpack_job_logs(archive).get(REQUIRED_JOB, ""))
            if archive
            else []
        )
        if tests:
            return GateFacts(**{**facts.__dict__, "failed": frozenset(tests)})
        steps = failing_step_names(self.repo, job.group(2))
        if archive is None and not steps:
            return GateFacts(sha=sha, pr=pr, first_parent=first_parent, conclusion=conclusion)
        return GateFacts(
            **{
                **facts.__dict__,
                "failed": frozenset(context_item(REQUIRED_JOB, s) for s in steps),
            }
        )

    def on_main(self, sha: str) -> bool | None:
        status = gh(f"repos/{self.repo}/compare/{sha}...{self.base}", ".status")
        if status is None:
            return None
        return status.strip() in ("ahead", "identical")

    def main_tip(self) -> str | None:
        if not self._tip_read:
            self._tip_read = True
            tip = gh(f"repos/{self.repo}/commits/{self.base}", ".sha")
            self._tip = tip.strip() if tip else None
        return self._tip


def chain_verdict_for(
    repo: str, run_id: str, source: ChainSource | None = None
) -> ChainVerdict | None:
    """The chain rule for one merge_group run, or None when it is not a queue run."""
    raw = gh(f"repos/{repo}/actions/runs/{run_id}", "[.head_branch,.head_sha]|@tsv")
    if not raw or "\t" not in raw:
        return None
    head_branch, head_sha = (part.strip() for part in raw.split("\t", 1))
    ref = BATCH_BRANCH_RE.match(head_branch)
    if not ref:
        return None
    return classify_chain(
        int(ref.group(1)),
        head_sha,
        ref.group(2),
        source or LiveChainSource(repo),
        load_known_flakes(),
    )


@dataclass
class Ejection:
    """One failed_checks removal of a pull request, and the group that caused it."""

    removed_at: str
    before: str
    run_id: str = ""
    record: dict | None = None


def graphql(query: str, **variables: str) -> dict | None:
    args = ["api", "graphql"]
    for key, value in variables.items():
        args += ["-F", f"{key}={value}"]
    args += ["-f", f"query={query}"]
    proc = run_gh(args, capture_output=True, text=True)
    if proc.returncode != 0:
        return None
    try:
        return json.loads(proc.stdout)
    except ValueError:
        return None


EJECTIONS_QUERY = """
query($owner:String!,$name:String!,$pr:Int!){repository(owner:$owner,name:$name){
pullRequest(number:$pr){timelineItems(last:20,itemTypes:[REMOVED_FROM_MERGE_QUEUE_EVENT]){
nodes{... on RemovedFromMergeQueueEvent{createdAt reason beforeCommit{oid}}}}}}}
"""


def parse_ejections(payload: dict | None) -> list[Ejection]:
    nodes = (
        ((payload or {}).get("data") or {}).get("repository", {}) or {}
    ).get("pullRequest", {}) or {}
    found: list[Ejection] = []
    for node in ((nodes.get("timelineItems") or {}).get("nodes") or []):
        if (node or {}).get("reason") != "failed_checks":
            continue
        before = ((node.get("beforeCommit") or {}).get("oid")) or ""
        if before:
            found.append(Ejection(removed_at=str(node.get("createdAt") or ""), before=before))
    return found


def ejections(repo: str, pr: int) -> list[Ejection]:
    """Every failed_checks removal of `pr`, each tied to the group run that ejected it.

    The removal event's `beforeCommit` is the head of the entry's own merge
    group -- the commit the next entry was stacked on -- so the ejecting run is
    the merge_group run built on exactly that sha, found by identity rather
    than by time.
    """
    owner, name = repo.split("/", 1)
    found = parse_ejections(graphql(EJECTIONS_QUERY, owner=owner, name=name, pr=str(pr)))
    for ejection in found:
        raw = gh(
            f"repos/{repo}/actions/workflows/build.yml/runs?event=merge_group"
            f"&head_sha={ejection.before}",
            ".workflow_runs[0].id",
        )
        ejection.run_id = (raw or "").strip()
    return found


def render_ejection(
    pr: int,
    ejection: Ejection,
    observation: GroupObservation | None,
    histories: dict[str, TestHistory],
) -> list[str]:
    head = f"#{pr} ejected {ejection.removed_at} by merge group {ejection.run_id or '(run not found)'}"
    if observation is None:
        return [head, "    that group is outside the window or proves nothing; widen --since"]
    lines = [head + (f" (members {sorted(observation.members)})" if observation.members else "")]
    if not observation.failed:
        lines.append(
            "    no required context failed on that group, so its checks do not "
            "explain the ejection"
        )
    for item in sorted(observation.failed):
        hist = histories.get(item)
        verdict = hist.verdict if hist else HISTORY_UNATTRIBUTED
        named = "".join(f" #{n}" for n in hist.culprits) if hist else ""
        lines.append(f"    required failure: {item} -> {verdict}{named}")
    if observation.advisory_failed:
        lines.append(
            "    advisory reds, not the cause: "
            + ", ".join(sorted(observation.advisory_failed))
        )
    return lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_id", nargs="?", help="failed merge_group run id")
    parser.add_argument("--repo", default=DEFAULT_REPO)
    parser.add_argument(
        "--run-id",
        dest="run_id_flag",
        help="the same run id as a flag, which is how Shipyard's queue-arm-guard "
        "passes it",
    )
    parser.add_argument(
        "--pr",
        type=int,
        help="the pull request to rule on, required by --certify; with --history, "
        "also explain each of its failed_checks ejections from the group that "
        "caused it",
    )
    parser.add_argument(
        "--certify",
        action="store_true",
        help="emit Shipyard's queue-attribution verdict for --pr as one JSON "
        "object on stdout, and nothing else",
    )
    parser.add_argument(
        "--source-root",
        default=str(Path(__file__).resolve().parents[2]),
        help="checkout whose committed census names the exported include roots",
    )
    parser.add_argument("--format", choices=("text", "json"), default="text")
    parser.add_argument(
        "--history",
        action="store_true",
        help="attribute by batch membership over the recent merge groups: name "
        "the entry whose presence separates the failing batches from the passing "
        "ones, and name flakes as flakes",
    )
    parser.add_argument(
        "--since",
        default=DEFAULT_HISTORY_SINCE,
        help="--history window: 24h, 90m, 2d, or an ISO-8601 instant "
        f"(default {DEFAULT_HISTORY_SINCE})",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=DEFAULT_HISTORY_MAX_RUNS,
        help="most merge_group runs --history reads; a window holding more is "
        f"cut to the newest and says so (default {DEFAULT_HISTORY_MAX_RUNS})",
    )
    parser.add_argument(
        "--no-cache",
        action="store_true",
        help="--history reads every run afresh instead of the per-run cache",
    )
    args = parser.parse_args(argv)

    if args.run_id and args.run_id_flag and args.run_id != args.run_id_flag:
        parser.error(
            f"run id given twice and they disagree: {args.run_id} and "
            f"{args.run_id_flag}"
        )
    given = args.run_id or args.run_id_flag

    if args.certify:
        # The guard reads stdout as JSON, so nothing else may go there, and it
        # reads a non-zero exit as "did not rule" -- which loses the evidence a
        # refusal should carry. So a verdict it can read, even a refusing one, is
        # printed and exits 0; only an unusable invocation exits non-zero.
        if args.pr is None or not given:
            print("--certify needs --pr and a run id", file=sys.stderr)
            return 2
        if not given.isdigit():
            print(f"run id {given!r} is not a number", file=sys.stderr)
            return 2
        verdict = certification_for(
            args.repo, given, args.pr, Path(args.source_root)
        )
        print(json.dumps(verdict.as_json(), indent=2, sort_keys=True))
        return 0

    if args.history:
        read = observe_history(
            args.repo,
            since=args.since,
            max_runs=args.limit,
            cache_dir=None if args.no_cache else default_cache_dir(args.repo),
        )
        observations = read.observations
        focus = None
        if given:
            focus = sorted(
                {t for obs in observations if obs.run_id == given for t in obs.failed}
            )
        histories = history_attribution(observations, focus)
        inputs_list = Path(args.source_root) / SCRIPT_INPUTS_LIST
        if inputs_list.is_file():
            attribute_by_inputs(
                histories, observations, json.loads(inputs_list.read_text(encoding="utf-8")),
                lambda pr: [c.path for c in pr_files(args.repo, pr)])
        explained: list[str] = []
        if args.pr is not None:
            by_run = {obs.run_id: obs for obs in observations}
            by_test = {hist.test: hist for hist in histories}
            for ejection in ejections(args.repo, args.pr):
                explained += render_ejection(
                    args.pr, ejection, by_run.get(ejection.run_id), by_test
                )
        if args.format == "json":
            print(
                json.dumps(
                    {
                        "since": read.since,
                        "required_contexts": read.required,
                        "required_unread": read.required_unread,
                        "advisory_only": {
                            obs.run_id: sorted(obs.advisory_failed)
                            for obs in advisory_only(observations)
                        },
                        "ejections": explained,
                        "listed_groups": read.listed,
                        "capped": read.capped,
                        "from_cache": read.from_cache,
                        "unreadable": read.unreadable,
                        "observed_groups": len(observations),
                        "tests": [h.as_json() for h in histories],
                        "likely_culprits": {
                            str(pr): tests
                            for pr, tests in likely_culprits(histories).items()
                        },
                    },
                    indent=2,
                    sort_keys=True,
                )
            )
        else:
            print(
                f"{read.listed} merge group(s) since {read.since}"
                f" ({read.from_cache} cached), {len(observations)} with test evidence"
            )
            if read.capped:
                print(f"window cut to the newest {read.listed}; raise --limit to read further back")
            if read.unreadable:
                print("unreadable, left out: " + ", ".join(read.unreadable))
            if read.required_unread:
                print("required contexts unreadable; judging macos only")
            else:
                print("required contexts: " + ", ".join(read.required))
            for line in explained:
                print(line)
            print("\n".join(render_history(histories) + render_advisory(observations)))
        return 0

    run_id = given or latest_failed_merge_group(args.repo)
    if not run_id:
        print("no failed merge_group batch found")
        return 0

    tests = failing_tests(args.repo, run_id)
    if not tests:
        print(
            f"run {run_id}: no ctest failure block "
            "(failure is not a test failure)"
        )
        return 0

    members = batch_members(args.repo, run_id)
    opened = open_prs(args.repo)
    # Fetching a diff costs a round trip per pull request, and only a member can
    # own the batch, so narrow before fetching rather than after.
    in_batch = opened if members is None else [n for n in opened if n in members]
    files = {n: pr_files(args.repo, n) for n in in_batch}
    result = attribute(
        tests,
        files,
        census_roots=census_include_roots(Path(args.source_root)),
        batch_members=members,
    )
    result.scoped_out = sorted(set(opened) - set(in_batch))

    if args.format == "json":
        print(
            json.dumps(
                {
                    "run_id": run_id,
                    "verdict": result.verdict,
                    "culprit": result.culprit,
                    "tests": result.tests,
                    "strength": result.strength,
                    "census_tests": result.census_tests,
                    "census_roots_read": result.census_roots_read,
                    "contenders": result.contenders,
                    "batch_members_known": result.batch_members_known,
                },
                indent=2,
                sort_keys=True,
            )
        )
    else:
        print("\n".join(render(result, run_id)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
