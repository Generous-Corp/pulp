#!/usr/bin/env python3
"""Static safety contract for the disposable Mac Pro Linux supervisor."""

from __future__ import annotations

import pathlib
import subprocess
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "ci" / "proxmox-ephemeral-runner-linux.sh"
SERVICE = ROOT / "tools" / "ci" / "pulp-ephemeral-pool@.service"
GUIDE = ROOT / "docs" / "guides" / "local-ci.md"


class ProxmoxEphemeralRunnerLinuxTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.script = SCRIPT.read_text(encoding="utf-8")
        cls.service = SERVICE.read_text(encoding="utf-8")
        cls.guide = GUIDE.read_text(encoding="utf-8")

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

    def test_shutdown_fences_only_idle_runner_before_deregistration(self) -> None:
        cleanup = self.script[
            self.script.index("cleanup() {") : self.script.index("trap cleanup EXIT")
        ]
        self.assertIn('exact runner is busy; leaving clone $VMID', cleanup)
        self.assertIn('"${REGISTRATION_API}/actions/runners/${rid}/labels"', cleanup)
        self.assertIn("-f 'labels[]=pulp-shutdown-fenced'", cleanup)
        self.assertNotIn('AUTOMATIC_NETWORK_ISOLATION" = 1', cleanup)
        self.assertNotIn('if [ "$runner_status" = online ]; then', cleanup)
        self.assertIn('cannot fence automatic dispatch', cleanup)
        self.assertIn('exact runner became busy before dispatch fence', cleanup)
        self.assertIn('a routing label survived dispatch fence', cleanup)
        self.assertIn('pulp-auto-linux-x64', cleanup)
        self.assertIn('pulp-build-linux-x64', cleanup)
        self.assertIn('pulp-host-macpro', cleanup)
        self.assertIn('shutdown label is missing after dispatch fence', cleanup)
        self.assertIn("for fence_probe in 1 2; do", cleanup)
        self.assertIn('fenced automatic dispatch for idle runner id $rid', cleanup)
        self.assertIn("shutdown_deadline=$((SECONDS + 120))", cleanup)
        self.assertIn('timeout 20s qm stop "$VMID"', cleanup)
        self.assertIn('fenced runner is busy during shutdown', cleanup)
        self.assertIn('fenced runner stayed online before cleanup deadline', cleanup)
        self.assertIn('runner deregistered itself during fenced shutdown', cleanup)
        self.assertLess(
            cleanup.index('"${REGISTRATION_API}/actions/runners/${rid}/labels"'),
            cleanup.index('"${REGISTRATION_API}/actions/runners/${rid}"'),
        )
        self.assertLess(
            cleanup.index('"${REGISTRATION_API}/actions/runners/${rid}"'),
            cleanup.index('qm stop "$VMID"'),
        )
        self.assertLess(
            cleanup.index('qm stop "$VMID"'),
            cleanup.rindex('[ "$runner_status" = offline ]'),
        )

    def test_controller_dependencies_and_org_credential_are_fail_closed(self) -> None:
        self.assertIn('command -v "$GH_CLI"', self.script)
        self.assertIn('RT="$(GH_TOKEN="$PAT" "$GH_CLI" api --method POST', self.script)
        self.assertNotIn('Authorization: Bearer $PAT', self.script)
        self.assertLess(
            self.script.index('command -v "$GH_CLI"'),
            self.script.index("# ── admission"),
        )
        self.assertIn("gh-org-runner-pat", self.script)
        self.assertIn(
            'automatic Linux runner organization PAT is missing', self.script
        )
        self.assertIn("apt-get install -y gh", self.guide)

    def test_automatic_pool_requires_private_network_isolation(self) -> None:
        self.assertIn(
            'automatic Linux runners require the Proxmox firewall', self.script
        )
        self.assertIn('NET0="${NET0},firewall=1"', self.script)
        self.assertIn("ipfilter: 1", self.script)
        self.assertIn("layer2_protocols: ARP,IPv4", self.script)
        self.assertIn("[IPSET ipfilter-net0]", self.script)
        self.assertIn("${GUEST_IP}", self.script)
        self.assertIn('cannot write automatic runner firewall policy', self.script)
        self.assertIn('cannot install automatic runner firewall policy', self.script)
        self.assertIn('"$FIREWALL_STATUS_BIN" compile', self.script)
        self.assertIn("firewall_policy_active=0", self.script)
        self.assertIn('[ "$firewall_policy_active" = 1 ]', self.script)
        self.assertIn(
            '"$($FIREWALL_STATUS_BIN status 2>/dev/null)" = "Status: enabled/running"',
            self.script,
        )
        self.assertGreater(
            self.script.index("firewall_policy_active=0"),
            self.script.index('"$FIREWALL_STATUS_BIN" compile'),
        )
        self.assertIn('automatic runner firewall policy is not active', self.script)
        self.assertIn('automatic runner firewall rules are not installed', self.script)
        self.assertIn('iptables-save', self.script)
        self.assertIn('ip6tables-save', self.script)
        self.assertIn('ebtables-save', self.script)
        self.assertIn('ipset test "PVEFW-${VMID}-ipfilter-net0-v4"', self.script)
        self.assertIn("ipv6_drop_installed=0", self.script)
        self.assertIn(
            'grep -Fxq -- "-A tap${VMID}i0-OUT -j DROP"', self.script
        )
        self.assertIn('[ "$ipv6_drop_installed" = 1 ]', self.script)
        self.assertIn('--arp-ip-src ${GUEST_IP} -j RETURN', self.script)
        self.assertIn('-A tap${VMID}i0-OUT-ARP -j DROP', self.script)
        self.assertIn('-A tap${VMID}i0-OUT -j tap${VMID}i0-OUT-PROTO', self.script)
        self.assertIn('-A tap${VMID}i0-OUT-PROTO -p ARP -j RETURN', self.script)
        self.assertIn('-A tap${VMID}i0-OUT-PROTO -p IPv4 -j RETURN', self.script)
        self.assertIn(
            '! grep -Fq -- "-A tap${VMID}i0-OUT-PROTO -p IPv6 -j RETURN"',
            self.script,
        )
        self.assertIn('-A tap${VMID}i0-OUT-PROTO -j DROP', self.script)
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
