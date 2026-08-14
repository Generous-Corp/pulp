#!/usr/bin/env python3
"""Static safety contract for the disposable Mac Pro Linux supervisor."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
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

    def test_org_registration_executes_under_nounset(self) -> None:
        assignments = self.script[
            self.script.index('REPO="') : self.script.index("log() {")
        ]
        registration = self.script[
            self.script.index("# Repository runners remain dispatch-only.")
            : self.script.index("# ── admission")
        ]
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = pathlib.Path(temporary_directory)
            binaries = temporary / "bin"
            binaries.mkdir()
            for name in (
                "gh",
                "iptables-save",
                "ip6tables-save",
                "ipset",
                "ebtables-save",
            ):
                executable = binaries / name
                executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
                executable.chmod(0o755)

            firewall = binaries / "pve-firewall"
            firewall.write_text(
                '#!/bin/sh\n[ "$1" = status ] && echo "Status: enabled/running"\n',
                encoding="utf-8",
            )
            firewall.chmod(0o755)
            verifier = temporary / "verify-group.py"
            verifier.write_text('print("Pulp Automatic Linux")\n', encoding="utf-8")
            token = temporary / "org-token"
            token.write_text("test-token\n", encoding="utf-8")
            firewall_directory = temporary / "firewall"
            firewall_directory.mkdir()

            harness = "\n".join(
                (
                    "set -uo pipefail",
                    assignments,
                    "log() { :; }",
                    'die() { printf "ERROR: %s\\n" "$*" >&2; exit 1; }',
                    'PAT="repository-token"',
                    registration,
                    'printf "%s\\n" "$REGISTRATION_API|$RUNNER_URL|'
                    '$RUNNER_GROUP_ARG|$LABELS|$AUTOMATIC_NETWORK_ISOLATION"',
                )
            )
            environment = {
                "PATH": f"{binaries}:{pathlib.Path('/usr/bin')}:/bin",
                "PULP_LINUX_RUNNER_GROUP_ID": "42",
                "PULP_LINUX_ORG_PAT_FILE": str(token),
                "PULP_LINUX_GROUP_VERIFIER": str(verifier),
                "PULP_LINUX_GH_CLI": str(binaries / "gh"),
                "PULP_LINUX_FIREWALL_STATUS_BIN": str(firewall),
                "PULP_LINUX_FIREWALL_DIR": str(firewall_directory),
            }
            result = subprocess.run(
                ["/bin/bash"],
                input=harness,
                capture_output=True,
                text=True,
                env=environment,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                result.stdout.strip(),
                "orgs/Generous-Corp|https://github.com/Generous-Corp|"
                "--runnergroup Pulp Automatic Linux|"
                "self-hosted,Linux,X64,pulp-build-linux-x64,pulp-host-macpro,"
                "pulp-auto-linux-x64|1",
            )

    def test_all_registration_lifecycle_calls_follow_the_selected_scope(self) -> None:
        self.assertGreaterEqual(self.script.count("${REGISTRATION_API}/actions/runners"), 3)
        self.assertNotIn("api.github.com/repos/${REPO}/actions/runners", self.script)

    def test_group_configuration_is_optional_and_root_managed(self) -> None:
        self.assertIn(
            "EnvironmentFile=-/etc/pulp/linux-runner-group-%i.env", self.service
        )
        self.assertLess(
            self.service.index(
                "EnvironmentFile=-/etc/pulp/linux-runner-group.env\n"
            ),
            self.service.index(
                "EnvironmentFile=-/etc/pulp/linux-runner-group-%i.env"
            ),
        )
        self.assertIn("loaded last so it can override", self.service)
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
        self.assertIn("layer2_protocols: ARP,IPv4", self.script)
        self.assertIn("[IPSET ipfilter-net0]", self.script)
        self.assertIn("${GUEST_IP}", self.script)
        self.assertIn('cannot write automatic runner firewall policy', self.script)
        self.assertIn('cannot install automatic runner firewall policy', self.script)
        self.assertIn('"$FIREWALL_STATUS_BIN" compile', self.script)
        self.assertIn('automatic runner firewall policy is not active', self.script)
        self.assertIn('automatic runner firewall rules are not installed', self.script)
        self.assertIn('iptables-save', self.script)
        self.assertIn('ip6tables-save', self.script)
        self.assertIn('ebtables-save', self.script)
        self.assertIn('ipset test "PVEFW-${VMID}-ipfilter-net0-v4"', self.script)
        self.assertIn('[ "$ipv6_drop_installed" = 1 ]', self.script)
        self.assertIn('--arp-ip-src ${GUEST_IP} -j RETURN', self.script)
        self.assertIn('-A tap${VMID}i0-OUT-ARP -j DROP', self.script)
        self.assertIn(
            '-A tap${VMID}i0-OUT -j tap${VMID}i0-OUT-PROTO', self.script
        )
        self.assertIn(
            '-A tap${VMID}i0-OUT-PROTO -p ARP -j RETURN', self.script
        )
        self.assertIn(
            '-A tap${VMID}i0-OUT-PROTO -p IPv4 -j RETURN', self.script
        )
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
        self.assertIn("destroy_clone_and_firewall_policy", self.script)
        self.assertIn('VMID_LOCK=/var/lock/pulp-ephemeral-vmid.lock', self.script)
        self.assertIn('exec 9>"$VMID_LOCK"', self.script)
        self.assertIn('rm -f -- "$firewall_file"', self.script)


if __name__ == "__main__":
    unittest.main()
