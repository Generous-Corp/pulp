#!/usr/bin/env python3
"""Safety contract for the Proxmox CI host-network helper."""

from __future__ import annotations

import pathlib
import os
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "ci" / "configure-proxmox-ci-network.sh"
QUALITY_TESTS = ROOT / "test" / "cmake" / "quality_tests.cmake"


class ConfigureProxmoxCiNetworkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.script = SCRIPT.read_text(encoding="utf-8")
        cls.quality_tests = QUALITY_TESTS.read_text(encoding="utf-8")

    def test_shell_is_syntactically_valid(self) -> None:
        result = subprocess.run(
            ["/bin/bash", "-n", str(SCRIPT)], capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_unknown_mode_fails_without_mutation(self) -> None:
        result = subprocess.run(
            ["/bin/bash", str(SCRIPT), "--not-a-mode"], capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 64)
        self.assertIn("Usage:", result.stderr)

    def test_exact_three_slot_point_to_point_contract(self) -> None:
        result = subprocess.run(
            ["/bin/bash", "-c", f"source {SCRIPT!s}; render_interfaces"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        for vmid in range(200, 203):
            self.assertIn(f"vmbr-ci{vmid}", result.stdout)
            self.assertIn(f"10.240.{vmid}.1/30", result.stdout)
        self.assertNotIn("bridge-ports vmbr0", result.stdout)
        self.assertIn("bridge-ports none", result.stdout)
        self.assertEqual(
            result.stdout.count(
                "post-up /usr/local/sbin/configure-proxmox-ci-network --ensure-nat"
            ),
            3,
        )
        self.assertEqual(
            result.stdout.count(
                "pre-down /usr/local/sbin/configure-proxmox-ci-network --remove-nat"
            ),
            3,
        )

    EXPECTED_POLICY = [
        "DROP -i vmbr-ci200 -d 10.0.0.0/8",
        "DROP -i vmbr-ci200 -d 172.16.0.0/12",
        "DROP -i vmbr-ci200 -d 192.168.0.0/16",
        "ACCEPT -i vmbr-ci200 -o vmbr0",
        "ACCEPT -o vmbr-ci200 -m conntrack --ctstate ESTABLISHED,RELATED",
        "DROP -i vmbr-ci200",
    ]

    def _filter_specs(self, bridge: str = "vmbr-ci200") -> list[str]:
        result = subprocess.run(
            ["/bin/bash", "-c", f"source {SCRIPT!s}; filter_specs {bridge}"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return [line for line in result.stdout.splitlines() if line]

    def test_egress_policy_denies_rfc1918_before_allowing_the_uplink(self) -> None:
        self.assertEqual(self._filter_specs(), self.EXPECTED_POLICY)

    def test_egress_policy_ends_in_a_catch_all_deny(self) -> None:
        """Default-deny must not rely on the FORWARD chain's own policy.

        Proxmox ships `-P FORWARD ACCEPT`, so a guest packet matching no earlier
        rule is accepted unless the policy terminates in its own DROP.
        """
        specs = self._filter_specs()
        self.assertEqual(specs[-1], "DROP -i vmbr-ci200")
        uplink = specs.index("ACCEPT -i vmbr-ci200 -o vmbr0")
        for rfc1918 in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"):
            with self.subTest(subnet=rfc1918):
                self.assertLess(
                    specs.index(f"DROP -i vmbr-ci200 -d {rfc1918}"),
                    uplink,
                    "RFC1918 must be denied before the uplink is allowed",
                )

    def _count_with_stub_table(self, rules: list[str]) -> str:
        """Run count_filter_rules against a stubbed iptables-save table."""
        with tempfile.TemporaryDirectory() as tmp:
            stub = pathlib.Path(tmp) / "iptables-save"
            body = "\n".join(rules)
            stub.write_text(f"#!/bin/sh\ncat <<'TABLE'\n{body}\nTABLE\n", encoding="utf-8")
            stub.chmod(0o755)
            result = subprocess.run(
                [
                    "/bin/bash",
                    "-c",
                    f'PATH="{tmp}:$PATH"; source {SCRIPT!s}; count_filter_rules vmbr-ci200',
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            return result.stdout.strip()

    def test_verify_detects_a_single_removed_deny_rule(self) -> None:
        """The count must drop when one rule is missing, or verify cannot fail.

        A checker that only proves a bridge exists would go green on today's
        state: bridges up, NAT present, and no egress filtering at all.
        """
        full = [
            f'-A FORWARD {spec.split(" ", 1)[1]} -m comment '
            f'--comment "pulp-ci-isolation:vmbr-ci200" -j {spec.split(" ", 1)[0]}'
            for spec in self.EXPECTED_POLICY
        ]
        self.assertEqual(self._count_with_stub_table(full), str(len(self.EXPECTED_POLICY)))
        sabotaged = [r for r in full if "10.0.0.0/8" not in r]
        self.assertEqual(
            self._count_with_stub_table(sabotaged),
            str(len(self.EXPECTED_POLICY) - 1),
            "removing one DROP rule must change the count verify asserts on",
        )
        self.assertEqual(self._count_with_stub_table([]), "0")

    def test_verify_asserts_the_egress_policy_not_just_the_bridge(self) -> None:
        self.assertTrue(
            "is not default-deny" in self.script,
            "verify_live must fail when the egress policy is incomplete",
        )
        self.assertTrue(
            "ensure_one_filter" in self.script and "remove_one_filter" in self.script,
            "the per-bridge hooks must restore and remove the egress policy",
        )

    def test_custom_vmid_range_renders_disjoint_point_to_point_bridge(self) -> None:
        result = subprocess.run(
            ["/bin/bash", "-c", f"source {SCRIPT!s}; render_interfaces"],
            capture_output=True,
            text=True,
            env={
                **os.environ,
                "TARTCI_PROXMOX_CLONE_BASE": "203",
                "TARTCI_PROXMOX_CLONE_MAX": "203",
            },
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("auto vmbr-ci203", result.stdout)
        self.assertIn("address 10.240.203.1/30", result.stdout)
        self.assertIn("--ensure-nat vmbr-ci203", result.stdout)
        self.assertNotIn("vmbr-ci200", result.stdout)

    def test_apply_does_not_reload_or_reconfigure_management_bridge(self) -> None:
        apply_body = self.script.split("apply_network() {", 1)[1].split(
            "rollback_network() {", 1
        )[0]
        self.assertNotIn("ifreload", apply_body)
        self.assertNotIn('ifup "$MANAGEMENT_BRIDGE"', apply_body)
        self.assertIn('before="$(management_signature)"', apply_body)
        self.assertIn('[ "$after" = "$before" ]', apply_body)
        self.assertIn('[ -d "/sys/class/net/$bridge/bridge" ] || ifup "$bridge"', apply_body)

    def test_mutating_modes_require_root_and_proxmox(self) -> None:
        self.assertEqual(self.script.count('requires root"'), 4)
        self.assertIn('[ -d /etc/pve ]', self.script)
        self.assertIn("refusing to configure a non-Proxmox host", self.script)
        self.assertIn("does not source interfaces.d", self.script)

    def test_apply_is_idempotent_and_nat_is_exactly_once(self) -> None:
        self.assertIn("refuse_conflicts", self.script)
        self.assertIn("has duplicate managed NAT rules", self.script)
        self.assertIn("cannot prove exact managed NAT", self.script)
        self.assertIn('must have exactly one managed NAT rule (found $count)', self.script)
        self.assertIn('file_is_exact "$INTERFACES_FILE"', self.script)
        self.assertIn('file_is_exact "$SYSCTL_FILE"', self.script)
        self.assertIn("stat -c '%u:%g:%a'", self.script)
        self.assertIn('state_is_valid || die "$STATE_FILE is absent, insecure, or invalid"', self.script)
        self.assertEqual(self.script.count('flock -w 30 9'), 3)
        self.assertEqual(self.script.count('flock -w 300 8'), 2)

    def test_managed_file_negative_controls_reject_drift_and_weak_mode(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            tmp = pathlib.Path(raw_tmp)
            interfaces = tmp / "interfaces"
            sysctl = tmp / "sysctl"
            harness = f"""
set -euo pipefail
source {SCRIPT!s}
INTERFACES_FILE={interfaces!s}
SYSCTL_FILE={sysctl!s}
BRIDGES=(pulp-test-missing-a pulp-test-missing-b pulp-test-missing-c)
render_interfaces > "$INTERFACES_FILE"
render_sysctl > "$SYSCTL_FILE"
chmod 0644 "$INTERFACES_FILE" "$SYSCTL_FILE"
if [ "$EUID" = 0 ]; then
    file_is_exact "$INTERFACES_FILE" render_interfaces
    refuse_conflicts
else
    if file_is_exact "$INTERFACES_FILE" render_interfaces; then exit 90; fi
fi
chmod 0666 "$INTERFACES_FILE"
if file_is_exact "$INTERFACES_FILE" render_interfaces; then exit 91; fi
chmod 0644 "$INTERFACES_FILE"
printf '# drift\n' >> "$INTERFACES_FILE"
if file_is_exact "$INTERFACES_FILE" render_interfaces; then exit 92; fi
"""
            result = subprocess.run(
                ["/bin/bash", "-c", harness], capture_output=True, text=True
            )
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_rollback_fails_closed_on_drift_or_attached_guest(self) -> None:
        rollback = self.script.split("rollback_network() {", 1)[1]
        self.assertIn("refusing rollback", rollback)
        self.assertIn('ports=("/sys/class/net/$bridge/brif/"*)', rollback)
        self.assertIn("stop its VM first", rollback)
        self.assertLess(rollback.index("remove_nat"), rollback.index('rm -f -- "$INTERFACES_FILE"'))
        self.assertIn('net.ipv4.ip_forward=$previous_forward', rollback)
        self.assertLess(
            rollback.index('net.ipv4.ip_forward=$previous_forward'),
            rollback.index('rm -f -- "$INTERFACES_FILE"'),
        )
        self.assertIn("already rolled back", rollback)

    def test_nat_removal_counts_then_proves_absence(self) -> None:
        removal = self.script.split("remove_nat() {", 1)[1].split(
            "verify_live() {", 1
        )[0]
        self.assertNotIn("nat_rule -C", removal)
        self.assertIn("count_nat_rules", removal)
        self.assertIn("managed NAT rule survived removal", removal)
        counter = self.script.split("count_nat_rules() {", 1)[1].split(
            "remove_nat() {", 1
        )[0]
        self.assertIn('rules="$(iptables-save -t nat)"', counter)
        self.assertNotIn("|| true", counter)
        one_removal = self.script.split("remove_one_nat() {", 1)[1].split(
            "count_nat_rules() {", 1
        )[0]
        self.assertIn('while [ "$count" -gt 0 ]', one_removal)
        self.assertNotIn("does not have exactly one", one_removal)

    def test_dry_run_cannot_reach_mutators(self) -> None:
        dry_run = self.script.split("dry_run() {", 1)[1].split("apply_network() {", 1)[0]
        for mutator in ("\n    install ", "\n    ifup ", "\n    iptables ", "sysctl -q", "rm -f"):
            self.assertNotIn(mutator, dry_run)
        self.assertIn("refuse_conflicts", dry_run)

    def test_registered_as_a_linux_selftest(self) -> None:
        linux_block = self.quality_tests.split(
            'if(CMAKE_SYSTEM_NAME STREQUAL "Linux")', 1
        )[1].split("endif()", 1)[0]
        self.assertIn("proxmox-ci-host-network-selftest", linux_block)


if __name__ == "__main__":
    unittest.main()
