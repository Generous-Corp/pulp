"""Terminal-driven proof launcher for macOS desktop video proofs.

Runs the proof command inside Terminal.app so it inherits the user's Screen
Recording / Accessibility grants, then tears the window down cleanly (killing
the tty before close so no "terminate running process?" modal is left behind).
"""

from __future__ import annotations

from collections.abc import Callable
import json
from pathlib import Path
import shlex
import subprocess
import time


def terminal_proof_shell_script(
    *,
    cwd: Path,
    command_args: list[str],
    title: str,
    stdout_path: Path,
    stderr_path: Path,
    returncode_path: Path,
    keepalive_secs: float,
) -> str:
    command = " ".join(shlex.quote(part) for part in command_args)
    return "\n".join(
        [
            f"cd {shlex.quote(str(cwd))}",
            f"printf '\\033]0;%s\\007' {shlex.quote(title)}",
            f"exec > >(tee -a {shlex.quote(str(stdout_path))}) 2> >(tee -a {shlex.quote(str(stderr_path))} >&2)",
            "printf '%s\\n' 'Pulp validation video proof'",
            f"printf '%s\\n' {shlex.quote('$ ' + command)}",
            f"{command} &",
            "pulp_child_pid=$!",
            f"sleep {max(0.5, keepalive_secs):.3f}",
            "if kill -0 \"$pulp_child_pid\" >/dev/null 2>&1; then",
            "  printf '\\n%s\\n' '[pulp video proof] stopping command after capture window'",
            "  kill -INT \"$pulp_child_pid\" >/dev/null 2>&1 || true",
            "  for _pulp_wait in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do",
            "    kill -0 \"$pulp_child_pid\" >/dev/null 2>&1 || break",
            "    sleep 0.1",
            "  done",
            "  if kill -0 \"$pulp_child_pid\" >/dev/null 2>&1; then",
            "    kill -TERM \"$pulp_child_pid\" >/dev/null 2>&1 || true",
            "    sleep 0.5",
            "  fi",
            "  if kill -0 \"$pulp_child_pid\" >/dev/null 2>&1; then",
            "    kill -KILL \"$pulp_child_pid\" >/dev/null 2>&1 || true",
            "  fi",
            "fi",
            "wait \"$pulp_child_pid\"",
            "pulp_rc=$?",
            f"printf '%s\\n' \"$pulp_rc\" > {shlex.quote(str(returncode_path))}",
            "printf '\\n[pulp video proof] command exit: %s\\n' \"$pulp_rc\"",
        ]
    )


def launch_macos_terminal_proof_command(
    command_args: list[str],
    *,
    cwd: Path,
    title: str,
    stdout_path: Path,
    stderr_path: Path,
    returncode_path: Path,
    keepalive_secs: float,
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> dict:
    stdout_path.parent.mkdir(parents=True, exist_ok=True)
    stderr_path.parent.mkdir(parents=True, exist_ok=True)
    returncode_path.parent.mkdir(parents=True, exist_ok=True)
    shell_script = terminal_proof_shell_script(
        cwd=cwd,
        command_args=command_args,
        title=title,
        stdout_path=stdout_path,
        stderr_path=stderr_path,
        returncode_path=returncode_path,
        keepalive_secs=keepalive_secs,
    )
    result = run_fn(
        ["osascript", "-e", f'tell application "Terminal" to do script {json.dumps(shell_script)}'],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or f"osascript exited {result.returncode}"
        raise RuntimeError(f"Could not launch Terminal proof command: {detail}")
    return {
        "bundle_id": "com.apple.Terminal",
        "title": title,
        "command": command_args,
        "cwd": str(cwd),
        "stdout": str(stdout_path),
        "stderr": str(stderr_path),
        "returncode": str(returncode_path),
        "keepalive_secs": keepalive_secs,
        "osascript_stdout": result.stdout.strip(),
        "osascript_stderr": result.stderr.strip(),
    }


def close_macos_terminal_windows_with_title(
    title_contains: str,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
    sleep_fn: Callable[[float], None] = time.sleep,
    attempts: int = 5,
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
            "            close w",
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
    # Closing a Terminal window whose shell still has a running process pops a
    # blocking "Do you want to terminate the running process?" modal that the
    # AppleScript `close` cannot dismiss. The `do script` shell counts as such a
    # process, so we first kill each proof window's tty foreground processes;
    # the window then closes silently (it shows "[Process completed]").
    killed_ttys: list[str] = []
    tty_script = "\n".join(
        [
            'tell application "Terminal"',
            "    set ttyList to \"\"",
            "    repeat with w in (every window)",
            f"        if (name of w) contains {json.dumps(title_contains)} then",
            "            try",
            "                set ttyList to ttyList & (tty of (selected tab of w)) & linefeed",
            "            end try",
            "        end if",
            "    end repeat",
            "    return ttyList",
            "end tell",
        ]
    )
    tty_result = run_fn(["osascript", "-e", tty_script], capture_output=True, text=True)
    if tty_result.returncode == 0:
        for line in (tty_result.stdout or "").splitlines():
            tty = line.strip()
            if not tty:
                continue
            tty_name = tty[len("/dev/") :] if tty.startswith("/dev/") else tty
            kill_result = run_fn(["pkill", "-t", tty_name], capture_output=True, text=True)
            # pkill returns 1 when no process matched (already idle) — both are fine.
            if kill_result.returncode in (0, 1):
                killed_ttys.append(tty_name)
        if killed_ttys:
            sleep_fn(0.3)

    result = subprocess.CompletedProcess(["osascript", "-e", script], 1, "", "")
    closed_count = 0
    terminated_terminal = False
    terminate_returncode: int | None = None
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
    if result.returncode != 0:
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
                if terminal_pid > 0 and proof_count > 0 and other_count == 0:
                    terminate_result = run_fn(["kill", "-TERM", str(terminal_pid)], capture_output=True, text=True)
                    terminate_returncode = terminate_result.returncode
                    terminated_terminal = terminate_result.returncode == 0
    return {
        "title_contains": title_contains,
        "closed_count": closed_count,
        "killed_ttys": killed_ttys,
        "terminated_terminal": terminated_terminal,
        "terminate_returncode": terminate_returncode,
        "returncode": result.returncode,
        "stdout": result.stdout.strip(),
        "stderr": result.stderr.strip(),
    }


