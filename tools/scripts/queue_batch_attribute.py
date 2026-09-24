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

Read-only. Prints findings; mutates nothing.
"""

from __future__ import annotations

import argparse
import json
import posixpath
import re
import subprocess
import sys
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


def failing_tests(repo: str, run_id: str) -> list[str]:
    """ctest names that failed in a run's macos job."""
    jid = gh(
        f"repos/{repo}/actions/runs/{run_id}/jobs?per_page=50",
        '.jobs[]|select(.name=="macos")|.id',
    )
    if not jid:
        return []
    jid = jid.splitlines()[0]
    log = gh(f"repos/{repo}/actions/jobs/{jid}/logs")
    return parse_failing_tests(log or "")


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


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_id", nargs="?", help="failed merge_group run id")
    parser.add_argument("--repo", default=DEFAULT_REPO)
    parser.add_argument(
        "--source-root",
        default=str(Path(__file__).resolve().parents[2]),
        help="checkout whose committed census names the exported include roots",
    )
    parser.add_argument("--format", choices=("text", "json"), default="text")
    args = parser.parse_args(argv)

    run_id = args.run_id or latest_failed_merge_group(args.repo)
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
