#!/usr/bin/env python3
"""Static contract tests for the repository-agnostic Proxmox runner."""

from pathlib import Path
import re
import unittest


SCRIPT = Path(__file__).with_name("proxmox-ephemeral-runner-linux.sh")


class ProxmoxRunnerContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = SCRIPT.read_text()

    def test_repository_identity_is_overrideable(self) -> None:
        self.assertIn('TARTCI_RUNNER_REPO:-${PULP_RUNNER_REPO:-Generous-Corp/pulp}', self.text)
        self.assertIn('TARTCI_RUNNER_LABELS:-${PULP_RUNNER_LABELS:-', self.text)
        self.assertIn('TARTCI_RUNNER_PAT_FILE:-${PULP_RUNNER_PAT_FILE:-', self.text)

    def test_runner_and_vm_names_are_not_pulp_hard_coded(self) -> None:
        self.assertIn('TARTCI_RUNNER_NAME_PREFIX:-', self.text)
        self.assertIn('TARTCI_PROXMOX_VM_NAME_PREFIX:-', self.text)
        self.assertNotRegex(self.text, r'--name "pulp-ci-ephemeral-\$VMID"')

    def test_command_overrides_are_available_for_one_shot_proof(self) -> None:
        for option in ("--repo", "--labels", "--golden", "--name-prefix", "--once"):
            self.assertRegex(self.text, rf'{re.escape(option)}\)')

    def test_isolation_guards_remain_in_the_generic_path(self) -> None:
        for marker in (
            "--ephemeral",
            "TARTCI_RUNNER_NETWORK_ISOLATION",
            "AUTOMATIC_NETWORK_ISOLATION=1",
            "deregistered runner id",
            "destroying clone",
        ):
            self.assertIn(marker, self.text)


if __name__ == "__main__":
    unittest.main()
