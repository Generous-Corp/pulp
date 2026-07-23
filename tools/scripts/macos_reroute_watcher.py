#!/usr/bin/env python3
"""Opportunistic macOS-overflow reroute watcher (pulp task #22).

Polls (a) free local macOS capacity and (b) the repo's queued Build-and-Test
workflow_runs that have a macOS job dispatched to a cloud target (macos-15 or
a Namespace selector). When there is free local capacity AND a cloud-bound job
is still queued (hasn't been picked up by its cloud runner pool yet), the
watcher claws it back to local via `pulp macos retarget --pr N --to local` —
which cancels the cloud dispatch and re-fires the macOS leg on the local Mac.

Why this exists: macOS builds run ~3× faster on the warm-cache local Mac
than on a cold GH-hosted `macos-15`. The default overflow logic in
`.github/workflows/build.yml` is "local-first when idle; cloud when busy".
That's correct, but coarse — once a PR is dispatched to cloud, it stays
there even if local frees up before the cloud runner picks it up. The
watcher captures those near-misses.

Capacity model (#3299). "Free local capacity" is VM-slot-aware, not just a
single runner's busy/idle. Each configured host is either bare-metal (one slot,
gated by the local Runner.Worker busy probe) or runs ephemeral Tart VMs (`cap`
slots — the macOS kernel caps 2 running macOS VMs per host; see the macOS CI
isolation plan Appendix D — minus the count of running macOS VMs). Free
capacity is the sum of free slots across hosts. With no `--hosts-config` the
default is a single local bare-metal slot, i.e. EXACTLY the pre-#3299 behavior,
so this is safe to run before the Tart-VM cutover and grows into it.

Safety properties:

- Only acts when there is a **free macOS slot** — bare-metal idle, or a Tart
  host below its VM cap. Never preempts a live local job or overbooks a host.

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

REPO = "Generous-Corp/pulp"
ACTIONS_RUNNER_WORKSPACE_MARKER = "actions-runner/_work/pulp"

# Tart VM capacity model (#3299). The macOS kernel caps 2 running macOS VMs
# per host (macOS CI isolation plan Appendix D); Linux/Windows guests are
# uncapped and don't count. A host either runs jobs directly (bare-metal, one
# slot) or via ephemeral Tart VMs (`cap` slots). The default config is a single
# local bare-metal slot — identical to the pre-#3299 single-runner behavior.
DEFAULT_TART_CAP = 2
DEFAULT_HOSTS = [{"name": "local", "mode": "baremetal", "cap": 1, "ssh": None}]
_SSH_OPTS = [
    "-o", "StrictHostKeyChecking=no",
    "-o", "UserKnownHostsFile=/dev/null",
    "-o", "LogLevel=ERROR",
    "-o", "ConnectTimeout=10",
    "-o", "BatchMode=yes",
]


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
    workspace for Generous-Corp/pulp.
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


def count_running_macos_vms(ssh: Optional[str] = None) -> Optional[int]:
    """Count currently-running macOS Tart VMs on a host (local, or the `ssh`
    target `user@host`). Linux/Windows guests don't count against the macOS
    2-VM kernel cap, so they're excluded. Returns None if the probe fails — the
    caller treats unknown capacity conservatively and never force-reclaims."""
    list_cmd = ["tart", "list", "--format", "json"]
    cmd = ["ssh", *_SSH_OPTS, ssh, " ".join(list_cmd)] if ssh else list_cmd
    try:
        result = subprocess.run(
            cmd, check=True, capture_output=True, text=True, timeout=20,
        )
    except subprocess.SubprocessError as exc:
        logging.warning("tart list (ssh=%s) failed: %s", ssh, exc)
        return None
    try:
        vms = json.loads(result.stdout or "[]")
    except json.JSONDecodeError as exc:
        logging.warning("tart list JSON parse failed (ssh=%s): %s", ssh, exc)
        return None
    running = 0
    for vm in vms if isinstance(vms, list) else []:
        state = str(vm.get("State", vm.get("state", ""))).lower()
        if not state.startswith("run"):
            continue
        # Unknown OS counts as macOS (conservative: report fewer free slots, so
        # the watcher reclaims less aggressively rather than overbooking a host).
        os_name = str(vm.get("OS", vm.get("os", "darwin"))).lower()
        if os_name in ("", "darwin", "macos"):
            running += 1
    return running


def _host_free_slots(host: dict) -> Optional[int]:
    """Free macOS execution slots on one host. Bare-metal hosts have one slot,
    gated by the local Runner.Worker busy probe (local-only by definition). Tart
    hosts have `cap` slots minus the count of running macOS VMs. None on probe
    failure (so an unreachable host doesn't masquerade as zero free capacity)."""
    mode = str(host.get("mode", "baremetal")).lower()
    if mode == "baremetal":
        # Bare-metal capacity == the local runner's busy/idle state. This mode
        # is local-only: the probe reads THIS machine's process table.
        busy = local_is_busy()
        if busy is None:
            return None
        return 0 if busy else 1
    running = count_running_macos_vms(ssh=host.get("ssh"))
    if running is None:
        return None
    cap = int(host.get("cap", DEFAULT_TART_CAP))
    return max(0, cap - running)


def free_macos_slots(hosts: list[dict]) -> Optional[int]:
    """Total free macOS slots across all configured hosts (#3299). Sums the
    free slots of every host whose probe succeeded; returns None only if EVERY
    host probe failed, so the watcher skips the tick rather than acting on
    unknown capacity."""
    total = 0
    any_ok = False
    for host in hosts:
        free = _host_free_slots(host)
        if free is None:
            continue
        any_ok = True
        total += free
    return total if any_ok else None


def load_hosts_config(path: Optional[str]) -> list[dict]:
    """Load the hosts capacity config from a JSON file, or return DEFAULT_HOSTS
    (a single local bare-metal slot — exactly the pre-#3299 behavior) when no
    path is given. File shape: `{"hosts": [{name, mode, cap, ssh}, ...]}`."""
    if not path:
        return DEFAULT_HOSTS
    data = json.loads(Path(path).read_text())
    hosts = data.get("hosts") if isinstance(data, dict) else data
    if not isinstance(hosts, list) or not hosts:
        raise ValueError(f"hosts config {path!r} has no non-empty 'hosts' list")
    return hosts


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


def watch(interval: int, flap_window: int,
          hosts: Optional[list[dict]] = None) -> None:
    hosts = hosts or DEFAULT_HOSTS
    guard = FlapGuard(window_seconds=flap_window)
    logging.info(
        "macos-reroute-watcher: interval=%ds flap_window=%ds hosts=%d repo=%s",
        interval, flap_window, len(hosts), REPO,
    )
    while True:
        try:
            tick(guard, hosts)
        except KeyboardInterrupt:
            logging.info("interrupted; exiting")
            return
        except Exception as exc:  # noqa: BLE001
            logging.exception("tick error (will continue): %s", exc)
        time.sleep(interval)


def tick(guard: FlapGuard, hosts: Optional[list[dict]] = None) -> None:
    hosts = hosts or DEFAULT_HOSTS
    free = free_macos_slots(hosts)
    if free is None:
        logging.warning("capacity probe failed; skipping tick")
        return
    if free <= 0:
        logging.debug("no free macOS slot (capacity full); nothing to do")
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
        "--hosts-config",
        default=None,
        help="JSON file describing macOS capacity hosts "
             "{\"hosts\": [{name, mode, cap, ssh}, ...]}. Default: a single "
             "local bare-metal slot (pre-#3299 single-runner behavior).",
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

    hosts = load_hosts_config(args.hosts_config)
    watch(interval=args.interval, flap_window=args.flap_window, hosts=hosts)
    return 0


if __name__ == "__main__":
    sys.exit(main())
