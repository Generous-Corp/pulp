#!/usr/bin/env python3
"""Withhold an SDK tag while main's MSVC CLI compile is known to be broken.

Used by .github/workflows/auto-release.yml before it creates a `vX.Y.Z` tag.
A tag starts release-cli.yml, whose Windows legs compile the CLI with MSVC; if
that compile is already known to fail, the tag can only produce a release whose
Windows legs fail and whose `release` job is skipped. Withholding the tag keeps
the version untagged instead, which release-cadence-check.yml reports, and the
next push to main re-evaluates: auto-release tags the then-current version once
this check clears.

Evidence: the `Windows CLI compile verdict` check that
.github/workflows/windows-cli-compile.yml posts on main's commits. That job runs
only when the MSVC build step itself ran, and fails only when that step failed,
so a commit whose range could not reach the compile, or whose runner broke
before compiling, carries no verdict (or a skipped one) and is looked through.
Candidates are main's first-parent commits that touched the workflow's push
paths, plus the commits a merge-queue batch could have pushed on top of one
(the run, and so its verdict, lands on the push's head commit, which need not
itself touch those paths); newest first, all ancestors of the tagged commit.

  * Newest verdict FAILED -> block.
  * Newest verdict PASSED -> allow.
  * No verdict, or an unreadable API -> allow, with the reason printed. This
    check narrows when a tag is cut; release-cli.yml stays the authority, so
    missing evidence must not withhold a release.

It reads check runs (`checks: read`), not workflow runs: auto-release.yml holds
no `actions` scope at all, which is what guarantees it cannot cancel a release.

`PULP_RELEASE_WINDOWS_PRECHECK=off` disables the check.

The decision logic is `decide()`, which is pure; `main()` is the I/O shell.
Output: one JSON line on stdout, and `block=1|0`, `reason=` and `run_url=` in
$GITHUB_OUTPUT when running under GitHub Actions. Exit status is always 0.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import urllib.parse
from dataclasses import dataclass
from typing import Callable, Iterable, Optional

WORKFLOW_FILE = "windows-cli-compile.yml"
COMPILE_JOB = "Windows CLI compile (MSVC)"
COMPILE_STEP = "Build CLI targets (MSVC)"
VERDICT_CHECK = "Windows CLI compile verdict"
# The trees windows-cli-compile.yml's push trigger watches, as git pathspecs.
# test_windows_cli_compile_workflow.py keeps the two lists identical.
PUSH_PATHS = (
    "tools/cli",
    "tools/mcp",
    "tools/cmake",
    "core",
    "inspect",
    "ship",
    "tools/audio",
    ".github/workflows/windows-cli-compile.yml",
    "tools/scripts/windows_cli_compile_scope.py",
)
# One check-runs call per candidate commit; this bounds a sweep.
MAX_COMMITS_INSPECTED = 30
# The merge queue lands up to this many PRs in one push (ruleset
# main-merge-queue), so a path-touching merge can sit this far below the head
# commit its push run reports on.
QUEUE_BATCH_MAX = 5
# How much first-parent history to enumerate locally (free) when choosing
# candidates.
HISTORY_WINDOW = 400

BLOCK = "block"
ALLOW = "allow"


@dataclass(frozen=True)
class Verdict:
    action: str
    reason: str
    run_url: str = ""

    def as_json(self) -> str:
        return json.dumps(
            {"action": self.action, "reason": self.reason, "run_url": self.run_url}
        )


def newest_verdict(check_runs: Iterable[dict]) -> Optional[dict]:
    """The newest completed success/failure verdict among a commit's checks."""
    decisive = [
        run
        for run in check_runs
        if run.get("name") == VERDICT_CHECK
        and run.get("status") == "completed"
        and run.get("conclusion") in ("success", "failure")
    ]
    if not decisive:
        return None
    return max(decisive, key=lambda run: run.get("completed_at") or "")


