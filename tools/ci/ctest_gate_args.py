#!/usr/bin/env python3
"""Decide the event-dependent ctest arguments for the Build and Test workflow.

Two ctest decisions depend on which event is running the suite, and both were
previously inline shell in `.github/workflows/build.yml`. They live here so the
rules are stated once, tested, and cannot drift between the lanes that share
them.

The lanes, and why they differ:

* ``merge_group`` is a GATE. Its only question is "may this batch land?", and a
  batch that has one failing test is already unlandable. Running the remaining
  suite proves nothing the first failure did not, and a merge queue multiplies
  the waste: every batch inherits the break, every lane pays it, and each
  re-formed batch pays it again. So the queue stops at the first failure.

* ``push`` to main is the DETECTOR and the diagnostic. It is the only lane that
  runs the whole macOS suite on a commit that is actually on main, so it must
  report every failing test rather than the first one. Stopping early here
  would trade the one complete signal in the system for a few minutes.

* The gate events also exclude ``source-selftest``: registrations that read
  only the checkout, which the build-free required ``Enforce version & skill
  sync`` context runs instead (``tools/ci/source_selftests.py``).

* ``pull_request`` heads run the ``pr-fast`` tier, which is what their
  required check means. A head that is READY TO LAND also runs the full suite,
  as evidence rather than as a gate: see ``pr_suite`` below.

The label set is a separate axis: `performance`, `bench` and `quality-lab` are
relative-timing tests that are robust to steady load but not to the load
VARIANCE of a host running concurrent build VMs. They are excluded wherever the
suite runs on the shared self-hosted macOS gate hosts, which now includes the
push lane, and retained on the steady GitHub-hosted Linux and Windows runners.
"""
from __future__ import annotations

import argparse
import json
import os
import shlex
import sys
import urllib.request
from typing import Any, Callable

# Label set for lanes whose timing tests would be measuring runner load.
SHARED_HOST_LABEL_EXCLUDE = "validation|slow|performance|bench|quality-lab"
# Source-only Python selftests (tools/ci/source_selftests.json). The required
# `Enforce version & skill sync` context runs them without a build, so the gate
# events drop them; `push` keeps them as the macOS detector.
SOURCE_SELFTEST_LABEL = "source-selftest"
# Label set for the gate events.
GATE_LABEL_EXCLUDE = f"{SHARED_HOST_LABEL_EXCLUDE}|{SOURCE_SELFTEST_LABEL}"
# Label set for steady-load lanes, which keep the heavier informational tests.
FULL_LABEL_EXCLUDE = "validation"

# Events whose suite runs on the shared self-hosted macOS gate hosts, or which
# stand in for them.
GATE_EVENTS = frozenset({"pull_request", "workflow_dispatch", "merge_group"})

STOP_ON_FAILURE_FLAG = "--stop-on-failure"


def _norm(value: str | None) -> str:
    return (value or "").strip()


def label_exclude(event_name: str, runner_os: str) -> str:
    """The ctest ``-LE`` value for this lane."""
    event = _norm(event_name)
    if event in GATE_EVENTS:
        return GATE_LABEL_EXCLUDE
    # A push builds macOS on the same shared Studios that serve the required
    # gate, so it inherits the gate's timing-test exclusions. Linux and Windows
    # pushes stay on GitHub-hosted runners under steady load and keep the
    # heavier set.
    if event == "push" and _norm(runner_os).lower() == "macos":
        return SHARED_HOST_LABEL_EXCLUDE
    return FULL_LABEL_EXCLUDE


def stop_on_failure(event_name: str) -> str:
    """``--stop-on-failure`` for the gate lane, empty for every other lane.

    ctest applies this only after ``--repeat until-pass`` has exhausted its
    retries, so a single flake still self-heals; only a test that fails every
    attempt stops the run.
    """
    return STOP_ON_FAILURE_FLAG if _norm(event_name) == "merge_group" else ""


# The branch the merge queue lands on. A receipt names the base it was
# validated against, and only a merge group on this branch can consume one.
QUEUE_BRANCH = "main"


