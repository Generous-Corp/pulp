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
consumption census is exactly one: `public_headers.count` is walked live from
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
import posixpath
import re
import subprocess
import sys
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

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


def gh(
    path: str,
    jq: str | None = None,
    repo_cwd: str | None = None,
    paginate: bool = False,
) -> str | None:
    cmd = ["ghapp", "api", path]
    if paginate:
        cmd.append("--paginate")
    cmd += ["--jq", jq] if jq else []
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=repo_cwd)
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
    proc = subprocess.run(
        ["ghapp", "api", f"repos/{repo}/actions/runs/{run_id}/logs"],
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
        match = re.search(
            r"\d+\s+-\s+(.+?)\s+\((Failed|Timeout|Subprocess aborted)\)", line
        )
        if match:
            names.append(match.group(1).strip())
        elif "Errors while running CTest" in line:
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


def certification_for(repo: str, run_id: str, pr: int, source_root: Path) -> Certification:
    """Read a failed batch and rule on whether it implicates `pr`."""
    jobs = failing_jobs(repo, run_id)
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
        explain_failures(pr, failures, ctest, pr_files(repo, pr)),
    )


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
        help="the pull request to rule on, required by --certify",
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
