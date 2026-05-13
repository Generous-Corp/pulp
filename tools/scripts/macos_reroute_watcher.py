#!/usr/bin/env python3
"""Opportunistic macOS-overflow reroute watcher (pulp task #22).

Polls (a) the local Mac self-hosted GH Actions runner's busy/idle state and
(b) the repo's queued Build-and-Test workflow_runs that have a macOS job
dispatched to a cloud target (macos-15 or a Namespace selector). When local
is idle AND a cloud-bound job is still queued (hasn't been picked up by its
cloud runner pool yet), the watcher claws it back to local via
`pulp macos retarget --pr N --to local` — which cancels the cloud dispatch
and re-fires the macOS leg on the local Mac.

Why this exists: macOS builds run ~3× faster on the warm-cache local Mac
than on a cold GH-hosted `macos-15`. The default overflow logic in
`.github/workflows/build.yml` is "local-first when idle; cloud when busy".
That's correct, but coarse — once a PR is dispatched to cloud, it stays
there even if local frees up before the cloud runner picks it up. The
watcher captures those near-misses.

Safety properties:

- Only acts when local is **idle** (no Runner.Worker child active under
  the actions-runner workspace). Never preempts a live local job.

- Flap-guard: a PR rerouted in the last `--flap-window` (default 5 min)
  is skipped, even if conditions still match. Prevents thrashing between
  cloud and local when GH picks up the cloud dispatch faster than expected.

- Only one reroute per polling tick. With local-busy probing in
  build.yml's resolve-provider, the next eligible job won't be rerouted
  until the first finishes — natural pacing.

- Falls back to local-not-detected (skip) if `ps` returns nothing
  recognizable. Better to no-op than to falsely declare local idle.

Run as a launchd agent (see tools/launchd/pulp-macos-reroute-watcher.plist)
or by hand: `python3 tools/scripts/macos_reroute_watcher.py --interval 30`.
"""

from __future__ import annotations

import argparse
import json
import logging
import subprocess
import sys
import time
from collections import OrderedDict
from pathlib import Path
from typing import Optional

REPO = "danielraffel/pulp"
ACTIONS_RUNNER_WORKSPACE_MARKER = "actions-runner/_work/pulp"


def _gh(args: list[str]) -> str:
    """Call gh with the given args; return stdout (stripped)."""
    result = subprocess.run(
        ["gh", "api", *args],
        check=True,
        capture_output=True,
        text=True,
        timeout=20,
    )
    return result.stdout.strip()


