#!/usr/bin/env python3
"""Self-test: on a gate host, gates.sh runs the build-free Python lanes and still refuses the heavy ones.

The blocks under test are cut out of gates.sh and run in bash against a fake
checkout whose source_selftests.py records its argv and exits with a chosen
status, with tartci faked to report the role under test. Nothing here depends
on the real host's role.
"""

from __future__ import annotations

import os
import re
import stat
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import process_liveness  # noqa: E402

GATES = (HERE / "gates.sh").read_text(encoding="utf-8")


def block(pattern: str) -> str:
    match = re.search(pattern, GATES, re.MULTILINE | re.DOTALL)
    assert match, pattern
    return match.group(0)


HEAD = block(r"^gate_host_builds_skipped=0\n.*?^fi$")
SRC_LANES = block(r'^if \[ "\$gate_host_python_lanes" -eq 1 \] && \[ -f "\$ROOT/tools/ci/source_selftests.py".*?^fi$')
SYNC_JOB = block(r'^if \[ -f "\$ROOT/tools/ci/sync_job_parity.json" \].*?^fi$')

FAKE_SELFTESTS = """#!/usr/bin/env python3
import os, sys
with open(os.environ["RECORD"], "a", encoding="utf-8") as record:
    record.write(" ".join(sys.argv[1:]) + "\\n")
lane = "sync-job" if "--sync-job" in sys.argv else next(
    (name for name, flag in (("pr-fast", "--drift-fast"), ("ctest-python", "--ctest-python"),
                             ("workflow-lint", "--workflow")) if flag in sys.argv), "manifest")
if lane in os.environ.get("FAIL_LANES", "").split(","):
    print("FAIL: deliberately failing selftest in lane " + lane)
    sys.exit(1)
if lane in os.environ.get("HANG_LANES", "").split(","):
    import time
    time.sleep(60)
print("source-selftests: 1 passed, 0 skipped, 0 failed, 0 pre-existing on base, out of 1 in 0.0s")
"""


def tartci(role: str, serves: str = "1") -> str:
    return f'#!/bin/sh\necho "TARTCI_SERVES_GATE={serves}"\necho "TARTCI_ROLE={role}"\n'


