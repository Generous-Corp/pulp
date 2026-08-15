#!/usr/bin/env python3
"""Static contract tests for the repository-agnostic Proxmox runner."""

from pathlib import Path
import re
import unittest


SCRIPT = Path(__file__).with_name("proxmox-ephemeral-runner-linux.sh")
SERVICE = Path(__file__).with_name("proxmox-ephemeral-pool@.service")


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
        self.assertIn('RUNNER_NAME="${RUNNER_NAME_PREFIX}-${VMID}"', self.text)
        self.assertIn("runner name exceeds GitHub's 64-character limit", self.text)
        self.assertIn('-v name="$RUNNER_NAME" \'$2 == name\'', self.text)

    def test_command_overrides_are_available_for_one_shot_proof(self) -> None:
        for option in ("--repo", "--labels", "--golden", "--name-prefix", "--once"):
            self.assertRegex(self.text, rf'{re.escape(option)}\)')

    def test_isolation_guards_remain_in_the_generic_path(self) -> None:
        for marker in (
            "--ephemeral",
            "TARTCI_RUNNER_NETWORK_ISOLATION",
            "TARTCI_PROXMOX_GUEST_IPV4_FIRST_OCTET",
            "AUTOMATIC_NETWORK_ISOLATION=1",
            "deregistered runner id",
            "destroying clone",
        ):
            self.assertIn(marker, self.text)

    def test_jit_config_does_not_use_registration_token_path(self) -> None:
        self.assertIn("actions/runners/generate-jitconfig", self.text)
        self.assertIn("encoded_jit_config", self.text)
        self.assertIn("install -m 600 /dev/stdin", self.text)
        self.assertIn("JIT_GUEST_FILE=/tmp/tartci-jit-config", self.text)
        self.assertIn("./run.sh --jitconfig", self.text)
        self.assertNotIn("registration-token", self.text)
        self.assertNotIn("config.sh --unattended", self.text)
        self.assertIn("guest IP ${GUEST_IP} is already assigned", self.text)

    def test_jit_runner_requires_github_visible_readiness(self) -> None:
        self.assertIn("TARTCI_RUNNER_READY_TIMEOUT_SECONDS", self.text)
        self.assertIn("never became visible to GitHub", self.text)
        self.assertIn('ready_status" = online', self.text)
        self.assertIn('kill "$RUNNER_PID"', self.text)
        self.assertIn('wait "$RUNNER_PID"', self.text)

    def test_jit_runner_has_bounded_heartbeat_watchdog(self) -> None:
        self.assertIn("HEARTBEAT_FAILURE_FILE", self.text)
        self.assertIn("cachebust=$(date +%s)", self.text)
        self.assertIn("lost GitHub heartbeat", self.text)
        self.assertIn("misses=$((misses + 1))", self.text)

    def test_systemd_profile_is_repository_agnostic(self) -> None:
        service = SERVICE.read_text()
        self.assertIn("/etc/pulp/proxmox-runner/%i.env", service)
        self.assertIn("proxmox-ephemeral-runner-linux.sh --once", service)
        self.assertNotIn("pulp-ephemeral-runner.sh", service)


if __name__ == "__main__":
    unittest.main()
