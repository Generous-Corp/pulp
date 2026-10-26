#!/usr/bin/env python3
"""Resolve and police the runs-on selector for a Windows x64 job.

A workflow_dispatch input can move a Windows job onto a self-hosted runner. A
typo or a pasted macOS selector would otherwise send the Windows build onto the
macOS gate VMs, or onto the Linux pool, where it either queues behind the
required gate or runs a Windows job on the wrong OS. This resolves the input
before any runner is chosen and refuses anything that is not a Windows
selector.

    windows_runner_selector.py --selector '<json or empty>' [--github-output]

An empty selector resolves to the GitHub-hosted fallback (--fallback,
default windows-latest). Exit 0 prints (or writes) runs_on=<compact JSON>;
exit 2 refuses the selector with a reason on stderr.

With --route the default flips: an empty selector resolves to the self-hosted
Windows x64 lane (--default-selector, from PULP_LOCAL_WINDOWS_X64_RUNS_ON_JSON,
else DEFAULT_SELF_HOSTED), and GitHub-hosted is used only when

  * the caller opted in (--allow-hosted 1/true: PULP_ALLOW_HOSTED_WINDOWS=1 or a
    dispatch's allow_hosted_windows input),
  * the lane selector is itself a hosted label (PULP_LOCAL_WINDOWS_X64_RUNS_ON_JSON
    set to '"windows-latest"' switches every routed job back to its own hosted
    image, without the push and pull-request jobs PULP_ALLOW_HOSTED_WINDOWS
    also turns on), or
  * the event is a pull request from a fork, whose code never runs on our VMs.
    This is checked first, so no selector or variable overrides it, and a
    pull request whose head repository is missing counts as a fork.

The lane is one VM that runs one job at a time, so an automatic event
(anything but workflow_dispatch) given --max-backlog N skips the job when N or
more Windows x64 jobs are already queued or running on the lane. Nothing that
reads this is a required check, so a skipped advisory job costs signal, never a
merge; a dispatch always queues, because someone asked for it. --route also
writes route=self-hosted|hosted|skip and route_reason=<reason>.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from typing import Callable, Optional

LABEL = re.compile(r"^[A-Za-z0-9._-]+$")
# Labels that belong to another OS's pool. Matched case-insensitively against
# whole labels, and as substrings for capability labels such as
# pulp-build-linux-x64 or pulp-gate-fast.
FOREIGN_LABELS = {"macos", "osx", "darwin", "linux", "ubuntu"}
FOREIGN_FRAGMENTS = ("macos", "linux", "ubuntu", "pulp-gate", "pulp-build-vm", "pulp-preamble")


# The Mac Pro Windows x64 slot carries these plus its host label; leaving the
# host label out lets a second Windows x64 host serve the same jobs.
DEFAULT_SELF_HOSTED = ["self-hosted", "Windows", "X64", "pulp-build-windows-x64"]
LANE_LABEL = "pulp-build-windows-x64"
ROUTE_REASONS = (
    "explicit-selector",
    "hosted-opt-in",
    "fork-hosted",
    "capacity-skip",
    "self-hosted-default",
)


class SelectorError(ValueError):
    pass


def _check_label(label: object) -> str:
    if not isinstance(label, str) or not LABEL.match(label):
        raise SelectorError(f"invalid runner label: {label!r}")
    folded = label.casefold()
    if folded in FOREIGN_LABELS or any(fragment in folded for fragment in FOREIGN_FRAGMENTS):
        raise SelectorError(f"label {label!r} selects a non-Windows runner pool")
    return label


def resolve(raw: str, fallback: str = "windows-latest") -> str:
    """Return the compact JSON runs-on value for a selector input."""
    raw = raw.strip()
    if not raw:
        return json.dumps(fallback)
    try:
        value = json.loads(raw)
    except ValueError as exc:
        raise SelectorError(f"selector is not JSON: {exc}") from exc
    if isinstance(value, str):
        label = _check_label(value)
        if not label.casefold().startswith("windows-"):
            raise SelectorError(f"a single label must be a GitHub-hosted Windows image, not {label!r}")
        return json.dumps(label)
    if not isinstance(value, list) or not value:
        raise SelectorError("selector must be a label string or a non-empty list of labels")
    labels = [_check_label(label) for label in value]
    if len(set(label.casefold() for label in labels)) != len(labels):
        raise SelectorError("selector repeats a label")
    folded = {label.casefold() for label in labels}
    if "windows" not in folded:
        raise SelectorError("a self-hosted selector must include the Windows label")
    if "self-hosted" not in folded:
        raise SelectorError("a label list must include self-hosted")
    return json.dumps(labels, separators=(",", ":"))


def _is_self_hosted(runs_on: str) -> bool:
    value = json.loads(runs_on)
    return isinstance(value, list) and "self-hosted" in {str(label).casefold() for label in value}


def count_lane_backlog(repository: str, current_run_id: str = "", label: str = LANE_LABEL) -> int:
    """Windows x64 lane jobs queued or running in other runs of `repository`.

    A probe failure counts 0, so an API blip queues the job rather than
    dropping it.
    """
    if not repository:
        return 0
    jq_runs = f'[.workflow_runs[] | select((.id | tostring) != "{current_run_id}") | .id] | .[]'
    jq_jobs = (
        '[.jobs[] | select(.status == "queued" or .status == "in_progress") '
        f'| select((.labels // []) | index({json.dumps(label)}))] | length'
    )
    try:
        run_ids: list[str] = []
        for status in ("queued", "in_progress"):
            listed = subprocess.run(
                ["gh", "api", f"repos/{repository}/actions/runs?status={status}&per_page=100",
                 "--jq", jq_runs],
                check=True, capture_output=True, text=True, encoding="utf-8", timeout=20,
            )
            run_ids += [line.strip() for line in listed.stdout.splitlines() if line.strip()]
        backlog = 0
        for run_id in dict.fromkeys(run_ids):
            jobs = subprocess.run(
                ["gh", "api", f"repos/{repository}/actions/runs/{run_id}/jobs?per_page=100",
                 "--jq", jq_jobs],
                check=True, capture_output=True, text=True, encoding="utf-8", timeout=20,
            )
            backlog += int((jobs.stdout or "0").strip() or "0")
        return backlog
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        print(f"windows-runner-selector: backlog probe failed, counting 0: {exc}", file=sys.stderr)
        return 0


def route(
    *,
    event_name: str,
    selector: str = "",
    default_selector: str = "",
    hosted_label: str = "windows-latest",
    allow_hosted: str = "",
    head_repo: str = "",
    repository: str = "",
    max_backlog: Optional[int] = None,
    backlog: Callable[[], int] = lambda: 0,
) -> tuple[str, str, str]:
    """Return (runs_on JSON, route, reason) for a Windows x64 job."""
    hosted = resolve("", hosted_label)
    # First, before any selector or variable: repository variables resolve for
    # fork pull requests, so nothing a pull request can carry may reach the
    # lane. A head repository that is missing or differs from this one counts
    # as a fork.
    if event_name.startswith("pull_request") and (
        not head_repo.strip() or not repository.strip() or head_repo.strip() != repository.strip()
    ):
        return hosted, "hosted", "fork-hosted"
    if selector.strip():
        runs_on = resolve(selector, hosted_label)
        return runs_on, "self-hosted" if _is_self_hosted(runs_on) else "hosted", "explicit-selector"
    if allow_hosted.strip().casefold() in ("1", "true"):
        return hosted, "hosted", "hosted-opt-in"
    local = resolve(default_selector or json.dumps(DEFAULT_SELF_HOSTED), hosted_label)
    if not _is_self_hosted(local):
        return hosted, "hosted", "hosted-opt-in"
    if max_backlog is not None and event_name != "workflow_dispatch":
        queued = backlog()
        if queued >= max_backlog:
            print(
                f"windows-runner-selector: {queued} Windows x64 job(s) already queued or "
                f"running (limit {max_backlog}); skipping this advisory job",
                file=sys.stderr,
            )
            return local, "skip", "capacity-skip"
    return local, "self-hosted", "self-hosted-default"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--selector", default="")
    parser.add_argument("--fallback", default="windows-latest")
    parser.add_argument("--github-output", action="store_true",
                        help="append runs_on=... to $GITHUB_OUTPUT")
    parser.add_argument("--route", action="store_true",
                        help="default to the self-hosted Windows x64 lane (see the module doc)")
    parser.add_argument("--event-name", default=os.environ.get("GITHUB_EVENT_NAME", ""))
    parser.add_argument("--default-selector", default="",
                        help="self-hosted selector for --route; empty uses DEFAULT_SELF_HOSTED")
    parser.add_argument("--allow-hosted", default="",
                        help="1 or true routes --route to the hosted fallback")
    parser.add_argument("--head-repo", default="", help="a pull request's head owner/repo")
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY", ""))
    parser.add_argument("--max-backlog", default="",
                        help="skip an automatic --route job when this many lane jobs are waiting")
    args = parser.parse_args(argv)
    try:
        _check_label(args.fallback)
        if args.route:
            max_backlog = int(args.max_backlog) if args.max_backlog.strip() else None
            runs_on, chosen, reason = route(
                event_name=args.event_name,
                selector=args.selector,
                default_selector=args.default_selector,
                hosted_label=args.fallback,
                allow_hosted=args.allow_hosted,
                head_repo=args.head_repo,
                repository=args.repository,
                max_backlog=max_backlog,
                backlog=lambda: count_lane_backlog(
                    args.repository, os.environ.get("GITHUB_RUN_ID", "")),
            )
            lines = [f"runs_on={runs_on}", f"route={chosen}", f"route_reason={reason}"]
        else:
            lines = [f"runs_on={resolve(args.selector, args.fallback)}"]
    except (SelectorError, ValueError) as exc:
        print(f"windows-runner-selector: refused: {exc}", file=sys.stderr)
        return 2
    if args.github_output:
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as handle:
            handle.write("".join(line + "\n" for line in lines))
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