def pr_suite(
    *,
    armed: bool,
    base_ref: str,
    event_base_sha: str,
    base_tip_sha: str,
    event_head_sha: str,
    live_head_sha: str,
) -> tuple[str, str]:
    """Which ctest suite a pull-request head runs: ``("full"|"fast", why)``.

    A merge group of one pull request can reuse that head's protected receipt
    instead of rebuilding, but only a full-suite run issues a receipt, and the
    pull-request gate itself runs only the fast tier. So the full suite runs
    once, on the run that will actually feed the queue: the head is armed for
    auto-merge (armed means "ready to land"), it is still the pull request's
    head, and the base it was merged onto is still the tip of the queue branch.

    Each clause removes a run whose receipt could never be consumed. The
    receipt is bound to the exact head and base (the merge group verifies the
    two-parent merge of those exact commits), so a newer push or a moved base
    makes it unmatchable, and the full suite would only have cost gate time.

    The full suite is evidence here, never the pull request's gate: the
    required check still means build + fast tier.
    """
    if not armed:
        return "fast", "not armed for auto-merge"
    if _norm(base_ref) != QUEUE_BRANCH:
        return "fast", f"base branch {base_ref!r} has no merge queue"
    if not event_head_sha or _norm(live_head_sha) != _norm(event_head_sha):
        return "fast", "a newer commit superseded this head"
    if not event_base_sha or _norm(base_tip_sha) != _norm(event_base_sha):
        return "fast", "the base branch moved since this head was merged onto it"
    return "full", "armed for auto-merge on the current base; issuing a receipt"


Fetch = Callable[[str], Any]


def _github_fetch(api_url: str, token: str) -> Fetch:
    def fetch(path: str) -> Any:
        request = urllib.request.Request(
            f"{api_url.rstrip('/')}/{path.lstrip('/')}",
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": f"Bearer {token}",
                "X-GitHub-Api-Version": "2022-11-28",
            },
        )
        with urllib.request.urlopen(request, timeout=20) as response:
            return json.load(response)
    return fetch


def probe_pr_suite(
    fetch: Fetch,
    *,
    repository: str,
    number: str,
    event_base_sha: str,
    event_head_sha: str,
) -> tuple[str, str]:
    """Read the pull request's live state and decide with ``pr_suite``.

    Fails closed: anything unreadable or malformed means the fast tier, which
    is exactly what a pull-request head ran before receipts existed.

    REST ``auto_merge`` is non-null for a pull request that is armed and still
    waiting on its checks, and null once the merge queue holds it (decisions
    contract row 11). A run that feeds the queue is by construction the one
    producing those checks, so it sees the armed value; a queued pull request
    reads as unarmed and keeps the fast tier, which is correct because its
    merge group already exists.
    """
    try:
        pull = fetch(f"repos/{repository}/pulls/{number}")
        if not isinstance(pull, dict):
            raise ValueError("pull request is not an object")
        base_ref = pull["base"]["ref"]
        ref = fetch(f"repos/{repository}/git/ref/heads/{base_ref}")
        base_tip = ref["object"]["sha"]
        return pr_suite(
            armed=isinstance(pull.get("auto_merge"), dict),
            base_ref=base_ref,
            event_base_sha=event_base_sha,
            base_tip_sha=base_tip,
            event_head_sha=event_head_sha,
            live_head_sha=pull["head"]["sha"],
        )
    except Exception as error:  # noqa: BLE001 - any failure keeps the fast tier
        reason = " ".join(str(error).split()) or type(error).__name__
        return "fast", f"pull request state unavailable ({reason})"


def decide(event_name: str, runner_os: str) -> dict[str, str]:
    return {
        "label_exclude": label_exclude(event_name, runner_os),
        "stop_on_failure": stop_on_failure(event_name),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--event-name", required=True)
    parser.add_argument("--runner-os", default="")
    parser.add_argument("--format", choices=("shell", "json"), default="shell")
    parser.add_argument(
        "--pr-suite",
        action="store_true",
        help="decide the pull-request suite (pr_suite, pr_suite_reason) from "
        "the live pull request; reads GITHUB_TOKEN, GITHUB_REPOSITORY, "
        "PR_NUMBER, PR_BASE_SHA and PR_HEAD_SHA",
    )
    args = parser.parse_args(argv)

    if args.pr_suite:
        if _norm(args.event_name) != "pull_request":
            decision = {"pr_suite": "fast", "pr_suite_reason": "not a pull request"}
        else:
            env = os.environ
            suite, reason = probe_pr_suite(
                _github_fetch(env.get("GITHUB_API_URL", "https://api.github.com"),
                              env.get("GITHUB_TOKEN", "")),
                repository=env.get("GITHUB_REPOSITORY", ""),
                number=env.get("PR_NUMBER", ""),
                event_base_sha=env.get("PR_BASE_SHA", ""),
                event_head_sha=env.get("PR_HEAD_SHA", ""),
            )
            decision = {"pr_suite": suite, "pr_suite_reason": reason}
    else:
        decision = decide(args.event_name, args.runner_os)
    if args.format == "json":
        print(json.dumps(decision, sort_keys=True))
        return 0
    # Shell-quoted so the caller can `eval` the output without word-splitting
    # surprises, even though every value in the vocabulary is already a single
    # shell-safe token.
    for key, value in decision.items():
        print(f"{key}={shlex.quote(value)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