def decide(
    commits: Iterable[str],
    *,
    checks_for: Callable[[str], list[dict]],
    max_commits: int = MAX_COMMITS_INSPECTED,
) -> Verdict:
    """Return the verdict of the newest commit that has one. Pure.

    `commits` must be newest first and contain only ancestors of the commit
    being tagged.
    """
    inspected = 0
    for sha in commits:
        if inspected >= max_commits:
            break
        inspected += 1
        verdict = newest_verdict(checks_for(sha))
        if verdict is None:
            continue
        url = verdict.get("html_url", "")
        if verdict["conclusion"] == "failure":
            return Verdict(
                BLOCK,
                f"the MSVC CLI compile failed at {sha[:12]} and no later commit "
                "compiled cleanly; a tag now would fail release-cli.yml's Windows legs",
                url,
            )
        return Verdict(ALLOW, f"the MSVC CLI compile passed at {sha[:12]}", url)
    return Verdict(
        ALLOW,
        f"no compile verdict on the last {inspected} candidate commit(s); "
        "nothing to hold the tag on",
    )


def select_candidates(
    first_parent: list[str], touching: set[str], batch: int = QUEUE_BATCH_MAX
) -> list[str]:
    """Commits that could carry a verdict, newest first. Pure.

    A commit qualifies when it, or one of the `batch - 1` first-parent commits
    below it, touched the push paths: a push can carry that many merges, and the
    run reports on the newest.
    """
    picked: list[str] = []
    for index, sha in enumerate(first_parent):
        if any(older in touching for older in first_parent[index:index + batch]):
            picked.append(sha)
    return picked


def candidate_commits(head: str) -> list[str]:
    def rev_list(*pathspec: str) -> list[str]:
        out = subprocess.run(
            ["git", "rev-list", "--first-parent", f"--max-count={HISTORY_WINDOW}",
             head, *(("--", *pathspec) if pathspec else ())],
            capture_output=True, text=True, check=True,
        ).stdout
        return [line.strip() for line in out.splitlines() if line.strip()]

    return select_candidates(rev_list(), set(rev_list(*PUSH_PATHS)))


def _gh_json(args: list[str]) -> object:
    out = subprocess.run(
        ["gh", *args], capture_output=True, text=True, check=True
    ).stdout
    return json.loads(out) if out.strip() else {}


def _write_output(verdict: Verdict) -> None:
    target = os.environ.get("GITHUB_OUTPUT")
    if not target:
        return
    reason = verdict.reason.replace("\n", " ")
    with open(target, "a", encoding="utf-8") as handle:
        handle.write(f"block={'1' if verdict.action == BLOCK else '0'}\n")
        handle.write(f"reason={reason}\n")
        handle.write(f"run_url={verdict.run_url}\n")


def main() -> int:
    if os.environ.get("PULP_RELEASE_WINDOWS_PRECHECK", "").strip().lower() == "off":
        verdict = Verdict(ALLOW, "disabled by PULP_RELEASE_WINDOWS_PRECHECK=off")
    else:
        repo = os.environ["REPO"]
        head = os.environ.get("HEAD_SHA", "HEAD")
        name = urllib.parse.quote(VERDICT_CHECK)
        try:
            commits = candidate_commits(head)

            def checks_for(sha: str) -> list[dict]:
                data = _gh_json(
                    ["api", f"repos/{repo}/commits/{sha}/check-runs"
                            f"?check_name={name}&filter=all&per_page=100"]
                )
                return data.get("check_runs", []) if isinstance(data, dict) else []

            verdict = decide(commits, checks_for=checks_for)
        except (subprocess.CalledProcessError, ValueError, KeyError) as error:
            detail = getattr(error, "stderr", "") or str(error)
            verdict = Verdict(
                ALLOW,
                f"could not read the {VERDICT_CHECK} checks ({detail.strip()[:200]}); "
                "not holding the tag on missing evidence",
            )
    print(verdict.as_json())
    _write_output(verdict)
    return 0


if __name__ == "__main__":
    sys.exit(main())
