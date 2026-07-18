#!/usr/bin/env python3
"""Tests for tools/ci/tart-runner.sh.

The Tart supervisor mints a Just-In-Time runner config, clones the golden
VM, runs one CI job, and discards the VM. Historically it named each VM
`ephr-$$-$i` (launcher PID + per-job counter), so the same physical Mac
registered under a fresh throwaway name on every launchd restart. The
supervisor now derives a STATIC, machine-recognizable name per (host,
slot) — `pulp-<class>-<NN>` — matching the bare-metal lane's
`pulp-studio-01` convention, and reclaims that name (deletes a stale
GitHub registration / leftover clone) before each reuse, the JIT-lane
equivalent of bare-metal `config.sh --replace`.

These tests pin the name-derivation contract via the `--print-name` hook,
which is pure (no gh/tart needed), so they run on every platform in CI.

Run:  python3 tools/ci/test_tart_runner.py
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("tart-runner.sh")
LINUX_SCRIPT = Path(__file__).with_name("tart-runner-linux.sh")
WINDOWS_SCRIPT = Path(__file__).with_name("qemu-runner-windows.sh")
SUPERVISORS = (SCRIPT, LINUX_SCRIPT, WINDOWS_SCRIPT)
M5_LABELS = "self-hosted,macos,arm64,pulp-build,pulp-build-m5"
STUDIO_LABELS = "self-hosted,macos,arm64,pulp-build,pulp-build-studio"
PLAIN_LABELS = "self-hosted,macos,arm64,pulp-build"
PILOT_LABELS = "self-hosted,macos,arm64,pulp-build-vm"


def _run(*args: str, env: dict | None = None) -> subprocess.CompletedProcess:
    full_env = {**os.environ, **(env or {})}
    return subprocess.run(
        ["bash", str(SCRIPT), *args],
        capture_output=True, text=True, check=False, env=full_env,
    )


def _name(*args: str, env: dict | None = None) -> str:
    r = _run("--print-name", *args, env=env)
    assert r.returncode == 0, r.stderr
    return r.stdout.strip()


class ScriptContractTests(unittest.TestCase):
    def test_script_exists_and_executable(self) -> None:
        self.assertTrue(SCRIPT.is_file(), SCRIPT)
        self.assertTrue(os.access(SCRIPT, os.X_OK), f"{SCRIPT} not executable")

    def test_syntax_is_valid(self) -> None:
        r = subprocess.run(["bash", "-n", str(SCRIPT)],
                           capture_output=True, text=True, check=False)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_unknown_arg_fails(self) -> None:
        r = _run("--definitely-not-a-flag")
        self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_workflow_name_option_is_parseable(self) -> None:
        # Coverage uses the same supervisor binary but watches the Coverage
        # workflow queue instead of the Build and Test queue.
        self.assertEqual(
            _name(
                "--workflow-name",
                "Coverage",
                "--name-prefix",
                "pulp-coverage-macos",
            ),
            "pulp-coverage-macos-01",
        )


class NameDerivationTests(unittest.TestCase):
    """The `--print-name` contract — derivation must be pure + deterministic."""

    def test_class_label_drives_name(self) -> None:
        self.assertEqual(_name("--labels", M5_LABELS), "pulp-m5-01")
        self.assertEqual(_name("--labels", STUDIO_LABELS), "pulp-studio-01")

    def test_slot_zero_pads_and_distinguishes_supervisors(self) -> None:
        # Two supervisors on one host (the 2-VM cap) must not collide.
        self.assertEqual(_name("--labels", M5_LABELS, "--slot", "2"), "pulp-m5-02")
        self.assertEqual(
            _name("--labels", M5_LABELS, env={"PULP_RUNNER_SLOT": "2"}),
            "pulp-m5-02",
        )
        a = _name("--labels", M5_LABELS, "--slot", "1")
        b = _name("--labels", M5_LABELS, "--slot", "2")
        self.assertNotEqual(a, b)

    def test_explicit_name_override_wins(self) -> None:
        self.assertEqual(
            _name("--labels", M5_LABELS, "--name", "my-fixed-runner"),
            "my-fixed-runner",
        )
        self.assertEqual(
            _name("--labels", M5_LABELS, env={"PULP_RUNNER_NAME": "env-fixed"}),
            "env-fixed",
        )

    def test_prefix_override(self) -> None:
        self.assertEqual(
            _name("--name-prefix", "pulp-studio", "--slot", "3"),
            "pulp-studio-03",
        )

    def test_no_class_label_falls_back_to_hostname(self) -> None:
        # No `pulp-build-<class>` label → "pulp-<short-hostname>-NN".
        name = _name("--labels", PLAIN_LABELS)
        self.assertTrue(name.startswith("pulp-"), name)
        self.assertTrue(name.endswith("-01"), name)

    def test_generic_pilot_label_falls_back_to_hostname(self) -> None:
        # `pulp-build-vm` is a routing label shared by every pilot host, not a
        # host class. It must not derive the cross-host-colliding `pulp-vm-01`.
        name = _name("--labels", PILOT_LABELS)
        self.assertTrue(name.startswith("pulp-"), name)
        self.assertTrue(name.endswith("-01"), name)
        self.assertNotEqual(name, "pulp-vm-01")

    def test_name_is_stable_across_invocations(self) -> None:
        # The whole point: same machine + slot → same name, every time.
        first = _name("--labels", M5_LABELS)
        second = _name("--labels", M5_LABELS)
        self.assertEqual(first, second)


class ReuseSafetyTests(unittest.TestCase):
    """A static name is reusable only if the supervisor reclaims it first."""

    def setUp(self) -> None:
        self.body = SCRIPT.read_text(encoding="utf-8")

    def test_no_longer_uses_pid_counter_vm_name(self) -> None:
        # The old churn pattern `ephr-$$-$1` must be gone as the VM name.
        self.assertNotIn('vm="ephr-$$', self.body)

    def test_jit_config_minted_with_static_name(self) -> None:
        self.assertIn("generate-jitconfig", self.body)
        self.assertIn('name=$vm', self.body)
        self.assertIn('vm="$RUNNER_NAME"', self.body)

    def test_reclaims_stale_registration_and_clone(self) -> None:
        self.assertIn("reclaim_runner_name", self.body)
        # Deletes a lingering GitHub registration of the same name...
        self.assertIn("-X DELETE", self.body)
        self.assertIn("actions/runners", self.body)
        # ...and any crashed leftover Tart clone.
        self.assertIn('tart delete "$name"', self.body)

    def test_loop_queue_gate_uses_configured_workflow_name(self) -> None:
        self.assertIn('WORKFLOW_NAME="${PULP_RUNNER_WORKFLOW_NAME:-Build and Test}"', self.body)
        self.assertIn("--workflow-name)", self.body)
        self.assertIn('select(.name == \\"${WORKFLOW_NAME}\\")', self.body)

    def test_queue_gate_can_require_matching_labels(self) -> None:
        self.assertIn('MATCH_LABELS="${PULP_RUNNER_QUEUE_MATCH_LABELS:-0}"', self.body)
        self.assertIn("--queue-match-labels)", self.body)
        # A VM that registers with `want` can serve a queued job iff the job's
        # requested labels are a SUBSET of `want` (GitHub's matching rule —
        # the runner may advertise extra labels). The reverse, want ⊆ labels,
        # silently fails to boot whenever the runner carries an extra label
        # (e.g. a per-host pulp-build-vm-secondary), so guard against it.
        self.assertIn("labels.issubset(want)", self.body)
        self.assertNotIn("want.issubset(labels)", self.body)

    def test_label_matched_queue_scan_covers_in_progress_runs(self) -> None:
        # A workflow whose early GitHub-hosted resolver/classify job runs first
        # (Coverage, Release CLI) flips the run to `in_progress` before its
        # self-hosted leg is queued. A queued-only run scan would never see that
        # job and the VM would never boot. The label-matched gate must scan BOTH
        # statuses (mirrors the tartci macOS provider's loop).
        self.assertIn("for st in queued in_progress;", self.body)
        self.assertIn("runs?status=$st", self.body)


class LabelMatchSemanticsTests(unittest.TestCase):
    """Behavioral test of the --queue-match-labels matcher.

    The label-matched queue gate embeds an inline Python snippet that decides,
    for each queued job, whether THIS VM (which registers with the runner's
    advertised labels) is allowed to serve it. We extract that exact snippet
    from the script and run it so the assertion can't drift from the source,
    and so an inverted subset check (the bug fixed alongside this test) is
    caught by behavior, not just by grepping for a string.
    """

    def setUp(self) -> None:
        body = SCRIPT.read_text(encoding="utf-8")
        m = re.search(r"(import json, os, sys\nwant = \{.*?print\(n\)\n)",
                      body, re.S)
        self.assertIsNotNone(m, "label-match snippet not found in tart-runner.sh")
        self.snippet = m.group(1)

    def _count(self, runner_labels: list[str], jobs: list[list[str]]) -> int:
        env = {**os.environ, "LABEL_JSON": json.dumps(runner_labels)}
        stdin = "".join(json.dumps(j) + "\n" for j in jobs)
        r = subprocess.run(["python3", "-c", self.snippet], input=stdin,
                           capture_output=True, text=True, check=True, env=env)
        return int(r.stdout.strip())

    def test_extra_runner_label_still_matches_pool_label_job(self) -> None:
        # The regression: a host that advertises the shared pool label PLUS a
        # per-host label must still wake for a job requesting only the pool set.
        runner = ["self-hosted", "macOS", "ARM64", "pulp-build",
                  "pulp-build-vm", "pulp-build-vm-secondary"]
        job = ["self-hosted", "macOS", "ARM64", "pulp-build", "pulp-build-vm"]
        self.assertEqual(self._count(runner, [job]), 1)

    def test_exact_match_counts(self) -> None:
        labels = ["self-hosted", "macOS", "ARM64", "pulp-coverage-vm-macos"]
        self.assertEqual(self._count(labels, [labels]), 1)

    def test_job_needing_unavailable_label_does_not_match(self) -> None:
        runner = ["self-hosted", "macOS", "ARM64", "pulp-coverage-vm-macos"]
        job = ["self-hosted", "macOS", "ARM64", "pulp-build", "pulp-build-vm"]
        self.assertEqual(self._count(runner, [job]), 0)

    def test_match_is_case_insensitive(self) -> None:
        runner = ["self-hosted", "macos", "arm64", "pulp-coverage-vm-macos"]
        job = ["self-hosted", "macOS", "ARM64", "pulp-coverage-vm-macos"]
        self.assertEqual(self._count(runner, [job]), 1)

    def test_malformed_lines_are_skipped_not_counted(self) -> None:
        runner = ["self-hosted", "macOS", "ARM64", "pulp-coverage-vm-macos"]
        good = runner
        env = {**os.environ, "LABEL_JSON": json.dumps(runner)}
        stdin = "not json\n" + json.dumps(good) + "\n{ bad\n"
        r = subprocess.run(["python3", "-c", self.snippet], input=stdin,
                           capture_output=True, text=True, check=True, env=env)
        self.assertEqual(int(r.stdout.strip()), 1)


GH_STUB = r"""#!/usr/bin/env bash
# Stub `gh`: one job always queued, no stale runner registration, and a JIT mint
# that always fails — standing in for the transient API errors (token hiccup,
# rate limit, 5xx) the supervisor must survive rather than exit on.
case "$*" in
  *generate-jitconfig*) echo "gh: simulated transient JIT mint failure" >&2; exit 1;;
  *actions/runners*)    exit 0;;   # reclaim_runner_name: nothing stale to clear
  *actions/runs*)       echo 1;;   # queue gate: one job waiting
