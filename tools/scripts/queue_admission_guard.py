#!/usr/bin/env python3
"""Refuse to admit work to the merge queue while main's own suite is red.

THE WASTE THIS EXISTS TO STOP. A merge_group batch is `main` + the queued
entries. When main carries a failing test, EVERY batch inherits it and fails
after a full ~40-minute gate, on up to three lanes at once, then ejects
innocent PRs and re-forms. On 2026-09-23 that consumed most of a day: 10+
consecutive batches, zero merges for five hours, and three different PRs
blamed in turn while main had not changed at all.

Arming a PR into a red base is not neutral. It is a guaranteed loss of a gate
lane, and it is indistinguishable at the PR level from ordinary queueing -
which is exactly why it went unnoticed for hours.

TWO TRAPS THIS ENCODES, both of which produced wrong answers by hand:

1. A "green" macos gate can mean NOTHING RAN. `protected-receipt-reuse` lets a
   batch report success with a 3-step job that never built or tested. A health
   check that counts that as evidence is worse than no check: it reports a
   passing base while main is broken. Only a job that actually executed the
   suite counts, and step-count is the discriminator (3 vs ~41).

2. A PR-head `macos` pass is a BUILD pass, not a test pass. `Test (non-Windows)`
   is gated off `pull_request`, so the suite's first execution is inside the
   queue. A green PR gate says nothing about whether the tests pass.

Read-only. Exit 0 = admission is sane. Exit 1 = admitting would waste a lane.
"""
import json, subprocess, sys

REPO = "Generous-Corp/pulp"
RECEIPT_REUSE_MAX_STEPS = 6   # a real gate job has ~41; reuse has 3


def gh(path, jq=None):
    cmd = ["ghapp", "api", path] + (["--jq", jq] if jq else [])
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.stdout.strip() if p.returncode == 0 else None


def last_executed_batch():
    """Most recent merge_group run whose macos job GENUINELY ran the suite."""
    # Only SUCCESS or FAILURE is evidence. A `cancelled` run proves nothing -
    # and cancelling doomed batches (a legitimate way to free lanes) would
    # otherwise ERASE the red signal and flip this guard to green while the
    # base is still broken. Observed live on 2026-09-23: cancelling three
    # batches turned this check from RED to OK with nothing fixed.
    raw = gh(f"repos/{REPO}/actions/workflows/build.yml/runs"
             f"?event=merge_group&per_page=30",
             '[.workflow_runs[]|select(.conclusion=="success" or .conclusion=="failure")]'
             '|.[]|"\\(.id) \\(.conclusion) \\(.created_at)"')
    if not raw:
        return None
    for line in raw.splitlines():
        rid, concl, created = line.split(None, 2)
        jid = gh(f"repos/{REPO}/actions/runs/{rid}/jobs?per_page=50",
                 '.jobs[]|select(.name=="macos")|.id')
        if not jid:
            continue
        jid = jid.splitlines()[0]
        steps = gh(f"repos/{REPO}/actions/jobs/{jid}", ".steps|length")
        try:
            n = int(steps or 0)
        except ValueError:
            n = 0
        if n <= RECEIPT_REUSE_MAX_STEPS:
            continue          # receipt reuse - ran nothing, proves nothing
        failing = gh(f"repos/{REPO}/actions/jobs/{jid}",
                     '[.steps[]|select(.conclusion=="failure")|.name]|join(", ")')
        return {"run": rid, "conclusion": concl, "created": created,
                "steps": n, "failing_steps": failing or ""}
    return None


def last_merge_to_main():
    """When main last moved. Evidence older than that describes a different base."""
    return gh(f"repos/{REPO}/commits?sha=main&per_page=1",
              ".[0].commit.committer.date")


def main() -> int:
    b = last_executed_batch()
    if b is None:
        print("UNKNOWN: no merge_group batch in the sample actually executed the")
        print("  suite - every one was receipt reuse. The base's health is UNPROVEN.")
        print("  Treat as red: admitting work now is a coin flip on a lane.")
        return 1

    tag = f"run {b['run']} ({b['created'][11:16]}, {b['steps']} steps)"

    # RECENCY. A pass that predates the last merge to main describes a DIFFERENT
    # base and proves nothing about this one. Observed live on 2026-09-23: after
    # cancelling the day's failing batches this guard reached back TWENTY DAYS,
    # found a success from 09-03, and reported the base healthy while it was red.
    # A guard with no age bound does not fail loudly - it reassures you.
    last_merge = last_merge_to_main()
    if last_merge and b["created"] < last_merge:
        print(f"STALE EVIDENCE: newest genuinely-executed batch is {tag},")
        print(f"  but main last moved at {last_merge}. That batch tested a")
        print("  different base. The current base's health is UNPROVEN - treat as")
        print("  red rather than assume the older pass still applies.")
        return 1

    if b["conclusion"] == "success":
        print(f"OK: last genuinely-executed batch PASSED - {tag}")
        print("  Admission is sane; a new batch has a real chance of merging.")
        return 0

    print(f"RED BASE: last genuinely-executed batch FAILED - {tag}")
    if b["failing_steps"]:
        print(f"  failing step(s): {b['failing_steps']}")
    print("  Every new batch inherits this and will fail the same way, burning a")
    print("  ~40-minute gate lane each. Do NOT arm PRs into it.")
    print("  Land the fix first, then re-arm. Attribute with:")
    print(f"    python3 tools/scripts/queue_batch_attribute.py {b['run']}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
