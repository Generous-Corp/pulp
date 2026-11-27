#!/usr/bin/env python3
"""Run a command at low priority under a wall-time cap that survives this wrapper's own death.

    run_capped.py --seconds 600 --nice 19 [--background] -- python3 suite.py

The command runs in its own session so the whole group can be signalled. Exit
status is the command's, or 124 when the cap expired. A cap that fires means the
command did not finish, so a caller must treat 124 as "not checked", never as a
pass. The group is always stopped the same way, whatever ended the run (the cap,
SIGTERM, SIGHUP, SIGINT): TERM to the group, a short grace, then KILL to the
whole group whether or not the leader has exited, so a descendant that ignores
TERM cannot outlive the cap. --background also starts the command at background
QoS (macOS ``taskpolicy -b``), which bounds its I/O and CPU priority beyond nice.
Used by gates.sh on gate hosts, where build-free Python suites may run but must
not starve the gate VMs.
"""

from __future__ import annotations

import argparse
import os
import shutil
import signal
import subprocess
import sys
import time

TIMED_OUT = 124
GRACE_S = float(os.environ.get("PULP_RUN_CAPPED_GRACE", "5"))
SETTLE_S = 1.0


def stop_group(proc: subprocess.Popen) -> None:
    """TERM the group, wait up to the grace for the leader, then KILL the whole group."""
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        proc.wait(timeout=GRACE_S)
        time.sleep(SETTLE_S)  # a descendant that honours TERM needs a moment after the leader
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    proc.wait()


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seconds", type=float, required=True)
    ap.add_argument("--nice", type=int, default=0)
    ap.add_argument("--background", action="store_true")
    ap.add_argument("command", nargs=argparse.REMAINDER)
    args = ap.parse_args(argv[1:])
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        ap.error("a command is required after --")
    shown = command[0]
    taskpolicy = shutil.which("taskpolicy") if args.background else None
    if taskpolicy:
        command = [taskpolicy, "-b", *command]

    def lower_priority() -> None:
        if args.nice:
            os.nice(args.nice)

    try:
        proc = subprocess.Popen(command, preexec_fn=lower_priority,  # noqa: PLW1509 (POSIX only, by design)
                                start_new_session=True)
    except OSError as exc:
        print(f"run_capped: {shown} could not run: {exc}", file=sys.stderr)
        return 127

    def on_signal(signum: int, _frame: object) -> None:
        raise SystemExit(128 + signum)

    for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(sig, on_signal)
    try:
        return proc.wait(timeout=args.seconds)
    except subprocess.TimeoutExpired:
        print(f"run_capped: wall-time cap of {args.seconds:g}s expired; stopping {shown}", file=sys.stderr)
        stop_group(proc)
        return TIMED_OUT
    except BaseException:
        stop_group(proc)
        raise


if __name__ == "__main__":
    sys.exit(main(sys.argv))
