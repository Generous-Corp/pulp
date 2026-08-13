#!/usr/bin/env python3
"""Static safety contract for the disposable Mac Pro Linux supervisor."""

from __future__ import annotations

import pathlib
import subprocess
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "ci" / "proxmox-ephemeral-runner-linux.sh"
SERVICE = ROOT / "tools" / "ci" / "pulp-ephemeral-pool@.service"


class ProxmoxEphemeralRunnerLinuxTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.script = SCRIPT.read_text(encoding="utf-8")
        cls.service = SERVICE.read_text(encoding="utf-8")

    def test_shell_is_syntactically_valid(self) -> None:
        result = subprocess.run(
            ["/bin/bash", "-n", str(SCRIPT)], capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_repository_registration_remains_the_default(self) -> None:
        self.assertIn('REGISTRATION_API="repos/${REPO}"', self.script)
        self.assertIn('RUNNER_URL="https://github.com/${REPO}"', self.script)
        self.assertIn('RUNNER_GROUP_ID="${PULP_LINUX_RUNNER_GROUP_ID:-}"', self.script)

    def test_org_registration_requires_the_fail_closed_verifier(self) -> None:
        self.assertIn('REGISTRATION_API="orgs/${ORG}"', self.script)
        self.assertIn('RUNNER_URL="https://github.com/${ORG}"', self.script)
        self.assertIn('--gh "$GH_CLI" --repo "$REPO" --group-id "$RUNNER_GROUP_ID"', self.script)
        self.assertIn('automatic Linux runner group policy is not fail-closed', self.script)
        self.assertIn('LABELS="${LABELS},pulp-auto-linux-x64"', self.script)
        self.assertIn('RUNNER_GROUP_ARG="--runnergroup ${GROUP_NAME}"', self.script)

    def test_all_registration_lifecycle_calls_follow_the_selected_scope(self) -> None:
        self.assertGreaterEqual(self.script.count("${REGISTRATION_API}/actions/runners"), 3)
        self.assertNotIn("api.github.com/repos/${REPO}/actions/runners", self.script)

    def test_group_configuration_is_optional_and_root_managed(self) -> None:
        self.assertIn(
            "EnvironmentFile=-/etc/pulp/linux-runner-group.env", self.service
        )
        self.assertIn("User=root", self.service)

    def test_slot_identity_is_stable_but_registration_name_is_per_boot(self) -> None:
        self.assertIn('RUNNER_SLOT_ID="macpro-linux-${VMID}"', self.script)
        self.assertIn(
            'RUNNER_NAME="pulp-ci-ephemeral-${VMID}-$(cat /proc/sys/kernel/random/uuid)"',
            self.script,
        )
        self.assertIn("slot ${RUNNER_SLOT_ID}", self.script)

    def test_stale_reclamation_is_slot_scoped_and_busy_safe(self) -> None:
        self.assertIn("reclaim_stale_slot_runners", self.script)
        self.assertIn('name.startswith(prefix)', self.script)
        self.assertIn('if [ "$stale_busy" = true ]', self.script)
        self.assertIn('refusing to reclaim busy stale registration', self.script)
        self.assertIn('runner_id = runner.get("id", "")', self.script)


if __name__ == "__main__":
    unittest.main()
