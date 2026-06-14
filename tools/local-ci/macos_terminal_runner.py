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
    close_window_script = "\n".join(
        [
            'tell application "Terminal"',
            "    set closedCount to 0",
            "    repeat with w in (every window)",
            "        set windowName to name of w",
            f"        if windowName contains {json.dumps(title_contains)} then",
            "            close w saving no",
            "            set closedCount to closedCount + 1",
            "        end if",
            "    end repeat",
            "    return closedCount",
            "end tell",
        ]
    )
    close_tab_script = "\n".join(
        [
            'tell application "Terminal"',
            "    set closedCount to 0",
            "    repeat with w in (every window)",
            "        set windowName to name of w",
            f"        if windowName contains {json.dumps(title_contains)} then",
            "            try",
            "                close selected tab of w saving no",
            "                set closedCount to closedCount + 1",
            "            end try",
            "        end if",
            "    end repeat",
            "    return closedCount",
            "end tell",
        ]
    )
    exit_tab_script = "\n".join(
        [
            'tell application "Terminal"',
            "    set exitCount to 0",
            "    repeat with w in (every window)",
            "        set windowName to name of w",
            f"        if windowName contains {json.dumps(title_contains)} then",
            "            try",
            '                do script "exit" in selected tab of w',
            "                set exitCount to exitCount + 1",
            "            end try",
            "        end if",
            "    end repeat",
            "    return exitCount",
            "end tell",
        ]
    )
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
    miniaturize_script = "\n".join(
        [
            'tell application "Terminal"',
            "    set miniaturizedCount to 0",
            "    repeat with w in (every window)",
            "        set windowName to name of w",
            f"        if windowName contains {json.dumps(title_contains)} then",
            "            set miniaturized of w to true",
            "            set miniaturizedCount to miniaturizedCount + 1",
            "        end if",
            "    end repeat",
            "    return miniaturizedCount",
            "end tell",
        ]
    )
    clear_stale_title_script = "\n".join(
        [
            'tell application "Terminal"',
            "    set clearedTitleCount to 0",
            "    repeat with w in (every window)",
            "        set windowName to name of w",
            f"        if windowName contains {json.dumps(title_contains)} then",
            "            repeat with t in tabs of w",
            "                try",
            "                    if busy of t is false then",
            '                        set custom title of t to ""',
            "                        set clearedTitleCount to clearedTitleCount + 1",
            "                    end if",
            "                end try",
            "            end repeat",
            "        end if",
            "    end repeat",
            "    return clearedTitleCount",
            "end tell",
        ]
    )
    result = subprocess.CompletedProcess(["osascript", "-e", close_window_script], 1, "", "")
    closed_count = 0
    close_attempt_count = 0
    terminated_terminal = False
    terminate_returncode: int | None = None
    remaining_proof_count: int | None = None
    other_window_count: int | None = None
    miniaturized_count = 0
    exit_attempt_count = 0
    stale_title_clear_count = 0
    for attempt in range(max(1, attempts)):
        if attempt:
            sleep_fn(0.2)
        result = run_fn(["osascript", "-e", close_window_script], capture_output=True, text=True)
        if result.returncode == 0:
            try:
                attempt_closed_count = int((result.stdout or "0").strip())
            except ValueError:
                attempt_closed_count = 0
            close_attempt_count += attempt_closed_count
        state_result = run_fn(["osascript", "-e", state_script], capture_output=True, text=True)
        if state_result.returncode == 0:
            state_fields = (state_result.stdout or "").strip().split("\t")
            if len(state_fields) == 3:
                try:
                    remaining_proof_count = int(state_fields[1])
                    other_window_count = int(state_fields[2])
                except ValueError:
                    remaining_proof_count = None
                    other_window_count = None
            if remaining_proof_count == 0:
                closed_count = close_attempt_count
                break
    if remaining_proof_count and remaining_proof_count > 0:
        exit_result = run_fn(["osascript", "-e", exit_tab_script], capture_output=True, text=True)
        if exit_result.returncode == 0:
            try:
                exit_attempt_count += int((exit_result.stdout or "0").strip())
            except ValueError:
                pass
            sleep_fn(0.2)
            retry_result = run_fn(["osascript", "-e", close_window_script], capture_output=True, text=True)
            if retry_result.returncode == 0:
                try:
                    close_attempt_count += int((retry_result.stdout or "0").strip())
                except ValueError:
                    pass
            sleep_fn(0.2)
            state_result = run_fn(["osascript", "-e", state_script], capture_output=True, text=True)
            if state_result.returncode == 0:
                state_fields = (state_result.stdout or "").strip().split("\t")
                if len(state_fields) == 3:
                    try:
                        remaining_proof_count = int(state_fields[1])
                        other_window_count = int(state_fields[2])
                    except ValueError:
                        remaining_proof_count = None
                        other_window_count = None
    if remaining_proof_count and remaining_proof_count > 0:
        tab_result = run_fn(["osascript", "-e", close_tab_script], capture_output=True, text=True)
        if tab_result.returncode == 0:
            try:
                close_attempt_count += int((tab_result.stdout or "0").strip())
            except ValueError:
                pass
        sleep_fn(0.2)
        state_result = run_fn(["osascript", "-e", state_script], capture_output=True, text=True)
        if state_result.returncode == 0:
            state_fields = (state_result.stdout or "").strip().split("\t")
            if len(state_fields) == 3:
                try:
                    remaining_proof_count = int(state_fields[1])
                    other_window_count = int(state_fields[2])
                except ValueError:
                    remaining_proof_count = None
                    other_window_count = None
    if remaining_proof_count == 0:
        closed_count = close_attempt_count
    else:
        closed_count = 0
    if remaining_proof_count and remaining_proof_count > 0:
        clear_title_result = run_fn(["osascript", "-e", clear_stale_title_script], capture_output=True, text=True)
        if clear_title_result.returncode == 0:
            try:
                stale_title_clear_count = int((clear_title_result.stdout or "0").strip())
            except ValueError:
                stale_title_clear_count = 0
            if stale_title_clear_count > 0:
                sleep_fn(0.2)
                state_result = run_fn(["osascript", "-e", state_script], capture_output=True, text=True)
                if state_result.returncode == 0:
                    state_fields = (state_result.stdout or "").strip().split("\t")
                    if len(state_fields) == 3:
                        try:
                            remaining_proof_count = int(state_fields[1])
                            other_window_count = int(state_fields[2])
                        except ValueError:
                            remaining_proof_count = None
                            other_window_count = None
                if remaining_proof_count == 0:
                    closed_count = close_attempt_count
    if remaining_proof_count and remaining_proof_count > 0:
        state_result = run_fn(["osascript", "-e", state_script], capture_output=True, text=True)
        if state_result.returncode == 0:
            state_fields = (state_result.stdout or "").strip().split("\t")
            if len(state_fields) == 3:
                try:
                    terminal_pid = int(state_fields[0])
                    remaining_proof_count = int(state_fields[1])
                    other_window_count = int(state_fields[2])
                except ValueError:
                    terminal_pid = 0
                if terminal_pid > 0 and (
                    (remaining_proof_count > 0 and other_window_count == 0) or allow_terminate_with_nonproof_windows
                ):
                    terminate_result = run_fn(["kill", "-TERM", str(terminal_pid)], capture_output=True, text=True)
                    terminate_returncode = terminate_result.returncode
                    terminated_terminal = terminate_result.returncode == 0
    if remaining_proof_count and remaining_proof_count > 0 and not terminated_terminal:
        miniaturize_result = run_fn(["osascript", "-e", miniaturize_script], capture_output=True, text=True)
        if miniaturize_result.returncode == 0:
            try:
                miniaturized_count = int((miniaturize_result.stdout or "0").strip())
            except ValueError:
                miniaturized_count = 0
    return {
        "title_contains": title_contains,
        "closed_count": closed_count,
        "close_attempt_count": close_attempt_count,
        "exit_attempt_count": exit_attempt_count,
        "stale_title_clear_count": stale_title_clear_count,
        "remaining_proof_count": remaining_proof_count,
        "other_window_count": other_window_count,
        "miniaturized_count": miniaturized_count,
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
        wrapper_path = tmp_path / "terminal-command.sh"
        wrapper_path.write_text(f"#!/bin/zsh\n{shell_script}\n")
        wrapper_path.chmod(0o700)
        terminal_command = f"/bin/zsh {shlex.quote(str(wrapper_path))}"
        result = run_fn(
            ["osascript", "-e", f'tell application "Terminal" to do script {json.dumps(terminal_command)}'],
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
