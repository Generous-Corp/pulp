#!/usr/bin/env python3
"""Focused process-lifecycle tests for captured pre-push gates."""

from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / ".githooks" / "lib" / "gate-output.sh"
SUPERVISOR = ROOT / ".githooks" / "lib" / "gate-supervisor.py"


def load_supervisor_module():
    spec = importlib.util.spec_from_file_location("prepush_gate_supervisor", SUPERVISOR)
    if spec is None or spec.loader is None:
        raise AssertionError(f"could not load {SUPERVISOR}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def process_is_live(pid: int) -> bool:
    result = subprocess.run(
        ["ps", "-o", "stat=", "-p", str(pid)],
        capture_output=True,
        text=True,
        check=False,
    )
    return result.returncode == 0 and not result.stdout.lstrip().startswith("Z")


def wait_for_file(path: Path, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() and path.stat().st_size:
            return
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {path}")


def wait_until_dead(*pids: int, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if all(not process_is_live(pid) for pid in pids):
            return
        time.sleep(0.05)
    live = [pid for pid in pids if process_is_live(pid)]
    raise AssertionError(f"processes survived supervised teardown: {live}")


def children_of(pid: int) -> list[int]:
    result = subprocess.run(
        ["pgrep", "-P", str(pid)],
        capture_output=True,
        text=True,
        check=False,
    )
    return [int(value) for value in result.stdout.split()]


def force_cleanup(gate_group: int | None, *pids: int | None) -> None:
    """Keep a deliberately broken mutation from orphaning the test fixture."""
    if gate_group is not None:
        try:
            os.killpg(gate_group, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
    for pid in pids:
        if pid is None:
            continue
        try:
            os.kill(pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass


GATE_PROGRAM = r"""
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

pid_file = Path(sys.argv[1])
signal_file = Path(sys.argv[2]) if sys.argv[2] != "-" else None
child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])

def forwarded(signum, _frame):
    if signal_file is not None:
        signal_file.write_text(str(signum), encoding="utf-8")
    raise SystemExit(0)

for signum in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
    signal.signal(signum, forwarded)
pid_file.write_text(f"{os.getpid()} {child.pid}", encoding="utf-8")
if len(sys.argv) > 3:
    raise SystemExit(int(sys.argv[3]))
time.sleep(60)
"""


def hook_command(temp: Path, pid_file: Path, signal_file: Path | None = None) -> list[str]:
    hook = temp / "hook.sh"
    hook.write_text(
        """#!/usr/bin/env bash
set -u
PREPUSH_GATE_LOG_DIR="$1/logs"
mkdir -p "$PREPUSH_GATE_LOG_DIR"
source "$2"
run_gate_captured "$3" -c "$4" "$5" "$6"
""",
        encoding="utf-8",
    )
    return [
        "bash",
        str(hook),
        str(temp),
        str(HELPER),
        sys.executable,
        GATE_PROGRAM,
        str(pid_file),
        str(signal_file) if signal_file is not None else "-",
    ]


class GateSupervisorTests(unittest.TestCase):
    def test_exact_success_failure_and_output_are_preserved(self) -> None:
        with tempfile.TemporaryDirectory(prefix="prepush-supervisor-status-") as raw:
            temp = Path(raw)
            for expected in (0, 7):
                log = temp / f"gate-{expected}.log"
                result = subprocess.run(
                    [
                        sys.executable,
                        str(SUPERVISOR),
                        "--log",
                        str(log),
                        "--",
                        sys.executable,
                        "-c",
                        f"print('gate diagnostic'); raise SystemExit({expected})",
                    ],
                    capture_output=True,
                    text=True,
                    check=False,
                    timeout=10,
                )
                self.assertEqual(result.returncode, expected, result.stderr)
                self.assertEqual(log.read_text(encoding="utf-8"), "gate diagnostic\n")

    def test_signal_terminated_gate_preserves_shell_status(self) -> None:
        with tempfile.TemporaryDirectory(prefix="prepush-supervisor-signaled-") as raw:
            temp = Path(raw)
            result = subprocess.run(
                [
                    sys.executable,
                    str(SUPERVISOR),
                    "--log",
                    str(temp / "gate.log"),
                    "--",
                    sys.executable,
                    "-c",
                    "import os,signal; os.kill(os.getpid(), signal.SIGTERM)",
                ],
                check=False,
                timeout=10,
            )
            self.assertEqual(result.returncode, 128 + signal.SIGTERM)

    def test_configured_python_interpreter_launches_supervisor(self) -> None:
        with tempfile.TemporaryDirectory(prefix="prepush-supervisor-python-") as raw:
            temp = Path(raw)
            marker = temp / "python-used"
            wrapper = temp / "selected-python"
            wrapper.write_text(
                "#!/bin/sh\nprintf selected >\"$PYTHON_MARKER\"\nexec \"$REAL_PYTHON\" \"$@\"\n",
                encoding="utf-8",
            )
            wrapper.chmod(0o755)
            env = os.environ.copy()
            env.update(
                {
                    "PYTHON": str(wrapper),
                    "PYTHON_MARKER": str(marker),
                    "REAL_PYTHON": sys.executable,
                }
            )
            hook = temp / "selected-python-hook.sh"
            hook.write_text(
                """#!/usr/bin/env bash
set -u
PREPUSH_GATE_LOG_DIR="$1/logs"
mkdir -p "$PREPUSH_GATE_LOG_DIR"
source "$2"
run_gate_captured "$3" -c 'raise SystemExit(0)'
""",
                encoding="utf-8",
            )
            result = subprocess.run(
                ["bash", str(hook), str(temp), str(HELPER), sys.executable],
                env=env,
                check=False,
                timeout=10,
            )
            self.assertEqual(result.returncode, 0)
            self.assertEqual(marker.read_text(encoding="utf-8"), "selected")

    def test_ownership_probe_error_reaps_live_gate_group(self) -> None:
        with tempfile.TemporaryDirectory(prefix="prepush-supervisor-probe-") as raw:
            temp = Path(raw)
            pid_file = temp / "pids"
            module = load_supervisor_module()
            actual_parent_pid = module.parent_pid(os.getppid())
            self.assertIsNotNone(actual_parent_pid)
            original_parent_pid = module.parent_pid
            calls = 0

            def failing_parent_pid(pid: int) -> int | None:
                nonlocal calls
                calls += 1
                if calls <= 2:
                    return actual_parent_pid
                raise OSError("simulated ps spawn failure")

            module.parent_pid = failing_parent_pid
            gate_pid = descendant_pid = None
            try:
                result = module.main(
                    [
                        str(SUPERVISOR),
                        "--log",
                        str(temp / "gate.log"),
                        "--",
                        sys.executable,
                        "-c",
                        GATE_PROGRAM,
                        str(pid_file),
                        "-",
                    ]
                )
                self.assertEqual(result, 2)
                wait_for_file(pid_file)
                gate_pid, descendant_pid = map(
                    int, pid_file.read_text(encoding="utf-8").split()
                )
                wait_until_dead(gate_pid, descendant_pid)
            finally:
                module.parent_pid = original_parent_pid
                force_cleanup(gate_pid, descendant_pid)

    def test_leader_exit_cannot_daemonize_a_descendant(self) -> None:
        with tempfile.TemporaryDirectory(prefix="prepush-supervisor-reap-") as raw:
            temp = Path(raw)
            pid_file = temp / "pids"
            gate_pid = descendant_pid = None
            try:
                result = subprocess.run(
                    [
                        sys.executable,
                        str(SUPERVISOR),
                        "--log",
                        str(temp / "gate.log"),
                        "--",
                        sys.executable,
                        "-c",
                        GATE_PROGRAM,
                        str(pid_file),
                        "-",
                        "7",
                    ],
                    check=False,
                    timeout=10,
                )
                self.assertEqual(result.returncode, 7)
                gate_pid, descendant_pid = map(
                    int, pid_file.read_text(encoding="utf-8").split()
                )
                wait_until_dead(gate_pid, descendant_pid)
            finally:
                if pid_file.exists():
                    gate_pid, descendant_pid = map(
                        int, pid_file.read_text(encoding="utf-8").split()
                    )
                force_cleanup(gate_pid, descendant_pid)

    def test_killing_hook_tears_down_gate_and_descendant(self) -> None:
        with tempfile.TemporaryDirectory(prefix="prepush-supervisor-hook-") as raw:
            temp = Path(raw)
            pid_file = temp / "pids"
            hook = subprocess.Popen(
                hook_command(temp, pid_file),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            gate_pid = descendant_pid = None
            try:
                wait_for_file(pid_file)
                gate_pid, descendant_pid = map(
                    int, pid_file.read_text(encoding="utf-8").split()
                )
                os.kill(hook.pid, signal.SIGTERM)
                hook.wait(timeout=5)
                wait_until_dead(gate_pid, descendant_pid)
            finally:
                if hook.poll() is None:
                    hook.kill()
                    hook.wait()
                force_cleanup(gate_pid, descendant_pid)

    def test_killing_only_git_caller_tears_down_surviving_hook_tree(self) -> None:
        with tempfile.TemporaryDirectory(prefix="prepush-supervisor-caller-") as raw:
            temp = Path(raw)
            pid_file = temp / "pids"
            hook_pid_file = temp / "hook-pid"
            command = hook_command(temp, pid_file)
            caller_code = (
                "import pathlib,subprocess,sys,time; "
                "hook=subprocess.Popen(sys.argv[2:], stdout=subprocess.DEVNULL, "
                "stderr=subprocess.DEVNULL); "
                "pathlib.Path(sys.argv[1]).write_text(str(hook.pid)); "
                "time.sleep(60)"
            )
            caller = subprocess.Popen(
                [sys.executable, "-c", caller_code, str(hook_pid_file), *command]
            )
            hook_pid = gate_pid = descendant_pid = None
            try:
                wait_for_file(hook_pid_file)
                wait_for_file(pid_file)
                hook_pid = int(hook_pid_file.read_text(encoding="utf-8"))
                gate_pid, descendant_pid = map(
                    int, pid_file.read_text(encoding="utf-8").split()
                )
                os.kill(caller.pid, signal.SIGTERM)
                caller.wait(timeout=5)
                wait_until_dead(hook_pid, gate_pid, descendant_pid)
            finally:
                if caller.poll() is None:
                    caller.kill()
                    caller.wait()
                force_cleanup(gate_pid, hook_pid, descendant_pid)

    def test_signal_is_forwarded_to_isolated_gate_group(self) -> None:
        with tempfile.TemporaryDirectory(prefix="prepush-supervisor-signal-") as raw:
            temp = Path(raw)
            pid_file = temp / "pids"
            signal_file = temp / "signal"
            hook = subprocess.Popen(
                hook_command(temp, pid_file, signal_file),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            gate_pid = descendant_pid = None
            try:
                wait_for_file(pid_file)
                deadline = time.monotonic() + 5
                supervisor_pids: list[int] = []
                while time.monotonic() < deadline and not supervisor_pids:
                    supervisor_pids = children_of(hook.pid)
                    time.sleep(0.05)
                self.assertEqual(len(supervisor_pids), 1)
                os.kill(supervisor_pids[0], signal.SIGINT)
                self.assertEqual(hook.wait(timeout=5), 128 + signal.SIGINT)
                wait_for_file(signal_file)
                self.assertEqual(signal_file.read_text(encoding="utf-8"), "2")
                gate_pid, descendant_pid = map(
                    int, pid_file.read_text(encoding="utf-8").split()
                )
                wait_until_dead(gate_pid, descendant_pid)
            finally:
                if hook.poll() is None:
                    hook.kill()
                    hook.wait()
                force_cleanup(gate_pid, descendant_pid)

    def test_load_bearing_supervision_primitives_remain_present(self) -> None:
        source = SUPERVISOR.read_text(encoding="utf-8")
        self.assertIn("start_new_session=True", source)
        self.assertIn("os.killpg(process_group, first_signal)", source)
        self.assertIn("os.getppid() != owner_pid", source)
        self.assertIn("parent_pid(owner_pid) != caller_pid", source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
