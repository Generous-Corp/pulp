#!/usr/bin/env python3
"""Prove pre-push gates survive saturated nonblocking caller output."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
PREPUSH = ROOT / ".githooks/pre-push"
GATE_OUTPUT_HELPER = ROOT / ".githooks/lib/gate-output.sh"


def fill_nonblocking_pipe(write_fd: int) -> None:
    os.set_blocking(write_fd, False)
    payload = b"x" * 65_536
    while True:
        try:
            os.write(write_fd, payload)
        except BlockingIOError:
            return


def main() -> int:
    prepush_source = PREPUSH.read_text(encoding="utf-8")
    helper_source = GATE_OUTPUT_HELPER.read_text(encoding="utf-8")
    if 'source "$ROOT/.githooks/lib/gate-output.sh"' not in prepush_source:
        print("FAIL: pre-push does not source the gate-output helper", file=sys.stderr)
        return 1
    if '"$@" >"$gate_log" 2>&1' not in helper_source:
        print("FAIL: gate output is not captured to a regular file", file=sys.stderr)
        return 1
    if 'cat "$gate_log" >&2 || true' not in helper_source:
        print("FAIL: diagnostic replay can override gate status", file=sys.stderr)
        return 1
    captured_diff_cover = any(
        line.strip().startswith("if ! run_gate_captured ")
        and 'bash "$DIFF_COVER_SH"' in line
        and line.strip().endswith("; then")
        for line in prepush_source.splitlines()
    )
    if not captured_diff_cover:
        print("FAIL: slow diff-cover output bypasses gate capture", file=sys.stderr)
        return 1

    read_fd, write_fd = os.pipe()
    try:
        fill_nonblocking_pipe(write_fd)
        script = """
set -u
PREPUSH_GATE_LOG_DIR="$(mktemp -d)"
trap 'rm -rf -- "$PREPUSH_GATE_LOG_DIR"' EXIT
source "$1"
run_gate_captured "$2" -c \
  'import sys; print("healthy gate diagnostic", file=sys.stderr)'
"""
        result = subprocess.run(
            (
                "bash",
                "-c",
                script,
                "gate-output-test",
                str(GATE_OUTPUT_HELPER),
                sys.executable,
            ),
            stdout=subprocess.DEVNULL,
            stderr=write_fd,
            check=False,
            timeout=10,
        )
        if result.returncode != 0:
            print(
                "FAIL: captured healthy gate failed with a saturated "
                f"nonblocking stderr (exit {result.returncode})",
                file=sys.stderr,
            )
            return 1

        failing = subprocess.run(
            (
                "bash",
                "-c",
                script.replace(
                    "'import sys; print(\"healthy gate diagnostic\", file=sys.stderr)'",
                    "'import sys; print(\"failing gate diagnostic\", file=sys.stderr); sys.exit(7)'",
                ),
                "gate-output-test",
                str(GATE_OUTPUT_HELPER),
                sys.executable,
            ),
            stdout=subprocess.DEVNULL,
            stderr=write_fd,
            check=False,
            timeout=10,
        )
        if failing.returncode != 7:
            print(
                f"FAIL: captured failing gate returned {failing.returncode}, expected 7",
                file=sys.stderr,
            )
            return 1
    finally:
        os.close(write_fd)
        os.close(read_fd)

    with tempfile.TemporaryDirectory(prefix="pulp-prepush-output-test-") as temp:
        gate_dir = Path(temp) / "gates"
        normal_script = f"""
set -u
PREPUSH_GATE_LOG_DIR={str(gate_dir)!r}
mkdir -p "$PREPUSH_GATE_LOG_DIR"
trap 'rm -rf -- "$PREPUSH_GATE_LOG_DIR"' EXIT
source "$1"
run_gate_captured "$2" -c \
  'import sys; print("replayed diagnostic", file=sys.stderr)'
"""
        normal = subprocess.run(
            (
                "bash",
                "-c",
                normal_script,
                "gate-output-test",
                str(GATE_OUTPUT_HELPER),
                sys.executable,
            ),
            text=True,
            capture_output=True,
            check=False,
            timeout=10,
        )
        if normal.returncode != 0 or "replayed diagnostic" not in normal.stderr:
            print("FAIL: normal gate diagnostics were not replayed", file=sys.stderr)
            return 1
        if gate_dir.exists():
            print("FAIL: captured gate logs were not cleaned on exit", file=sys.stderr)
            return 1

    print("prepush-gate-output: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
