#!/usr/bin/env python3
"""Static safety contract for the disposable Mac Pro Linux supervisor."""

from __future__ import annotations

import pathlib
import subprocess
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "ci" / "proxmox-ephemeral-runner-linux.sh"
SERVICE = ROOT / "tools" / "ci" / "pulp-ephemeral-pool@.service"
PR_SAFE_SERVICE = ROOT / "tools" / "ci" / "pulp-pr-safe-ephemeral-pool@.service"
PR_SAFE_WRAPPER = ROOT / "tools" / "ci" / "proxmox-pr-safe-ephemeral-runner-linux.sh"
TRUSTED_WRAPPER = ROOT / "tools" / "ci" / "proxmox-trusted-ephemeral-runner-linux.sh"


class ProxmoxEphemeralRunnerLinuxTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.script = SCRIPT.read_text(encoding="utf-8")
        cls.service = SERVICE.read_text(encoding="utf-8")
        cls.pr_safe_service = PR_SAFE_SERVICE.read_text(encoding="utf-8")
        cls.pr_safe_wrapper = PR_SAFE_WRAPPER.read_text(encoding="utf-8")
        cls.trusted_wrapper = TRUSTED_WRAPPER.read_text(encoding="utf-8")

    def test_shell_is_syntactically_valid(self) -> None:
        result = subprocess.run(
            ["/bin/bash", "-n", str(SCRIPT)], capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_repository_registration_remains_the_default(self) -> None:
        self.assertIn('REGISTRATION_API="repos/${REPO}"', self.script)
        self.assertIn('RUNNER_URL="https://github.com/${REPO}"', self.script)
        self.assertIn('RUNNER_GROUP_ID="${PULP_LINUX_RUNNER_GROUP_ID:-}"', self.script)
        self.assertGreater(
            self.script.index('else\n    [ -r "$PAT_FILE" ]'),
            self.script.index('if [ -n "$RUNNER_GROUP_ID" ]'),
        )

    def test_org_registration_requires_the_fail_closed_verifier(self) -> None:
        self.assertIn('REGISTRATION_API="orgs/${ORG}"', self.script)
        self.assertIn('RUNNER_URL="https://github.com/${ORG}"', self.script)
        self.assertIn('--profile "$RUNNER_GROUP_PROFILE"', self.script)
        self.assertIn('automatic Linux runner group policy is not fail-closed', self.script)
        self.assertIn('RUNNER_GROUP_ARG="--runnergroup ${GROUP_NAME}"', self.script)

    def test_profiles_have_disjoint_groups_names_and_capability_labels(self) -> None:
        self.assertIn('PULP_LINUX_RUNNER_GROUP_ID="${PULP_TRUSTED_LINUX_RUNNER_GROUP_ID:-3}"', self.trusted_wrapper)
        self.assertIn('PULP_LINUX_RUNNER_GROUP_PROFILE="trusted"', self.trusted_wrapper)
        self.assertIn('PULP_LINUX_RUNNER_GROUP_ID="${PULP_PR_SAFE_LINUX_RUNNER_GROUP_ID:-5}"', self.pr_safe_wrapper)
        self.assertIn('PULP_LINUX_RUNNER_GROUP_PROFILE="pr-safe"', self.pr_safe_wrapper)
        self.assertNotIn("pulp-pr-safe-linux-x64", self.trusted_wrapper)
        self.assertNotIn("pulp-auto-linux-x64", self.pr_safe_wrapper)

    def test_all_registration_lifecycle_calls_follow_the_selected_scope(self) -> None:
        self.assertGreaterEqual(self.script.count("${REGISTRATION_API}/actions/runners"), 3)
        self.assertNotIn("api.github.com/repos/${REPO}/actions/runners", self.script)

    def test_group_configuration_is_optional_and_root_managed(self) -> None:
        self.assertIn(
            "EnvironmentFile=-/etc/pulp/linux-runner-group.env", self.service
        )
        self.assertIn("User=root", self.service)
        self.assertIn("Documentation=https://", self.service)
        self.assertIn("Documentation=https://", self.pr_safe_service)

    def test_systemd_can_run_both_profiles_concurrently(self) -> None:
        self.assertEqual(
            self.service.count(
                "ExecStart=/usr/local/sbin/proxmox-trusted-ephemeral-runner-linux.sh"
            ),
            1,
        )
        self.assertEqual(
            self.pr_safe_service.count(
                "ExecStart=/usr/local/sbin/proxmox-pr-safe-ephemeral-runner-linux.sh"
            ),
            1,
        )

    def test_slot_identity_is_stable_but_registration_name_is_per_boot(self) -> None:
        self.assertIn('RUNNER_SLOT_ID="macpro-linux-${VMID}"', self.script)
        self.assertIn(
            'RUNNER_NAME="${RUNNER_NAME_PREFIX}-${VMID}-$(cat /proc/sys/kernel/random/uuid)"',
            self.script,
        )
        self.assertIn("slot ${RUNNER_SLOT_ID}", self.script)

    def test_stale_reclamation_is_slot_scoped_and_busy_safe(self) -> None:
        self.assertIn("reclaim_stale_slot_runners", self.script)
        self.assertIn('index($2, prefix) == 1', self.script)
        self.assertIn('multiple registrations claim ${RUNNER_SLOT_ID}', self.script)
        self.assertIn('[ "$match_count" = 1 ]', self.script)
        self.assertIn('[ "$stale_busy" = false ]', self.script)
        self.assertIn('[ "$stale_status" = offline ]', self.script)
        self.assertIn('registration $stale_name is not offline', self.script)

    def test_runner_inventory_is_paginated_for_organization_scope(self) -> None:
        self.assertGreaterEqual(self.script.count('"$GH_CLI" api --paginate'), 2)
        self.assertGreaterEqual(
            self.script.count(".runners[] | [.id,.name,.busy,.status] | @tsv"), 2
        )
        self.assertNotIn("runner lookup exceeded one API page", self.script)

    def test_controller_dependencies_and_org_credential_are_fail_closed(self) -> None:
        self.assertIn('command -v "$GH_CLI"', self.script)
        self.assertLess(
            self.script.index('command -v "$GH_CLI"'),
            self.script.index("# ── admission"),
        )
        self.assertIn("gh-org-runner-pat", self.script)
        self.assertIn(
            'automatic Linux runner organization PAT is missing', self.script
        )

    def test_automatic_pool_requires_private_network_isolation(self) -> None:
        self.assertIn(
            'automatic Linux runners require the Proxmox firewall', self.script
        )
        self.assertIn('NET0="${NET0},firewall=1"', self.script)
        self.assertIn("ipfilter: 1", self.script)
        self.assertIn("[IPSET ipfilter-net0]", self.script)
        self.assertIn("${GUEST_IP}", self.script)
        self.assertIn('cannot write automatic runner firewall policy', self.script)
        self.assertIn('cannot install automatic runner firewall policy', self.script)
        self.assertIn('"$FIREWALL_STATUS_BIN" compile', self.script)
        self.assertIn('automatic runner firewall policy is not active', self.script)
        self.assertIn('automatic runner firewall rules are not installed', self.script)
        self.assertIn(
            '^-A tap${VMID}i0-OUT( -d ::/0)? -j DROP$', self.script
        )
        self.assertIn('iptables-save', self.script)
        self.assertIn('ip6tables-save', self.script)
        self.assertIn('ebtables-save', self.script)
        self.assertIn('ipset test "PVEFW-${VMID}-ipfilter-net0-v4"', self.script)
        self.assertIn('--arp-ip-src ${GUEST_IP} -j RETURN', self.script)
        self.assertIn('-A tap${VMID}i0-OUT-ARP -j DROP', self.script)
        self.assertNotIn('chmod 600 "$VM_FIREWALL_TMP"', self.script)
        self.assertGreater(
            self.script.index('qm set "$VMID"'),
            self.script.index('VM_FIREWALL_FILE="${FIREWALL_DIR}/${VMID}.fw"'),
        )
        self.assertLess(
            self.script.index('qm set "$VMID"'),
            self.script.index('"$FIREWALL_STATUS_BIN" compile'),
        )
        self.assertLess(
            self.script.index('automatic runner firewall rules are not installed'),
            self.script.index("# ── wait for the guest"),
        )
        self.assertIn(
            "OUT ACCEPT -dest ${GUEST_IPV4_GATEWAY} -p udp -dport 53",
            self.script,
        )
        for subnet in (
            "0.0.0.0/8",
            "10.0.0.0/8",
            "100.64.0.0/10",
            "127.0.0.0/8",
            "169.254.0.0/16",
            "172.16.0.0/12",
            "192.0.0.0/24",
            "192.0.2.0/24",
            "192.88.99.0/24",
            "192.168.0.0/16",
            "198.18.0.0/15",
            "198.51.100.0/24",
            "203.0.113.0/24",
            "224.0.0.0/4",
            "240.0.0.0/4",
            "::/0",
        ):
            self.assertIn(f"OUT DROP -dest {subnet}", self.script)
        self.assertIn('rm -f "$VM_FIREWALL_FILE"', self.script)


if __name__ == "__main__":
    unittest.main()
