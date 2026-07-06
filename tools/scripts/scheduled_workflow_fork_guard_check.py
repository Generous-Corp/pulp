#!/usr/bin/env python3
"""Enforce the fork-guard on scheduled GitHub Actions workflows.

When someone forks this repo, GitHub copies every workflow and runs the
``on: schedule`` ones on the fork's default branch — then emails the fork owner
whenever one *fails* (and our monitors WILL fail on a fork: they probe this
repo's state or use secrets the fork lacks). The fix is a per-job guard so a
scheduled run no-ops on a fork:

    if: github.event_name != 'schedule' || github.repository == 'danielraffel/pulp'

This lint keeps that guard in place as new scheduled workflows land. For every
workflow with an ``on: schedule`` trigger, each *entry* job (one with no
``needs:`` — the roots that a scheduled run actually dispatches; dependents
cascade-skip when their roots skip) must carry an ``if:`` that references
``github.repository`` (the guard, optionally composed with the job's own
condition). Non-scheduled workflows and dependent jobs are ignored.

The guard only suppresses the *schedule* event on forks — ``push`` /
``pull_request`` / ``workflow_dispatch`` are untouched, so a contributor's PR to
this repo (which runs in this repo's context) and a fork owner's manual dispatch
both behave exactly as before.

Escape hatch: a workflow that legitimately should run on forks' schedules can
opt out with a top-of-file ``# fork-guard-exempt: <reason>`` comment.

Usage:
    scheduled_workflow_fork_guard_check.py            # scan .github/workflows
    scheduled_workflow_fork_guard_check.py --list     # list scheduled workflows + status
    scheduled_workflow_fork_guard_check.py <files...>  # scan specific files

Exit codes: 0 = all guarded (or exempt), 1 = one or more violations.
"""
from __future__ import annotations

import glob
import sys

try:
    import yaml
except ImportError:
    print("scheduled_workflow_fork_guard_check: PyYAML not available", file=sys.stderr)
    sys.exit(2)

CANONICAL = "github.repository"
EXEMPT_MARKER = "# fork-guard-exempt"


def _on_block(doc):
    # YAML parses the bare key `on:` as the boolean True, so check both.
    if not isinstance(doc, dict):
        return None
    return doc.get("on", doc.get(True))


def is_scheduled(doc) -> bool:
    on = _on_block(doc)
    return isinstance(on, dict) and "schedule" in on


def entry_jobs(doc):
    jobs = doc.get("jobs", {}) if isinstance(doc, dict) else {}
    return {
        name: job
        for name, job in jobs.items()
        if isinstance(job, dict) and "needs" not in job
    }


def job_is_guarded(job) -> bool:
    cond = job.get("if")
    return isinstance(cond, str) and CANONICAL in cond


def check_file(path: str):
    """Return a list of violation strings for one workflow file."""
    raw = open(path, encoding="utf-8").read()
    if EXEMPT_MARKER in raw:
        return []
    try:
        doc = yaml.safe_load(raw)
    except yaml.YAMLError as exc:
        return [f"{path}: YAML parse error: {exc}"]
    if not is_scheduled(doc):
        return []
    violations = []
    for name, job in entry_jobs(doc).items():
        if not job_is_guarded(job):
            violations.append(f"{path}: job '{name}' runs on schedule but lacks the fork guard")
    return violations


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    list_mode = "--list" in argv
    files = args or sorted(glob.glob(".github/workflows/*.yml") + glob.glob(".github/workflows/*.yaml"))

    all_violations = []
    for path in files:
        raw = open(path, encoding="utf-8").read()
        try:
            doc = yaml.safe_load(raw)
        except yaml.YAMLError:
            doc = None
        if list_mode and is_scheduled(doc):
            status = "EXEMPT" if EXEMPT_MARKER in raw else ("OK" if not check_file(path) else "MISSING GUARD")
            print(f"  {status:13} {path}")
        all_violations.extend(check_file(path))

    if all_violations:
        print("scheduled-workflow-fork-guard: violations found:", file=sys.stderr)
        for v in all_violations:
            print(f"  ✗ {v}", file=sys.stderr)
        print(
            "\nAdd the guard to each flagged entry job (a job with no `needs:`):\n"
            "    if: github.event_name != 'schedule' || github.repository == 'danielraffel/pulp'\n"
            "compose it with an existing condition as\n"
            "    if: (github.event_name != 'schedule' || github.repository == 'danielraffel/pulp') && (<existing>)\n"
            "or, if the workflow SHOULD run on forks' schedules, add a top-of-file\n"
            "    # fork-guard-exempt: <reason>\n",
            file=sys.stderr,
        )
        return 1

    if not list_mode:
        print("scheduled-workflow-fork-guard: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