class GateHostPythonLanes(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name).resolve() / "repo"
        for rel, text in {
            "tools/ci/source_selftests.py": FAKE_SELFTESTS,
            "tools/ci/sync_job_parity.json": "{}\n",
            "tools/scripts/gate_host_policy.sh": (HERE / "gate_host_policy.sh").read_text(encoding="utf-8"),
            "tools/scripts/run_capped.py": (HERE / "run_capped.py").read_text(encoding="utf-8"),
            ".github/workflows/workflow-lint.yml": "name: x\n",
        }.items():
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        for cmd in (["git", "init", "-q", "-b", "main"], ["git", "add", "-A"],
                    ["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-q", "-m", "x"]):
            subprocess.run(cmd, cwd=self.root, check=True, capture_output=True)
        self.record = self.root.parent / "record.txt"

    def run_blocks(self, role: str, *, fail: str = "", hang: str = "", **env: str) -> tuple[subprocess.CompletedProcess, list[str]]:
        fake = self.root.parent / "tartci"
        fake.write_text(tartci(role), encoding="utf-8")
        fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
        script = textwrap.dedent(f"""
            set -u
            ROOT={self.root}; PYTHON=python3; BASE=HEAD
            source "$ROOT/tools/scripts/gate_host_policy.sh"
            fail=0; failed_gates=(); gate_fail() {{ fail=1; failed_gates+=("$1"); }}
            not_checked_log="$(mktemp)"
            {HEAD}
            {SRC_LANES}
            {SYNC_JOB}
            echo "RESULT fail=$fail failed=${{failed_gates[*]-}}"
            echo "NOTCHECKED<<"; cat "$not_checked_log"; echo ">>"
        """)
        proc = subprocess.run(["bash", "-c", script], capture_output=True, text=True, encoding="utf-8", check=False,
                              env={**os.environ, "PULP_TARTCI_BIN": str(fake), "RECORD": str(self.record),
                                   "FAIL_LANES": fail, "HANG_LANES": hang, **env})
        calls = self.record.read_text(encoding="utf-8").splitlines() if self.record.exists() else []
        return proc, calls

    def test_dev_overflow_runs_every_python_lane_at_low_priority_two_jobs(self) -> None:
        proc, calls = self.run_blocks("dev-overflow")
        self.assertIn("RESULT fail=0", proc.stdout, proc.stderr)
        self.assertEqual(len(calls), 5, calls)  # manifest, workflow-lint, pr-fast, ctest-python, sync-job
        for call in calls:
            self.assertIn("--jobs 2", call)
        self.assertIn("--sync-job", calls[-1])
        self.assertNotIn("--sync-job-checks-only", calls[-1], "the fixture tests must run, not just the checks")
        self.assertIn("[gate-host] REFUSED compiles, a kept build-gate configure and diff-coverage", proc.stderr)

    def test_dev_overflow_failing_selftest_fails_the_gate(self) -> None:
        for lane in ("manifest", "pr-fast", "sync-job"):
            with self.subTest(lane=lane):
                self.record.unlink(missing_ok=True)
                proc, _ = self.run_blocks("dev-overflow", fail=lane)
                self.assertIn("RESULT fail=1", proc.stdout, proc.stderr)
                self.assertIn("deliberately failing selftest", proc.stderr)

    def test_control_the_same_failure_passes_when_the_selftests_pass(self) -> None:
        proc, _ = self.run_blocks("dev-overflow", fail="no-such-lane")
        self.assertIn("RESULT fail=0", proc.stdout, proc.stderr)

    def test_the_lanes_really_run_at_nice_19(self) -> None:
        # The fake selftest runs as run_capped.py's child; its niceness is the proof.
        probe = self.root / "tools/ci/source_selftests.py"
        probe.write_text("import os,sys\nopen(os.environ['RECORD'],'a').write('nice=%d\\n' % os.nice(0))\n", encoding="utf-8")
        _, calls = self.run_blocks("dev-overflow")
        self.assertIn("nice=19", calls)

    def test_a_lane_that_hits_the_wall_time_cap_is_not_checked_not_failed(self) -> None:
        proc, _ = self.run_blocks("dev-overflow", hang="pr-fast", PULP_GATES_GATE_HOST_CAP_S="1")
        self.assertIn("RESULT fail=0", proc.stdout, proc.stderr)
        self.assertIn("[pr-fast] NOT CHECKED: exceeded the 1s gate-host wall-time cap", proc.stdout)

    def test_the_old_refusal_remains_available_and_is_listed(self) -> None:
        proc, calls = self.run_blocks("dev-overflow", PULP_GATES_GATE_HOST_PYTHON="0")
        self.assertEqual(len(calls), 1, calls)  # only the sync job's checks
        self.assertIn("--sync-job-checks-only", calls[0])
        self.assertIn("REFUSED compiles, a kept build-gate configure and diff-coverage and the source-selftest suites", proc.stderr)

    def test_control_a_light_host_runs_the_lanes_without_the_cap_or_the_job_limit(self) -> None:
        proc, calls = self.run_blocks("light")
        self.assertIn("RESULT fail=0", proc.stdout, proc.stderr)
        self.assertEqual(len(calls), 5, calls)
        self.assertTrue(all("--jobs 2" not in call for call in calls), calls)
        self.assertNotIn("[gate-host]", proc.stderr)

    def test_heavy_lanes_stay_behind_the_gate_host_flag(self) -> None:
        # Compiles, the kept build-gate configure and diff-coverage key off gate_host_builds_skipped,
        # which the Python-lane change must not have touched.
        self.assertIn("gate_host_builds_skipped=1", HEAD)
        self.assertIn("pulp_gate_host_configure_only_allowance", GATES)
        self.assertIn("Diff-coverage NOT run, and refused on this gate host", GATES)
        self.assertNotIn("local_diff_cover.sh", SRC_LANES + SYNC_JOB)


class RunCappedTests(unittest.TestCase):
    def run_capped(self, seconds: float, *cmd: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(HERE / "run_capped.py"), "--seconds", str(seconds), "--", *cmd],
                              capture_output=True, text=True, encoding="utf-8", check=False)

    def test_passes_the_command_status_through(self) -> None:
        self.assertEqual(self.run_capped(30, sys.executable, "-c", "raise SystemExit(7)").returncode, 7)
        self.assertEqual(self.run_capped(30, sys.executable, "-c", "pass").returncode, 0)

    def test_the_cap_kills_the_whole_group_and_returns_124(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            pidfile = Path(tmp) / "child.pid"
            code = ("import os,subprocess,sys,time\n"
                    f"c=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)'])\n"
                    f"open({str(pidfile)!r},'w').write(str(c.pid))\ntime.sleep(60)\n")
            proc = self.run_capped(1, sys.executable, "-c", code)
            self.assertEqual(proc.returncode, 124, proc.stderr)
            pid = int(pidfile.read_text(encoding="utf-8"))
            for _ in range(50):
                if not process_liveness.pid_alive(pid):
                    break
                time.sleep(0.1)
            self.assertFalse(process_liveness.pid_alive(pid), "the grandchild survived the cap")


if __name__ == "__main__":
    unittest.main()
