"""Run local-ci commands through Terminal.app for macOS TCC-scoped capture."""

from __future__ import annotations

from collections.abc import Callable, Mapping, Sequence
from pathlib import Path
import json
import os
import shlex
import subprocess
import tempfile
import time
import uuid


TERMINAL_REENTRY_ENV = "PULP_LOCAL_CI_TERMINAL_REENTRY"


def strip_run_in_terminal_args(argv: Sequence[str]) -> list[str]:
    return [arg for arg in argv if arg != "--run-in-terminal"]


def should_reinvoke_in_terminal(
    *,
    requested: bool,
    sys_platform: str,
    environ: Mapping[str, str] | None = None,
) -> bool:
    environ = environ or os.environ
    return bool(requested) and sys_platform == "darwin" and environ.get(TERMINAL_REENTRY_ENV) != "1"


def terminal_shell_script(
    *,
    cwd: Path,
    python_executable: str,
    script_path: Path,
    argv: Sequence[str],
    stdout_path: Path,
    stderr_path: Path,
    returncode_path: Path,
    title: str | None = None,
) -> str:
    command = [
        "env",
        f"{TERMINAL_REENTRY_ENV}=1",
        python_executable,
        str(script_path),
        *strip_run_in_terminal_args(argv),
    ]
    close_script = (
        f'tell application "Terminal" to close (first window whose name contains {json.dumps(title)}) saving no'
        if title
        else None
    )
    parts = [
        f"cd {shlex.quote(str(cwd))}",
        f"printf '\\033]0;%s\\007' {shlex.quote(title)}" if title else None,
        "(/usr/bin/caffeinate -u -t 60 >/dev/null 2>&1 &)",
        f"{' '.join(shlex.quote(part) for part in command)} "
        f"> {shlex.quote(str(stdout_path))} "
        f"2> {shlex.quote(str(stderr_path))}",
        f"printf '%s\\n' \"$?\" > {shlex.quote(str(returncode_path))}",
        f"(sleep 0.2; /usr/bin/osascript -e {shlex.quote(close_script)} >/dev/null 2>&1) &" if close_script else None,
        "exit",
    ]
    return "; ".join(part for part in parts if part)


def close_terminal_windows_with_title(
    title_contains: str,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess] = subprocess.run,
    sleep_fn: Callable[[float], None] = time.sleep,
    attempts: int = 5,
    allow_terminate_with_nonproof_windows: bool = False,
) -> dict:
    script = "\n".join(
        [
            'tell application "Terminal"',
            "    set closedCount to 0",
            "    set otherCount to 0",
            "    set otherProofCount to 0",
            "    repeat with w in (every window)",
            "        set windowName to name of w",
            f"        if windowName contains {json.dumps(title_contains)} then",
            "            close w saving no",
            "            set closedCount to closedCount + 1",
            '        else if windowName contains "Pulp Video Proof" then',
            "            set otherProofCount to otherProofCount + 1",
            "        else",
            "            set otherCount to otherCount + 1",
            "        end if",
            "    end repeat",
            "    if closedCount > 0 and otherCount = 0 then",
            "        quit",
            "    end if",
            "    return closedCount",
            "end tell",
        ]
    )
    result = subprocess.CompletedProcess(["osascript", "-e", script], 1, "", "")
    closed_count = 0
    terminated_terminal = False
    terminate_returncode: int | None = None
    remaining_proof_count: int | None = None
    other_window_count: int | None = None
    for attempt in range(max(1, attempts)):
        if attempt:
            sleep_fn(0.2)
        result = run_fn(["osascript", "-e", script], capture_output=True, text=True)
        if result.returncode == 0:
            try:
                attempt_closed_count = int((result.stdout or "0").strip())
            except ValueError:
                attempt_closed_count = 0
            closed_count += attempt_closed_count
            if attempt_closed_count == 0:
                break
    if result.returncode != 0 or closed_count > 0 or allow_terminate_with_nonproof_windows:
        state_script = "\n".join(
            [
                'tell application "System Events"',
                '    if not (exists process "Terminal") then',
                '        return "0\t0\t0"',
                "    end if",
                '    set terminalPid to unix id of process "Terminal"',
                "end tell",
                'tell application "Terminal"',
                "    set proofCount to 0",
                "    set otherProofCount to 0",
                "    set otherCount to 0",
                "    repeat with w in (every window)",
                "        set windowName to name of w",
                f"        if windowName contains {json.dumps(title_contains)} then",
                "            set proofCount to proofCount + 1",
                '        else if windowName contains "Pulp Video Proof" then',
                "            set otherProofCount to otherProofCount + 1",
                "        else",
                "            set otherCount to otherCount + 1",
                "        end if",
                "    end repeat",
                '    return (terminalPid as text) & "\t" & (proofCount as text) & "\t" & (otherCount as text)',
                "end tell",
            ]
        )
        state_result = run_fn(["osascript", "-e", state_script], capture_output=True, text=True)
        if state_result.returncode == 0:
            state_fields = (state_result.stdout or "").strip().split("\t")
            if len(state_fields) == 3:
                try:
                    terminal_pid = int(state_fields[0])
                    proof_count = int(state_fields[1])
                    other_count = int(state_fields[2])
                except ValueError:
                    terminal_pid = 0
                    proof_count = 0
                    other_count = 0
                if terminal_pid > 0 and (
                    (proof_count > 0 and other_count == 0) or allow_terminate_with_nonproof_windows
                ):
                    terminate_result = run_fn(["kill", "-TERM", str(terminal_pid)], capture_output=True, text=True)
                    terminate_returncode = terminate_result.returncode
                    terminated_terminal = terminate_result.returncode == 0
                remaining_proof_count = proof_count
                other_window_count = other_count
    return {
        "title_contains": title_contains,
        "closed_count": closed_count,
        "remaining_proof_count": remaining_proof_count,
        "other_window_count": other_window_count,
        "terminated_terminal": terminated_terminal,
        "terminate_returncode": terminate_returncode,
        "returncode": result.returncode,
        "stdout": (result.stdout or "").strip(),
        "stderr": (result.stderr or "").strip(),
    }


