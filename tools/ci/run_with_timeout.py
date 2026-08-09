#!/usr/bin/env python3
"""Run a command with a portable process-group timeout.

macOS does not ship GNU ``timeout``. This helper preserves its useful exit-124
contract while ensuring a timed-out build cannot leave compiler children alive.
"""

from __future__ import annotations

import os
import signal
import subprocess
import sys
import time


TERMINATION_GRACE_SECONDS = 5.0


def terminate_process_group(process: subprocess.Popen[bytes]) -> None:
    """Terminate every process in the command's session, including orphans."""
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        process.wait()
        return

    deadline = time.monotonic() + TERMINATION_GRACE_SECONDS
    while time.monotonic() < deadline:
        leader_exited = process.poll() is not None
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            return
        except PermissionError:
            # Darwin can report EPERM for a dying group between delivery of
            # SIGTERM and the leader becoming observable as reaped. Confirm
            # that the leader exits before treating the original group as
            # gone; never signal an inaccessible/reused numeric PGID.
            if not leader_exited:
                process.wait(timeout=TERMINATION_GRACE_SECONDS)
            return
        time.sleep(0.05)

    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(f"usage: {argv[0]} SECONDS COMMAND [ARG ...]", file=sys.stderr)
        return 2
    try:
        seconds = float(argv[1])
    except ValueError:
        print(f"invalid timeout: {argv[1]}", file=sys.stderr)
        return 2
    if seconds <= 0:
        print("timeout must be greater than zero", file=sys.stderr)
        return 2

    process = subprocess.Popen(argv[2:], start_new_session=True)
    try:
        return process.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        print(f"command exceeded {seconds:g}s; terminating process group", file=sys.stderr)
        terminate_process_group(process)
        return 124


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