def local_is_busy() -> Optional[bool]:
    """Return True if the local Mac runner is processing a Pulp job.

    Detection: look for Runner.Worker processes (or their build/test
    children) whose cwd includes the local runner's actions-runner
    workspace for danielraffel/pulp.
    """
    try:
        result = subprocess.run(
            ["ps", "-axww", "-o", "command="],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except subprocess.SubprocessError as exc:
        logging.warning("ps query failed: %s", exc)
        return None
    for line in result.stdout.splitlines():
        if ACTIONS_RUNNER_WORKSPACE_MARKER not in line:
            continue
        if any(tok in line for tok in ("cmake", "ctest", "ninja", "make", "clang", "swift")):
            return True
        if "Runner.Worker" in line and "spawnclient" in line:
            return True
    return False


def list_queued_cloud_bat_runs() -> list[tuple[int, int]]:
    """Return (pr_number, workflow_run_id) tuples for BAT runs whose macOS
    job is queued on a cloud target (i.e., NOT self-hosted)."""
    try:
        raw = _gh([
            f"repos/{REPO}/actions/runs?status=queued&per_page=100",
            "--jq",
            '[.workflow_runs[] | select(.name == "Build and Test") | '
            '{id, pr: (.pull_requests[0].number // null)}] | .[]',
        ])
    except subprocess.SubprocessError as exc:
        logging.warning("list workflow_runs failed: %s", exc)
        return []
    # raw is one JSON object per line.
    entries: list[tuple[int, int]] = []
    for line in raw.splitlines():
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        rid = obj.get("id")
        pr = obj.get("pr")
        if not rid or not pr:
            continue
        if _macos_job_targets_cloud(rid):
            entries.append((pr, rid))
    return entries


def _macos_job_targets_cloud(run_id: int) -> bool:
    """Return True iff the run's macOS job has labels that include a cloud
    target (macos-15 or nscloud/namespace-profile-*). Returns False if the
    macOS job hasn't been dispatched yet (resolve-provider still running) —
    we don't know its target yet."""
    try:
        labels_str = _gh([
            f"repos/{REPO}/actions/runs/{run_id}/jobs",
            "--jq",
            '[.jobs[] | select(.name | startswith("macOS")) | .labels] '
            '| flatten | join(",")',
        ])
    except subprocess.SubprocessError:
        return False
    if not labels_str:
        return False
    if "self-hosted" in labels_str:
        return False  # already on local; nothing to reroute
    cloud_markers = ("macos-15", "nscloud-", "namespace-profile-")
    return any(marker in labels_str for marker in cloud_markers)


def reroute_to_local(pr_number: int) -> bool:
    """Invoke `pulp macos retarget --pr <N> --to local`. Returns True on
    success."""
    try:
        result = subprocess.run(
            ["pulp", "macos", "retarget", "--pr", str(pr_number), "--to", "local"],
            check=False,
            capture_output=True,
            text=True,
            timeout=60,
        )
    except subprocess.SubprocessError as exc:
        logging.error("pulp macos retarget failed: %s", exc)
        return False
    if result.returncode != 0:
        logging.error(
            "pulp macos retarget PR #%d exit %d: %s",
            pr_number,
            result.returncode,
            result.stderr.strip(),
        )
        return False
    logging.info("Rerouted PR #%d to local. Output: %s",
                 pr_number, result.stdout.strip())
    return True


class FlapGuard:
    """Remembers when each PR was last rerouted; suppresses re-action
    within `window_seconds` to avoid thrashing."""

    def __init__(self, window_seconds: int):
        self.window = window_seconds
        self._last: OrderedDict[int, float] = OrderedDict()

    def can_reroute(self, pr_number: int) -> bool:
        now = time.time()
        last = self._last.get(pr_number, 0.0)
        return (now - last) >= self.window

    def record(self, pr_number: int) -> None:
        self._last[pr_number] = time.time()
        # Trim entries older than 2x window to bound memory.
        cutoff = time.time() - (2 * self.window)
        while self._last and next(iter(self._last.values())) < cutoff:
            self._last.popitem(last=False)


def watch(interval: int, flap_window: int) -> None:
    guard = FlapGuard(window_seconds=flap_window)
    logging.info(
        "macos-reroute-watcher: interval=%ds flap_window=%ds repo=%s",
        interval, flap_window, REPO,
    )
    while True:
        try:
            tick(guard)
        except KeyboardInterrupt:
            logging.info("interrupted; exiting")
            return
        except Exception as exc:  # noqa: BLE001
            logging.exception("tick error (will continue): %s", exc)
        time.sleep(interval)


def tick(guard: FlapGuard) -> None:
    busy = local_is_busy()
    if busy is None:
        logging.warning("local-busy probe failed; skipping tick")
        return
    if busy:
        logging.debug("local busy; nothing to do")
        return

    candidates = list_queued_cloud_bat_runs()
    if not candidates:
        logging.debug("no cloud-bound queued BAT runs; nothing to do")
        return

    for pr, run_id in candidates:
        if not guard.can_reroute(pr):
            logging.info("PR #%d recently rerouted; skipping (flap-guard)", pr)
            continue
        logging.info(
            "local idle + PR #%d (run %d) queued to cloud → rerouting",
            pr, run_id,
        )
        if reroute_to_local(pr):
            guard.record(pr)
            return  # one reroute per tick; let the next tick reassess


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Opportunistic macOS-overflow reroute watcher.",
    )
    parser.add_argument(
        "--interval",
        type=int,
        default=30,
        help="Seconds between polling ticks (default: 30)",
    )
    parser.add_argument(
        "--flap-window",
        type=int,
        default=300,
        help="Suppress re-reroute of the same PR within this many seconds "
             "(default: 300)",
    )
    parser.add_argument(
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
    )
    args = parser.parse_args(argv)

    logging.basicConfig(
        format="%(asctime)s %(levelname)s %(message)s",
        level=getattr(logging, args.log_level),
    )

    watch(interval=args.interval, flap_window=args.flap_window)
    return 0


if __name__ == "__main__":
    sys.exit(main())
