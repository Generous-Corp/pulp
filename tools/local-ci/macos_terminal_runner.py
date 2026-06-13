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
) -> str:
    command = [
        "env",
        f"{TERMINAL_REENTRY_ENV}=1",
        python_executable,
        str(script_path),
        *strip_run_in_terminal_args(argv),
    ]
    return (
        f"cd {shlex.quote(str(cwd))}; "
        f"(/usr/bin/caffeinate -u -t 60 >/dev/null 2>&1 &); "
        f"{' '.join(shlex.quote(part) for part in command)} "
        f"> {shlex.quote(str(stdout_path))} "
        f"2> {shlex.quote(str(stderr_path))}; "
        f"printf '%s\\n' \"$?\" > {shlex.quote(str(returncode_path))}"
    )


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
        shell_script = terminal_shell_script(
            cwd=cwd,
            python_executable=python_executable,
            script_path=script_path,
            argv=argv,
            stdout_path=stdout_path,
            stderr_path=stderr_path,
            returncode_path=returncode_path,
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
                }
            sleep_fn(0.5)

        return {
            "returncode": 124,
            "stdout": stdout_path.read_text() if stdout_path.exists() else "",
            "stderr": f"Timed out waiting for Terminal-launched local-ci command after {timeout_secs:g}s.\n",
            "timed_out": True,
        }
