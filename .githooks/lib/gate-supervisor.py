#!/usr/bin/env python3
"""Run one pre-push gate without letting its descendants outlive the push.

Git hooks and their callers do not form an automatic cancellation tree on
POSIX.  A killed ``git push`` can leave the hook, a coverage build, and its
governor lease running under PID 1.  This helper gives each gate an isolated
process group, watches both the hook and its original Git parent, and tears
down the whole group when either ownership edge disappears.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


# A one-second ownership probe stops an orphan promptly without spawning `ps`
# ten times per second throughout a legitimate multi-minute coverage build.
POLL_SECONDS = 1.0
TERMINATION_GRACE_SECONDS = 3.0
# Interactive shells and process managers commonly escalate a cancelled hook
# group to SIGKILL after roughly one second.  The supervisor lives in that hook
# group while the gate is intentionally isolated, so its signal path must
# finish before that external escalation can kill the supervisor and strand
# the gate under PID 1.
SIGNAL_TERMINATION_GRACE_SECONDS = 0.25


def parent_pid(pid: int) -> int | None:
    """Return a live process's current parent, or None when it is gone."""
    result = subprocess.run(
        ["ps", "-o", "ppid=", "-p", str(pid)],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0 or not result.stdout.strip():
        return None
    try:
        return int(result.stdout.strip())
    except ValueError:
        return None


def group_exists(process_group: int) -> bool:
    try:
        os.killpg(process_group, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        # The numeric group still exists; do not mistake an authorization
        # boundary for successful cleanup.
        return True


def terminate_group(
    process: subprocess.Popen[bytes],
    first_signal: signal.Signals = signal.SIGTERM,
    grace_seconds: float = TERMINATION_GRACE_SECONDS,
) -> None:
    """Signal, bound, and reap the gate's complete isolated process group."""
    process_group = process.pid
    try:
        os.killpg(process_group, first_signal)
    except ProcessLookupError:
        process.wait()
        return

    deadline = time.monotonic() + grace_seconds
    while time.monotonic() < deadline:
        process.poll()
        if not group_exists(process_group):
            process.wait()
            return
        time.sleep(0.05)

    try:
        os.killpg(process_group, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv[1:])
    if args.command and args.command[0] == "--":
        args.command = args.command[1:]
    if not args.command:
        parser.error("a command must follow --")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    owner_pid = os.getppid()
    caller_pid = parent_pid(owner_pid)
    if caller_pid is None or caller_pid < 2:
        print("[pre-push] gate supervisor could not identify its caller", file=sys.stderr)
        return 2
    pending_signal: int | None = None
    process: subprocess.Popen[bytes] | None = None

    def remember_signal(signum: int, _frame: object) -> None:
        nonlocal pending_signal
        pending_signal = signum
        # Python may restart time.sleep() after a handled signal. Do the
        # bounded teardown in the handler once the gate exists so an external
        # TERM->KILL escalation cannot kill this supervisor during that sleep
        # and strand the isolated gate group.
        if process is not None:
            terminate_group(
                process,
                signal.Signals(signum),
                grace_seconds=SIGNAL_TERMINATION_GRACE_SECONDS,
            )
            raise SystemExit(128 + signum)

    for signum in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, remember_signal)

    log_path = Path(args.log)
    try:
        with log_path.open("wb") as gate_log:
            try:
                process = subprocess.Popen(
                    args.command,
                    stdout=gate_log,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
            except FileNotFoundError as exc:
                gate_log.write(f"{exc}\n".encode())
                return 127
            except PermissionError as exc:
                gate_log.write(f"{exc}\n".encode())
                return 126

            while process.poll() is None:
                if pending_signal is not None:
                    forwarded = signal.Signals(pending_signal)
                    terminate_group(
                        process,
                        forwarded,
                        grace_seconds=SIGNAL_TERMINATION_GRACE_SECONDS,
                    )
                    return 128 + pending_signal

                # os.getppid() catches the hook itself disappearing. Checking
                # the hook's current parent catches the incident's subtler
                # shape: Git died, the hook survived under PID 1, and its
                # foreground build otherwise continued for many minutes.
                owner_lost = os.getppid() != owner_pid
                caller_lost = (
                    parent_pid(owner_pid) != caller_pid
                )
                if owner_lost or caller_lost:
                    reason = "hook exited" if owner_lost else "git caller exited"
                    gate_log.write(
                        f"[pre-push] {reason}; terminating gate process group\n".encode()
                    )
                    gate_log.flush()
                    terminate_group(process)
                    return 128 + signal.SIGTERM
                time.sleep(POLL_SECONDS)

            status = process.wait()
            # A successful gate leader must not be allowed to daemonize a
            # compiler, heartbeat, or lease owner. Clean any group members it
            # forgot before returning the leader's exact status.
            if group_exists(process.pid):
                terminate_group(process)
            return 128 - status if status < 0 else status
    except OSError as exc:
        # Fail closed after launch: even the ownership probe can transiently
        # fail to spawn under host process pressure. Never let that failure
        # strand the isolated gate group or its governor lease.
        if process is not None:
            terminate_group(process)
        print(f"[pre-push] gate supervisor could not use {log_path}: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