esac
exit 0
"""

TART_STUB = r"""#!/usr/bin/env bash
case "${1:-}" in
  list) echo '[]';;   # running_macos_vms → 0, i.e. a free VM slot
esac
exit 0
"""


class SupervisorSurvivesTransientFailureTests(unittest.TestCase):
    """The supervisor loop must RETRY transient failures, never exit on them.

    `die` exits the script. Called from inside run_one, that killed the whole
    supervisor on the first hiccup — a caller's `|| true` cannot catch an
    `exit` — leaving recovery to launchd KeepAlive, whose respawn throttle
    compounds a one-off blip into a long outage. Per-iteration failures now go
    through `soft_fail` (return 1) so the loop counts, backs off, and continues.

    This drives the REAL loop against stub `gh`/`tart` binaries on PATH, so it
    fails on the exit-on-transient behavior rather than on a grep.
    """

    def _run_loop(self, script: Path, marker: str, timeout: float = 30.0):
        """Run `script --loop` against the stubs; return (output, exited_early)."""
        with tempfile.TemporaryDirectory() as td:
            for name, body in (("gh", GH_STUB), ("tart", TART_STUB),
                               ("qemu-system-aarch64", TART_STUB)):
                p = Path(td, name)
                p.write_text(body, encoding="utf-8")
                p.chmod(0o755)
            env = {
                **os.environ,
                "PATH": f"{td}:{os.environ['PATH']}",
                "PULP_VM_POLL": "1",         # keep the test fast...
                "PULP_VM_BACKOFF_MAX": "1",  # ...and pin the backoff to 1s
                # Windows supervisor preconditions (fatal, checked before the
                # loop) — satisfy them so the loop itself is what's under test.
                "TARTCI_WIN_GOLDEN": str(Path(td, "golden.qcow2")),
            }
            Path(td, "golden.qcow2").write_text("stub", encoding="utf-8")
            proc = subprocess.Popen(
                ["bash", str(script), "--loop"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, env=env,
            )
            killer = threading.Timer(timeout, proc.kill)
            killer.start()
            lines: list[str] = []
            exited_early = False
            try:
                while True:
                    line = proc.stdout.readline()
                    if not line:
                        # EOF: the supervisor process is GONE. With the bug, this
                        # is exactly what happens after the first failed mint.
                        exited_early = True
                        break
                    lines.append(line)
                    if marker in line:
                        break
            finally:
                killer.cancel()
                proc.kill()
                proc.wait(timeout=5)
                proc.stdout.close()
            return "".join(lines), exited_early

    def test_loop_reaches_second_iteration_after_a_failed_mint(self) -> None:
        out, exited_early = self._run_loop(SCRIPT, "[2]")
        self.assertIn("JIT config mint failed", out,
                      f"stub gh never failed the mint:\n{out}")
        self.assertFalse(exited_early,
                         f"supervisor EXITED on a transient failure:\n{out}")
        self.assertIn("[2]", out,
                      f"loop never reached iteration 2:\n{out}")

    def test_loop_keeps_going_across_repeated_failures(self) -> None:
        # Not just one retry — a supervisor must survive a sustained outage.
        out, exited_early = self._run_loop(SCRIPT, "[3]")
        self.assertFalse(exited_early, f"supervisor EXITED:\n{out}")
        self.assertIn("[3]", out, f"loop never reached iteration 3:\n{out}")

    def test_failed_iteration_is_counted_and_backed_off(self) -> None:
        out, _ = self._run_loop(SCRIPT, "consecutive=2")
        self.assertIn("consecutive=1", out, out)
        self.assertIn("consecutive=2", out, out)
        self.assertIn("backing off", out, out)

    def test_linux_supervisor_survives_transient_failure(self) -> None:
        out, exited_early = self._run_loop(LINUX_SCRIPT, "[2]")
        self.assertFalse(exited_early, f"Linux supervisor EXITED:\n{out}")
        self.assertIn("[2]", out, out)

    def test_windows_supervisor_survives_transient_failure(self) -> None:
        out, exited_early = self._run_loop(WINDOWS_SCRIPT, "[2]")
        self.assertFalse(exited_early, f"Windows supervisor EXITED:\n{out}")
        self.assertIn("[2]", out, out)


class RunOneNeverExitsTests(unittest.TestCase):
    """Structural guard: no `die` may be reachable from a per-iteration path.

    The behavioral tests above cover the failure modes that exist today; this
    keeps a NEW `die` from being added to run_one tomorrow, in any of the three
    supervisors, where it would silently reintroduce exit-on-transient.
    """

    @staticmethod
    def _run_one_body(script: Path) -> str:
        body = script.read_text(encoding="utf-8")
        m = re.search(r"\nrun_one\(\)\{.*?\n\}\n", body, re.S)
        assert m, f"run_one not found in {script.name}"
        return m.group(0)

    def test_run_one_does_not_die(self) -> None:
        for script in SUPERVISORS:
            with self.subTest(script=script.name):
                self.assertNotRegex(
                    self._run_one_body(script), r"\bdie\b",
                    f"{script.name}: run_one calls `die` — `exit` inside a "
                    "function kills the supervisor and `|| true` cannot catch it",
                )

    def test_supervisors_define_soft_fail(self) -> None:
        for script in SUPERVISORS:
            with self.subTest(script=script.name):
                self.assertIn("soft_fail()",
                              script.read_text(encoding="utf-8"))

    def test_fatal_preconditions_still_die(self) -> None:
        # `die` is still correct for startup preconditions — don't over-correct
        # into a supervisor that loops forever against a missing `tart`.
        body = SCRIPT.read_text(encoding="utf-8")
        self.assertIn('command -v tart >/dev/null 2>&1 || die', body)
        self.assertIn('*) die "unknown arg: $1";;', body)


class BackoffTests(unittest.TestCase):
    """Behavioral test of the extracted `backoff_secs` helper."""

    def _backoff(self, script: Path, fails: int, poll: int, cap: int) -> int:
        body = script.read_text(encoding="utf-8")
        m = re.search(r"(backoff_secs\(\)\{.*?\n\}\n)", body, re.S)
        self.assertIsNotNone(m, f"backoff_secs not found in {script.name}")
        prog = f'POLL={poll}; BACKOFF_MAX={cap}\n{m.group(1)}\nbackoff_secs {fails}'
        r = subprocess.run(["bash", "-c", prog], capture_output=True,
                           text=True, check=True)
        return int(r.stdout.strip())

    def test_backoff_doubles_then_caps(self) -> None:
        for script in SUPERVISORS:
            with self.subTest(script=script.name):
                self.assertEqual(self._backoff(script, 1, 20, 300), 20)
                self.assertEqual(self._backoff(script, 2, 20, 300), 40)
                self.assertEqual(self._backoff(script, 3, 20, 300), 80)
                # Capped — never an unbounded wait.
                self.assertEqual(self._backoff(script, 9, 20, 300), 300)
                self.assertEqual(self._backoff(script, 99, 20, 300), 300)


if __name__ == "__main__":
    unittest.main(verbosity=2)
