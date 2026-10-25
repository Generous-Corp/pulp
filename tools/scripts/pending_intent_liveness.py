#!/usr/bin/env python3
"""Release liveness alarm — the time-based backstop for release-please.

A release-worthy merge no longer moves a version on its own. release-please
records it in the standing release PR for its surface, release_pr_enqueue.py
lands that PR when the merge queue is idle (or when the max-wait backstop
fires), auto-release.yml tags the merge, and release_please_handoff.py hands
the tagged PR back to release-please. Every one of those steps can stop
quietly, and each stall leaves the same symptom: a change on main that
consumers cannot reach, with nothing failing.

This alarm keys on the three places such a stall becomes visible, so it does
not need to re-derive which commits are releasable:

* `release-pr-overdue` — an open release PR older than `--threshold-minutes`.
  release-please opens it at the first releasable merge after a release, so
  its age is how long that change has waited. The threshold sits above the
  enqueue backstop, so a healthy busy queue never trips it; a red release PR,
  a held (draft) one, or a dead enqueuer does.
* `handoff-stuck` — a merged release PR still carrying its pending labels past
  `--grace-minutes`. Until the tag is cut and the labels swapped, release-please
  refuses to open the next release PR, so every later merge is stranded.
* `release-please-silent` — no successful run of release-please.yml on main
  within `--threshold-minutes`. A broken config or token fails that workflow,
  no release PR ever opens, and the first check alone would stay quiet.
* `unreleased-change` — a merge after a surface's newest tag, older than
  `--threshold-minutes`, that touches that surface's trigger paths and whose
  PR title is not an explicitly non-releasing type (`ci:`, `test:`, `docs:`,
  `chore:`, `build:`, `refactor:`, `style:`, `revert:`), while the surface has
  no open release PR. That is a fix or feature release-please could not see
  (a plain-language title with untyped commits, a non-standard type, a `!`
  only in the title), so no release PR opened and none of the checks above
  fires. Reads the local checkout, so it needs full history and tags.

    python3 tools/scripts/pending_intent_liveness.py --repo-name OWNER/REPO [--repo .] [--json]

Exit 0 = healthy, 1 = at least one stall (the workflow opens the tracker),
2 = could not read GitHub (which is not a pass).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gate_common import matches_any  # noqa: E402
from release_please_handoff import load_surfaces  # noqa: E402
from release_pr_enqueue import RELEASE_BRANCHES  # noqa: E402

# The enqueue backstop is 4 h (release-pr-enqueue.yml); two more hours covers
# one full build of the release PR after the backstop enqueues it.
DEFAULT_THRESHOLD_MINUTES = 360
# auto-release.yml tags on the merge push and swaps the labels in the same run.
DEFAULT_GRACE_MINUTES = 60
RELEASE_WORKFLOW = "release-please.yml"


def _parse(stamp: str) -> datetime:
    return datetime.fromisoformat(stamp.replace("Z", "+00:00"))


def _age_minutes(now: datetime, stamp: str) -> int:
    return max(0, int((now - _parse(stamp)).total_seconds() // 60))


def assess(
    open_pulls: list[dict],
    merged_pending: list[dict],
    last_release_run: str | None,
    now: datetime,
    threshold_minutes: int = DEFAULT_THRESHOLD_MINUTES,
    grace_minutes: int = DEFAULT_GRACE_MINUTES,
) -> dict:
    """Pure detection over GitHub facts. `overdue` non-empty == ALARM."""
    overdue: list[dict] = []
    pending: list[dict] = []
    for pull in open_pulls:
        if pull.get("head", {}).get("ref", "") not in RELEASE_BRANCHES:
            continue
        age = _age_minutes(now, pull["created_at"])
        row = {"kind": "release-pr-overdue", "pr": pull["number"],
               "title": pull["title"], "age_minutes": age}
        (overdue if age >= threshold_minutes else pending).append(row)
    for pull in merged_pending:
        if not pull.get("merged_at"):
            continue
        age = _age_minutes(now, pull["merged_at"])
        row = {"kind": "handoff-stuck", "pr": pull["number"],
               "title": pull["title"], "age_minutes": age}
        (overdue if age >= grace_minutes else pending).append(row)
    if last_release_run is None:
        overdue.append({"kind": "release-please-silent", "pr": None,
                        "title": f"no successful {RELEASE_WORKFLOW} run on main",
                        "age_minutes": None})
    else:
        age = _age_minutes(now, last_release_run)
        if age >= threshold_minutes:
            overdue.append({"kind": "release-please-silent", "pr": None,
                            "title": f"last successful {RELEASE_WORKFLOW} run",
                            "age_minutes": age})
    return {"overdue": overdue, "pending": pending}


# PR-title types that never release under conventional-commit semantics.
NON_RELEASING = re.compile(r"^(ci|test|docs|chore|build|refactor|style|revert)(\([^)]*\))?: ",
                           re.ASCII)


def _git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True,
                          encoding="utf-8", check=True).stdout


def unreleased_changes(repo: Path, config: Path, open_surfaces: set[str],
                       now: datetime, threshold_minutes: int) -> list[dict]:
    """Merges after each surface's newest tag that should have opened a release PR."""
    data = json.loads(config.read_text(encoding="utf-8"))
    rows: list[dict] = []
    for surface in load_surfaces(repo, config):
        if surface.name in open_surfaces:
            continue
        triggers = data["surfaces"][surface.name]["trigger_paths"]
        tag = _git(repo, "describe", "--tags", "--abbrev=0", "--match",
                   f"{surface.tag_prefix}[0-9]*", "HEAD").strip()
        for line in _git(repo, "log", "--first-parent", "--format=%H %ct %P",
                         f"{tag}..HEAD").splitlines():
            sha, stamp, *parents = line.split()
            if len(parents) != 2:
                continue
            title = (_git(repo, "log", "-1", "--format=%b", sha).splitlines() or [""])[0].strip()
            age = int((now.timestamp() - int(stamp)) // 60)
            if NON_RELEASING.match(title) or age < threshold_minutes:
                continue
            files = _git(repo, "diff", "--name-only", parents[0], sha).splitlines()
            if any(matches_any(path, triggers) for path in files):
                rows.append({"kind": "unreleased-change", "pr": None,
                             "title": f"{surface.name} since {tag}: {sha[:12]} {title[:80]}",
                             "age_minutes": age})
    return rows


def _gh(*args: str):
    proc = subprocess.run(["gh", "api", *args], capture_output=True, text=True,
                          encoding="utf-8")
    if proc.returncode != 0:
        raise RuntimeError(f"gh api {' '.join(args)}: {proc.stderr.strip()}")
    return json.loads(proc.stdout)


def gather(repo_name: str, repo: Path, config: Path) -> tuple[list[dict], list[dict], str | None]:
    surfaces = load_surfaces(repo, config)
    open_pulls: dict[int, dict] = {}
    merged_pending: dict[int, dict] = {}
    for surface in surfaces:
        labels = ",".join(surface.labels)
        for state, into in (("open", open_pulls), ("closed", merged_pending)):
            for issue in _gh("-X", "GET", f"repos/{repo_name}/issues", "-f", f"state={state}",
                             "-f", f"labels={labels}", "-f", "per_page=30"):
                if "pull_request" in issue:
                    into[issue["number"]] = _gh(f"repos/{repo_name}/pulls/{issue['number']}")
    runs = _gh("-X", "GET", f"repos/{repo_name}/actions/workflows/{RELEASE_WORKFLOW}/runs",
               "-f", "branch=main", "-f", "status=success", "-f", "per_page=1")
    latest = (runs.get("workflow_runs") or [{}])[0].get("updated_at")
    return list(open_pulls.values()), list(merged_pending.values()), latest


def _fmt(rows: list[dict]) -> str:
    return ", ".join(
        f"{r['kind']}" + (f" #{r['pr']}" if r["pr"] else "")
        + (f" ({r['age_minutes']}m)" if r["age_minutes"] is not None else "")
        for r in rows
    )


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo-name", required=True, help="OWNER/REPO")
    ap.add_argument("--repo", default=".", type=Path)
    ap.add_argument("--config", default="tools/scripts/versioning.json", type=Path)
    ap.add_argument(
        "--threshold-minutes", type=int,
        default=int(os.environ.get("PULP_RELEASE_LIVENESS_THRESHOLD_MIN",
                                   DEFAULT_THRESHOLD_MINUTES)),
    )
    ap.add_argument(
        "--grace-minutes", type=int,
        default=int(os.environ.get("PULP_RELEASE_HANDOFF_GRACE_MIN",
                                   DEFAULT_GRACE_MINUTES)),
    )
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)

    try:
        open_pulls, merged_pending, last_run = gather(args.repo_name, args.repo, args.config)
    except (RuntimeError, OSError, KeyError, json.JSONDecodeError) as error:
        print(f"::error::release liveness could not read GitHub: {error}", file=sys.stderr)
        return 2
    now = datetime.now(timezone.utc)
    result = assess(open_pulls, merged_pending, last_run, now,
                    args.threshold_minutes, args.grace_minutes)
    open_surfaces = {
        surface for pull in open_pulls
        for surface in [RELEASE_BRANCHES.get(pull.get("head", {}).get("ref", ""))] if surface
    }
    try:
        result["overdue"].extend(unreleased_changes(
            args.repo, args.config, open_surfaces, now, args.threshold_minutes))
    except (subprocess.CalledProcessError, OSError, KeyError) as error:
        print(f"::error::release liveness could not read the checkout: {error}", file=sys.stderr)
        return 2
    # With --json, stdout carries only the JSON the workflow parses; the
    # annotations go to stderr, which the runner reads just the same.
    log = sys.stderr if args.json else sys.stdout
    if args.json:
        print(json.dumps(result))
    if result["pending"]:
        print(f"::notice::release in progress: {_fmt(result['pending'])}", file=log)
    if result["overdue"]:
        print(
            "::error::a release has stalled: "
            f"{_fmt(result['overdue'])}. Merged changes are not reaching a tag. "
            "See docs/guides/versioning.md § Release PR.",
            file=log,
        )
        return 1
    print(f"release liveness OK: {len(result['pending'])} in progress, 0 overdue.", file=log)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