def terminal_app_running(
    *,
    run_fn: Callable[..., subprocess.CompletedProcess] = subprocess.run,
) -> bool:
    script = "\n".join(
        [
            'tell application "System Events"',
            '    return exists process "Terminal"',
            "end tell",
        ]
    )
    result = run_fn(["osascript", "-e", script], capture_output=True, text=True)
    return result.returncode == 0 and (result.stdout or "").strip().lower() == "true"


def run_local_ci_in_terminal(
    argv: Sequence[str],
    *,
    cwd: Path,
    python_executable: str,
    script_path: Path,
    timeout_secs: float = 1800.0,
    run_fn: Callable[..., subprocess.CompletedProcess] = subprocess.run,
    monotonic_fn: Callable[[], float] = time.monotonic,
    sleep_fn: Callable[[float], None] = time.sleep,
) -> dict:
    with tempfile.TemporaryDirectory(prefix="pulp-local-ci-terminal-") as tmp:
        tmp_path = Path(tmp)
        stdout_path = tmp_path / "stdout.txt"
        stderr_path = tmp_path / "stderr.txt"
        returncode_path = tmp_path / "returncode.txt"
        terminal_title = f"Pulp Video Proof local-ci {uuid.uuid4().hex[:8]}"
        terminal_was_running = terminal_app_running(run_fn=run_fn)
        shell_script = terminal_shell_script(
            cwd=cwd,
            python_executable=python_executable,
            script_path=script_path,
            argv=argv,
            stdout_path=stdout_path,
            stderr_path=stderr_path,
            returncode_path=returncode_path,
            title=terminal_title,
        )
        result = run_fn(
            ["osascript", "-e", f'tell application "Terminal" to do script {json.dumps(shell_script)}'],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            return {
                "returncode": result.returncode,
                "stdout": "",
                "stderr": (result.stderr or result.stdout or "osascript failed").strip() + "\n",
                "timed_out": False,
            }

        deadline = monotonic_fn() + timeout_secs
        while monotonic_fn() < deadline:
            if returncode_path.exists():
                stdout = stdout_path.read_text() if stdout_path.exists() else ""
                stderr = stderr_path.read_text() if stderr_path.exists() else ""
                try:
                    returncode = int(returncode_path.read_text().strip())
                except ValueError:
                    returncode = 1
                    stderr = stderr + f"Invalid Terminal return code file: {returncode_path}\n"
                return {
                    "returncode": returncode,
                    "stdout": stdout,
                    "stderr": stderr,
                    "timed_out": False,
                    "terminal_title": terminal_title,
                    "terminal_cleanup": close_terminal_windows_with_title(
                        terminal_title,
                        run_fn=run_fn,
                        sleep_fn=sleep_fn,
                        allow_terminate_with_nonproof_windows=not terminal_was_running,
                    ),
                }
            sleep_fn(0.5)

        return {
            "returncode": 124,
            "stdout": stdout_path.read_text() if stdout_path.exists() else "",
            "stderr": f"Timed out waiting for Terminal-launched local-ci command after {timeout_secs:g}s.\n",
            "timed_out": True,
            "terminal_title": terminal_title,
        }
