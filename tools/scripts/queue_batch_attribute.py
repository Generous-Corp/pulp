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

The threshold matters as much as the mapping. Incidental token overlap scores
low, and reporting a low score as a culprit is a false accusation -- worse than
no attribution, because someone acts on it. Below the confidence threshold this
reports a likely pre-existing break on main instead of naming anyone.

Read-only. Prints findings; mutates nothing.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field

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


def gh(path: str, jq: str | None = None, repo_cwd: str | None = None) -> str | None:
    cmd = ["ghapp", "api", path] + (["--jq", jq] if jq else [])
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


@dataclass
class Attribution:
    """Who owns a batch's failing tests, and how sure we are."""

    tests: list[str]
    # pr number -> test name -> (weight, owning files)
    scores: dict[int, dict[str, tuple[int, set[str]]]] = field(default_factory=dict)
    strength: dict[int, int] = field(default_factory=dict)
    verdict: str = VERDICT_UNOWNED
    culprit: int | None = None

    @property
    def best_strength(self) -> int:
        return max(self.strength.values(), default=0)


def attribute(tests: list[str], pr_files: dict[int, list[str]]) -> Attribution:
    """Map failing tests onto the pull requests whose files own them."""
    result = Attribution(tests=list(tests))
    for test in tests:
        for number, paths in pr_files.items():
            for path in paths:
                weight = score_match(test, path)
                if not weight:
                    continue
                prev_weight, prev_files = result.scores.setdefault(
                    number, {}
                ).setdefault(test, (0, set()))
                result.scores[number][test] = (
                    max(prev_weight, weight),
                    prev_files | {path},
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
        result.culprit = max(result.strength, key=lambda n: result.strength[n])
    return result


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


def pr_files(repo: str, number: int) -> list[str]:
    raw = gh(f"repos/{repo}/pulls/{number}/files?per_page=100", ".[].filename")
    return raw.splitlines() if raw else []


def latest_failed_merge_group(repo: str) -> str:
    raw = gh(
        f"repos/{repo}/actions/workflows/build.yml/runs?event=merge_group&per_page=20",
        '[.workflow_runs[]|select(.conclusion=="failure")]|.[0].id',
    )
    return (raw or "").strip()


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
        return out

    if result.verdict == VERDICT_PRE_EXISTING:
        out.append(
            f"  {len(result.tests)} failing test(s); NO open PR matches any of "
            f"them strongly (best score {result.best_strength})."
        )
        out.append("  => LIKELY PRE-EXISTING ON MAIN. Do not blame a batch member;")
        out.append("     check whether main's own suite is red before re-queueing.")
        ranked = sorted(result.scores.items(), key=lambda kv: -result.strength[kv[0]])
        for number, per_test in ranked[:3]:
            test, (weight, files) = list(per_test.items())[0]
            out.append(f"       (weak, {weight}) #{number}: {test} ~ {sorted(files)[0]}")
        return out

    ranked = sorted(result.scores.items(), key=lambda kv: -result.strength[kv[0]])
    for number, per_test in ranked:
        out.append(
            f"  #{number} owns {len(per_test)} failing test(s), "
            f"match strength {result.strength[number]}:"
        )
        for test, (weight, files) in list(per_test.items())[:4]:
            out.append(f"      [{weight:>3}] {test}  <- {sorted(files)[0]}")
    out.append(
        f"  => CULPRIT: #{result.culprit}. Do not re-arm it until fixed; it "
        "fails every batch it joins."
    )
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_id", nargs="?", help="failed merge_group run id")
    parser.add_argument("--repo", default=DEFAULT_REPO)
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

    files = {n: pr_files(args.repo, n) for n in open_prs(args.repo)}
    result = attribute(tests, files)

    if args.format == "json":
        print(
            json.dumps(
                {
                    "run_id": run_id,
                    "verdict": result.verdict,
                    "culprit": result.culprit,
                    "tests": result.tests,
                    "strength": result.strength,
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
