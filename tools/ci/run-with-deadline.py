#!/usr/bin/env python3
"""Run a command with a hard deadline and clean up its whole process group."""

import argparse
import os
import signal
import subprocess
import sys
import time


def terminate_group(process, grace_seconds=5):
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return

    deadline = time.monotonic() + grace_seconds
    while time.monotonic() < deadline:
        process.poll()
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            break
        except PermissionError:
            pass
        time.sleep(0.05)
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    process.wait()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if args.timeout <= 0 or not command:
        parser.error("--timeout must be positive and a command must follow --")
    if os.name != "posix":
        parser.error("process-group deadlines require a POSIX runner")

    process = subprocess.Popen(command, start_new_session=True)

    previous_handlers = {}
    def terminate_from_signal(signum, _frame):
        terminate_group(process)
        raise SystemExit(128 + signum)

    for signum in (signal.SIGINT, signal.SIGTERM):
        previous_handlers[signum] = signal.signal(signum, terminate_from_signal)
    try:
        try:
            return process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            print(
                f"deadline expired after {args.timeout:g}s: {' '.join(command)}",
                file=sys.stderr,
                flush=True,
            )
            terminate_group(process)
            return 124
    finally:
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)


if __name__ == "__main__":
    raise SystemExit(main())
