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

import os
import subprocess
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("tart-runner.sh")
M5_LABELS = "self-hosted,macos,arm64,pulp-build,pulp-build-m5"
STUDIO_LABELS = "self-hosted,macos,arm64,pulp-build,pulp-build-studio"
PLAIN_LABELS = "self-hosted,macos,arm64,pulp-build"


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


if __name__ == "__main__":
    unittest.main(verbosity=2)
