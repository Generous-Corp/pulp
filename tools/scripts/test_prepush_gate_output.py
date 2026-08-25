#!/usr/bin/env python3
"""Prove pre-push gates survive saturated nonblocking caller output."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
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

    # The hook validates checked-out HEAD. Git may ask it to push another local
    # ref, so prove the pushed-ref contract fails closed before any source gate
    # can accidentally authorize the wrong commit.
    with tempfile.TemporaryDirectory(prefix="pulp-prepush-refs-") as temp:
        repo = Path(temp)
        (repo / ".githooks" / "lib").mkdir(parents=True)
        shutil.copy2(GATE_OUTPUT_HELPER, repo / ".githooks" / "lib" / "gate-output.sh")

        def git(*args: str) -> subprocess.CompletedProcess[str]:
            return subprocess.run(
                ("git", *args),
                cwd=repo,
                text=True,
                capture_output=True,
                check=True,
            )

        git("init", "-q")
        git("config", "user.name", "Pulp Pre-push Test")
        git("config", "user.email", "prepush-test@example.invalid")
        (repo / "fixture.txt").write_text("first\n", encoding="utf-8")
        git("add", "fixture.txt")
        git("commit", "-qm", "first")
        prior = git("rev-parse", "HEAD").stdout.strip()
        (repo / "fixture.txt").write_text("second\n", encoding="utf-8")
        git("commit", "-qam", "second")
        head = git("rev-parse", "HEAD").stdout.strip()
        zeros = "0" * 40

        def run_hook(update_records: str) -> subprocess.CompletedProcess[str]:
            return subprocess.run(
                ("bash", str(PREPUSH)),
                cwd=repo,
                input=update_records,
                text=True,
                capture_output=True,
                check=False,
                timeout=10,
                env={**os.environ, "PULP_SKIP_PR_BATCH_ADVICE": "1"},
            )

        exact = run_hook(f"refs/heads/main {head} refs/heads/main {prior}\n")
        if exact.returncode != 0:
            print("FAIL: exact checked-out HEAD push was refused", file=sys.stderr)
            return 1

        mismatched = run_hook(
            f"refs/heads/prior {prior} refs/heads/prior {zeros}\n"
        )
        if mismatched.returncode == 0 or "check out that branch" not in mismatched.stderr:
            print("FAIL: non-HEAD branch push did not fail closed", file=sys.stderr)
            return 1

        multi_ref = run_hook(
            f"refs/heads/main {head} refs/heads/main {prior}\n"
            f"refs/heads/prior {prior} refs/heads/prior {zeros}\n"
        )
        if multi_ref.returncode == 0:
            print("FAIL: mixed-commit multi-ref push did not fail closed", file=sys.stderr)
            return 1

        truncated = run_hook(f"refs/heads/main {head}\n")
        if truncated.returncode == 0 or "malformed pushed-ref record" not in truncated.stderr:
            print("FAIL: truncated pushed-ref input did not fail closed", file=sys.stderr)
            return 1

        extra_field = run_hook(
            f"refs/heads/main {head} refs/heads/main {prior} unexpected\n"
        )
        if extra_field.returncode == 0 or "malformed pushed-ref record" not in extra_field.stderr:
            print("FAIL: pushed-ref input with extra fields did not fail closed", file=sys.stderr)
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
