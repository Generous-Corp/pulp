#!/usr/bin/env python3
"""Tests for install_shipyard_autoupdate.sh.

Only the paths that cannot mutate this machine are exercised: `--dry-run`
(prints the plist), `--status`, and argument handling. The install path
bootstraps a launchd agent, so it is deliberately never run here — a test
suite must not be able to load an agent onto a CI host.
"""

from __future__ import annotations

import plistlib
import subprocess
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "install_shipyard_autoupdate.sh"
REPO_ROOT = SCRIPT.parent.parent.parent


def run(*args: str, env: dict | None = None) -> subprocess.CompletedProcess:
    import os

    return subprocess.run(
        ["bash", str(SCRIPT), *args],
        capture_output=True,
        text=True,
        env={**os.environ, **(env or {})},
        timeout=120,
    )


class TestPlist(unittest.TestCase):
    def setUp(self) -> None:
        p = run("--dry-run")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.plist = plistlib.loads(p.stdout.encode())

    def test_is_valid_plist_with_expected_label(self) -> None:
        self.assertEqual(self.plist["Label"], "com.pulp.shipyard-autoupdate")

    def test_runs_the_stable_agent_copy_not_the_checkout(self) -> None:
        # A plist pointing into a worktree dies silently when that
        # worktree is removed after its PR lands.
        program = self.plist["ProgramArguments"][1]
        self.assertTrue(
            program.endswith("/.local/bin/shipyard_autoupdate.py"), program
        )
        self.assertNotIn(str(REPO_ROOT), program)

    def test_bakes_in_a_repo_for_the_pin(self) -> None:
        env = self.plist["EnvironmentVariables"]
        self.assertIn("PULP_SHIPYARD_AUTOUPDATE_REPO", env)

    def test_repo_is_overridable(self) -> None:
        p = run("--dry-run", env={"PULP_SHIPYARD_AUTOUPDATE_REPO": "/tmp/elsewhere"})
        pl = plistlib.loads(p.stdout.encode())
        self.assertEqual(
            pl["EnvironmentVariables"]["PULP_SHIPYARD_AUTOUPDATE_REPO"],
            "/tmp/elsewhere",
        )

    def test_path_reaches_local_bin_for_shipyard(self) -> None:
        # launchd hands an agent a minimal PATH; without this the agent
        # cannot find the very binary it exists to manage.
        self.assertIn("/.local/bin", self.plist["EnvironmentVariables"]["PATH"])

    def test_does_not_run_at_load(self) -> None:
        # Login/reboot is when runners start work; don't race them.
        self.assertNotIn("RunAtLoad", self.plist)

    def test_runs_on_an_interval_and_is_low_priority(self) -> None:
        self.assertEqual(self.plist["StartInterval"], 3600)
        self.assertEqual(self.plist["ProcessType"], "Background")
        self.assertTrue(self.plist["LowPriorityIO"])

    def test_interval_is_overridable(self) -> None:
        p = run("--dry-run", env={"PULP_SHIPYARD_AUTOUPDATE_INTERVAL": "900"})
        self.assertEqual(plistlib.loads(p.stdout.encode())["StartInterval"], 900)


class TestArgs(unittest.TestCase):
    def test_unknown_flag_is_rejected(self) -> None:
        p = run("--bogus")
        self.assertEqual(p.returncode, 2)
        self.assertIn("usage", p.stderr)

    def test_status_is_read_only_and_succeeds(self) -> None:
        p = run("--status")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("shipyard-autoupdate status", p.stdout)

    def test_status_reports_the_kill_switch_location(self) -> None:
        p = run("--status")
        self.assertIn("shipyard-autoupdate", p.stdout)
        self.assertIn("switch:", p.stdout)


class TestGuards(unittest.TestCase):
    def test_install_refuses_a_repo_without_a_pin_file(self) -> None:
        # Guards the "installed from the wrong directory" mistake before
        # it becomes a silently broken agent.
        p = run(env={"PULP_SHIPYARD_AUTOUPDATE_REPO": "/tmp"})
        self.assertEqual(p.returncode, 1)
        self.assertIn("shipyard.toml", p.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
