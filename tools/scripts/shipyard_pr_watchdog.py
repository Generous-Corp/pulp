#!/usr/bin/env python3
"""Run ``shipyard pr`` with a conservative orphaned-pipe watchdog.

The watchdog only intervenes when Shipyard has emitted no output, has no live
descendants, and has consumed effectively no CPU for the full stall window.
It restarts once so completed compiler and coverage artifacts remain reusable.
"""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass


@dataclass
class Activity:
    last_output: float


def process_snapshot() -> tuple[dict[int, list[int]], dict[int, float]] | None:
    result = subprocess.run(
        ["ps", "-axo", "pid=,ppid=,%cpu="],
        check=False,
        capture_output=True,
        text=True,
    )
    children: dict[int, list[int]] = {}
    cpu: dict[int, float] = {}
    if result.returncode != 0:
        return None
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) != 3:
            continue
        try:
            pid, ppid, usage = int(fields[0]), int(fields[1]), float(fields[2])
        except ValueError:
            continue
        children.setdefault(ppid, []).append(pid)
        cpu[pid] = usage
    return children, cpu


def descendants(root: int, children: dict[int, list[int]]) -> set[int]:
    found: set[int] = set()
    pending = list(children.get(root, ()))
    while pending:
        pid = pending.pop()
        if pid in found:
            continue
        found.add(pid)
        pending.extend(children.get(pid, ()))
    return found


def stream_output(pipe: object, activity: Activity) -> None:
    for line in iter(pipe.readline, ""):  # type: ignore[attr-defined]
        activity.last_output = time.monotonic()
        sys.stdout.write(line)
        sys.stdout.flush()


def stop_process_group(process: subprocess.Popen[str], grace: float) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGINT)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=grace)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            return
        process.wait()


def capture_diagnostic(pid: int) -> None:
    if sys.platform != "darwin" or os.environ.get("PULP_SHIPYARD_WATCHDOG_DIAGNOSTICS") == "0":
        return
    fd, path = tempfile.mkstemp(prefix="shipyard-watchdog-", suffix=".sample.txt")
    with os.fdopen(fd, "w") as output:
        subprocess.run(
            ["sample", str(pid), "1", "1"],
            check=False,
            stdout=output,
            stderr=subprocess.STDOUT,
        )
    print(f"shipyard-watchdog: diagnostic saved to {path}", file=sys.stderr)


def run_once(command: list[str], args: argparse.Namespace) -> tuple[int, bool]:
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        start_new_session=True,
    )
    assert process.stdout is not None
    activity = Activity(time.monotonic())
    reader = threading.Thread(target=stream_output, args=(process.stdout, activity), daemon=True)
    reader.start()
    quiet_since: float | None = None

    while process.poll() is None:
        time.sleep(args.poll_seconds)
        now = time.monotonic()
        snapshot = process_snapshot()
        if snapshot is None:
            quiet_since = None
            continue
        children, cpu = snapshot
        has_descendants = bool(descendants(process.pid, children))
        idle_cpu = cpu.get(process.pid, 0.0) <= args.max_idle_cpu
        quiet = now - activity.last_output >= args.stall_seconds
        candidate = quiet and not has_descendants and idle_cpu
        quiet_since = quiet_since or now if candidate else None
        if candidate and quiet_since is not None and now - quiet_since >= args.confirm_seconds:
            print(
                f"shipyard-watchdog: stalled for {args.stall_seconds + args.confirm_seconds:.0f}s "
                "with no descendants or output; capturing diagnostics and stopping",
                file=sys.stderr,
            )
            capture_diagnostic(process.pid)
            stop_process_group(process, args.stop_grace_seconds)
            return process.returncode or 1, True

    reader.join(timeout=args.stop_grace_seconds)
    return process.returncode or 0, False


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stall-seconds", type=float, default=300)
    parser.add_argument("--confirm-seconds", type=float, default=60)
    parser.add_argument("--poll-seconds", type=float, default=15)
    parser.add_argument("--stop-grace-seconds", type=float, default=10)
    parser.add_argument("--max-idle-cpu", type=float, default=0.5)
    parser.add_argument("--max-restarts", type=int, default=1)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.command[:1] == ["--"]:
        args.command = args.command[1:]
    if not args.command:
        args.command = ["shipyard", "pr"]
    for name in ("stall_seconds", "confirm_seconds", "poll_seconds", "stop_grace_seconds"):
        if getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')} must be positive")
    if args.max_restarts < 0:
        parser.error("--max-restarts must be non-negative")
    if args.max_idle_cpu < 0:
        parser.error("--max-idle-cpu must be non-negative")
    return args


def main() -> int:
    args = parse_args()
    attempts = args.max_restarts + 1
    for attempt in range(attempts):
        code, stalled = run_once(args.command, args)
        if not stalled:
            return code
        if attempt + 1 < attempts:
            print(
                f"shipyard-watchdog: restarting once ({attempt + 2}/{attempts}); "
                "completed build artifacts will be reused",
                file=sys.stderr,
            )
    print("shipyard-watchdog: repeated stall; refusing to loop", file=sys.stderr)
    return 124


if __name__ == "__main__":
    raise SystemExit(main())
