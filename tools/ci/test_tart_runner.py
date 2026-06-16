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
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("tart-runner.sh")
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
