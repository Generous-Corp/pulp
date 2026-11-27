#!/usr/bin/env python3
"""Run a command at low priority under a wall-time cap, killing its whole process group.

    run_capped.py --seconds 600 --nice 19 -- python3 suite.py

Exit status is the command's, or 124 when the cap expired (the group is TERMed,
then KILLed after a short grace). A cap that fires means the command did not
finish, so a caller must treat 124 as "not checked", never as a pass or a fail.
Used by gates.sh on gate hosts, where build-free Python suites may run but must
not starve the gate VMs of CPU or hold a terminal for longer than a budget.
"""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys

TIMED_OUT = 124
GRACE_S = 5


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seconds", type=float, required=True)
    ap.add_argument("--nice", type=int, default=0)
    ap.add_argument("command", nargs=argparse.REMAINDER)
    args = ap.parse_args(argv[1:])
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        ap.error("a command is required after --")

    def lower_priority() -> None:
        os.setsid()
        if args.nice:
            os.nice(args.nice)

    try:
        proc = subprocess.Popen(command, preexec_fn=lower_priority)  # noqa: PLW1509 (POSIX only, by design)
    except OSError as exc:
        print(f"run_capped: {command[0]} could not run: {exc}", file=sys.stderr)
        return 127
    try:
        return proc.wait(timeout=args.seconds)
    except subprocess.TimeoutExpired:
        print(f"run_capped: wall-time cap of {args.seconds:g}s expired; stopping {command[0]}", file=sys.stderr)
    except BaseException:
        os.killpg(proc.pid, signal.SIGTERM)
        raise
    os.killpg(proc.pid, signal.SIGTERM)
    try:
        proc.wait(timeout=GRACE_S)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait()
    return TIMED_OUT


if __name__ == "__main__":
    sys.exit(main(sys.argv))
