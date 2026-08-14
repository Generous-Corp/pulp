#!/usr/bin/env python3
"""Static safety contract for the disposable Mac Pro Linux supervisor."""

from __future__ import annotations

import pathlib
import os
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "ci" / "proxmox-ephemeral-runner-linux.sh"
SERVICE = ROOT / "tools" / "ci" / "pulp-ephemeral-pool@.service"
TRUSTED_WRAPPER = ROOT / "tools" / "ci" / "proxmox-trusted-ephemeral-runner-linux.sh"
PR_SAFE_WRAPPER = ROOT / "tools" / "ci" / "proxmox-pr-safe-ephemeral-runner-linux.sh"
RELEASE_CONTROL_WRAPPER = ROOT / "tools" / "ci" / "proxmox-release-control-ephemeral-runner-linux.sh"
TRUSTED_SERVICE = ROOT / "tools" / "ci" / "pulp-trusted-ephemeral-pool@.service"
PR_SAFE_SERVICE = ROOT / "tools" / "ci" / "pulp-pr-safe-ephemeral-pool@.service"
RELEASE_CONTROL_SERVICE = ROOT / "tools" / "ci" / "pulp-release-control-ephemeral-pool@.service"
QUALITY_TESTS = ROOT / "test" / "cmake" / "quality_tests.cmake"


class ProxmoxEphemeralRunnerLinuxTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.script = SCRIPT.read_text(encoding="utf-8")
        cls.service = SERVICE.read_text(encoding="utf-8")
        cls.trusted_wrapper = TRUSTED_WRAPPER.read_text(encoding="utf-8")
        cls.pr_safe_wrapper = PR_SAFE_WRAPPER.read_text(encoding="utf-8")
        cls.release_control_wrapper = RELEASE_CONTROL_WRAPPER.read_text(encoding="utf-8")
        cls.trusted_service = TRUSTED_SERVICE.read_text(encoding="utf-8")
        cls.pr_safe_service = PR_SAFE_SERVICE.read_text(encoding="utf-8")
        cls.release_control_service = RELEASE_CONTROL_SERVICE.read_text(encoding="utf-8")
        cls.quality_tests = QUALITY_TESTS.read_text(encoding="utf-8")

    def test_shell_is_syntactically_valid(self) -> None:
        for script in (SCRIPT, TRUSTED_WRAPPER, PR_SAFE_WRAPPER, RELEASE_CONTROL_WRAPPER):
            result = subprocess.run(
                ["/bin/bash", "-n", str(script)], capture_output=True, text=True
            )
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_linux_supervisor_selftest_is_not_registered_on_macos(self) -> None:
        marker = 'if(CMAKE_SYSTEM_NAME STREQUAL "Linux")'
        self.assertIn(marker, self.quality_tests)
        linux_block = self.quality_tests.split(marker, 1)[1].split("endif()", 1)[0]
        self.assertIn("proxmox-ephemeral-linux-runner-selftest", linux_block)

    def test_repository_registration_remains_the_default(self) -> None:
        self.assertIn('REGISTRATION_API="repos/${REPO}"', self.script)
        self.assertIn('RUNNER_URL="https://github.com/${REPO}"', self.script)
        self.assertIn('RUNNER_GROUP_ID="${PULP_LINUX_RUNNER_GROUP_ID:-}"', self.script)

    def test_org_registration_requires_the_fail_closed_verifier(self) -> None:
        self.assertIn('REGISTRATION_API="orgs/${ORG}"', self.script)
        self.assertIn('RUNNER_URL="https://github.com/${ORG}"', self.script)
        self.assertIn('--gh "$GH_CLI" --repo "$REPO"', self.script)
        self.assertIn('--group-id "$RUNNER_GROUP_ID" --policy "$RUNNER_GROUP_POLICY"', self.script)
        self.assertIn('--policy "$RUNNER_GROUP_POLICY"', self.script)
        self.assertIn('automatic Linux runner group policy is not fail-closed', self.script)
        self.assertIn('EXPECTED_LABELS="${BASE_LABELS},pulp-auto-linux-x64"', self.script)
        self.assertIn('EXPECTED_LABELS="${BASE_LABELS},pulp-pr-safe-linux-x64"', self.script)
        self.assertIn('RELEASE_CONTROL_LABELS="${BASE_LABELS},pulp-release-control-linux-x64"', self.script)
        self.assertIn('runner labels do not match the exact policy', self.script)
        self.assertIn('runner name prefix does not match the exact policy', self.script)
        self.assertIn('RUNNER_GROUP_ARG="--runnergroup ${GROUP_NAME}"', self.script)
        self.assertIn(
            "release-control runner requires a tag-shaped workflow ref",
            self.script,
        )
        self.assertNotIn("semantic-version tag ref", self.script)
        self.assertIn(
            'GROUP_VERIFY_ARGS+=(--workflow-ref "$RELEASE_WORKFLOW_REF")',
            self.script,
        )

    def test_automatic_capabilities_require_an_org_group(self) -> None:
        self.assertIn(
            'automatic Linux capability labels require a verified organization runner group',
            self.script,
        )
        self.assertIn('*,pulp-auto-linux-x64,*', self.script)
        self.assertIn('*,pulp-pr-safe-linux-x64,*', self.script)
        self.assertIn('*,pulp-release-control-linux-x64,*', self.script)

    def test_all_registration_lifecycle_calls_follow_the_selected_scope(self) -> None:
        self.assertGreaterEqual(self.script.count("${REGISTRATION_API}/actions/runners"), 3)
        self.assertNotIn("api.github.com/repos/${REPO}/actions/runners", self.script)

    def test_group_configuration_is_optional_and_root_managed(self) -> None:
        self.assertIn(
            "EnvironmentFile=-/etc/pulp/linux-runner-group.env", self.service
        )
        self.assertIn("User=root", self.service)

    def test_trusted_and_pr_safe_pools_have_distinct_fail_closed_units(self) -> None:
        self.assertIn(
            "EnvironmentFile=/etc/pulp/linux-trusted-runner-group.env",
            self.trusted_service,
        )
        self.assertIn(
            "ExecStart=/usr/local/sbin/pulp-trusted-ephemeral-runner.sh",
            self.trusted_service,
        )
        self.assertIn(
            "EnvironmentFile=/etc/pulp/linux-pr-safe-runner-group.env",
            self.pr_safe_service,
        )
        self.assertIn(
            "ExecStart=/usr/local/sbin/pulp-pr-safe-ephemeral-runner.sh",
            self.pr_safe_service,
        )
        self.assertNotIn("EnvironmentFile=-", self.trusted_service)
        self.assertNotIn("EnvironmentFile=-", self.pr_safe_service)
        self.assertIn(
            "EnvironmentFile=/etc/pulp/linux-release-control-runner-group.env",
            self.release_control_service,
        )
        self.assertIn(
            "ExecStart=/usr/local/sbin/pulp-release-control-ephemeral-runner.sh",
            self.release_control_service,
        )
        self.assertNotIn("EnvironmentFile=-", self.release_control_service)
        self.assertIn("Restart=on-failure", self.release_control_service)
        self.assertNotIn("Restart=always", self.release_control_service)
        documentation = (
            "Documentation=https://github.com/Generous-Corp/pulp/blob/main/"
            "docs/guides/local-ci.md"
        )
        for service in (
            self.service,
            self.trusted_service,
            self.pr_safe_service,
            self.release_control_service,
        ):
            self.assertIn(documentation, service)
        self.assertIn("User=root", self.trusted_service)
        self.assertIn("User=root", self.pr_safe_service)
        self.assertIn("User=root", self.release_control_service)

    def test_wrappers_select_exact_disjoint_policies(self) -> None:
        self.assertIn('PULP_LINUX_RUNNER_GROUP_POLICY="trusted"', self.trusted_wrapper)
        self.assertIn('PULP_RUNNER_NAME_PREFIX="pulp-ci-ephemeral"', self.trusted_wrapper)
        self.assertIn("pulp-auto-linux-x64", self.trusted_wrapper)
        self.assertNotIn("pulp-pr-safe-linux-x64", self.trusted_wrapper)
        self.assertIn('PULP_LINUX_RUNNER_GROUP_POLICY="pr-safe"', self.pr_safe_wrapper)
        self.assertIn(
            'PULP_RUNNER_NAME_PREFIX="pulp-pr-safe-ephemeral"',
            self.pr_safe_wrapper,
        )
        self.assertIn("pulp-pr-safe-linux-x64", self.pr_safe_wrapper)
        self.assertNotIn("pulp-auto-linux-x64", self.pr_safe_wrapper)
        self.assertIn(
            'PULP_LINUX_RUNNER_GROUP_POLICY="release-control"',
            self.release_control_wrapper,
        )
        self.assertIn(
            'PULP_RUNNER_NAME_PREFIX="pulp-release-control-ephemeral"',
            self.release_control_wrapper,
        )
        self.assertIn("pulp-release-control-linux-x64", self.release_control_wrapper)
        self.assertNotIn("pulp-auto-linux-x64", self.release_control_wrapper)
        self.assertNotIn("pulp-pr-safe-linux-x64", self.release_control_wrapper)
        for wrapper in (
            self.trusted_wrapper,
            self.pr_safe_wrapper,
            self.release_control_wrapper,
        ):
            self.assertIn('exec "$SCRIPT_DIR/pulp-ephemeral-runner.sh"', wrapper)
            self.assertNotIn('exec "$SCRIPT_DIR/proxmox-ephemeral-runner-linux.sh"', wrapper)

    def test_slot_identity_is_stable_but_registration_name_is_per_boot(self) -> None:
        self.assertIn('RUNNER_SLOT_ID="${RUNNER_NAME_PREFIX}-${VMID}"', self.script)
        self.assertIn(
            'RUNNER_NAME="${RUNNER_SLOT_ID}-$(cat /proc/sys/kernel/random/uuid)"',
            self.script,
        )
        self.assertIn("slot ${RUNNER_SLOT_ID}", self.script)

    def test_vmid_lock_covers_clone_attachment_and_firewall_proof(self) -> None:
        allocation = self.script.split("# ── claim a clone id", 1)[1].split(
            "# ── wait for the guest", 1
        )[0]
        self.assertLess(allocation.index('qm clone "$GOLDEN"'), allocation.index('qm start "$VMID"'))
        self.assertLess(
            allocation.index('automatic runner firewall rules are not installed'),
            allocation.rindex("flock -u 9"),
        )
        self.assertIn(
            '--description "pulp-runner-generation=${RUNNER_NAME}"', allocation
        )
        self.assertLess(
            allocation.index('--description "pulp-runner-generation=${RUNNER_NAME}"'),
            allocation.rindex("flock -u 9"),
        )
        self.assertIn('exec 8>"$HOST_NETWORK_LOCK"', allocation)
        self.assertLess(allocation.index('flock -w 30 8'), allocation.index('flock -w 300 9'))
        self.assertGreater(allocation.rindex("flock -u 8"), allocation.index('qm start "$VMID"'))

    def test_stale_reclamation_is_slot_scoped_and_busy_safe(self) -> None:
        self.assertIn("reclaim_stale_slot_runners", self.script)
        self.assertIn('index($2, prefix) == 1', self.script)
        self.assertIn('multiple registrations claim ${RUNNER_SLOT_ID}', self.script)
        self.assertIn('[ "$match_count" = 1 ]', self.script)
        self.assertIn('[ "$stale_busy" = false ]', self.script)
        self.assertIn('[ "$stale_status" = offline ]', self.script)
        self.assertIn('registration $stale_name is not offline', self.script)

    def test_runner_inventory_is_paginated_for_organization_scope(self) -> None:
        self.assertGreaterEqual(self.script.count("github_api --paginate"), 2)
        self.assertGreaterEqual(
            self.script.count(".runners[] | [.id,.name,.busy,.status] | @tsv"), 2
        )
        self.assertNotIn("runner lookup exceeded one API page", self.script)

    def test_shutdown_fences_only_idle_runner_before_deregistration(self) -> None:
        delegation_start = self.script.index("\ndelegate_deferred_cleanup() {") + 1
        cleanup_start = self.script.index("\ncleanup() {") + 1
        delegation = self.script[
            delegation_start:cleanup_start
        ]
        cleanup = self.script[
            cleanup_start : self.script.index("trap cleanup EXIT")
        ]
        self.assertIn('systemd-run --quiet --collect', delegation)
        self.assertIn('--service-type=oneshot', delegation)
        self.assertIn('--property=User=root', delegation)
        self.assertIn('--property=Restart=on-failure', delegation)
        self.assertIn('--setenv="PULP_LINUX_ORG_PAT_FILE=$ORG_PAT_FILE"', delegation)
        self.assertIn(
            '--setenv="PULP_LINUX_GITHUB_AUTH_MODE=$GITHUB_AUTH_MODE"', delegation
        )
        self.assertIn('--setenv="PULP_LINUX_GH_CLI=$GH_CLI"', delegation)
        self.assertIn('--setenv="PULP_LINUX_FIREWALL_DIR=$FIREWALL_DIR"', delegation)
        self.assertIn('--deferred-cleanup "$VMID"', delegation)
        self.assertIn('runner is busy; delegated clone $VMID', delegation)
        self.assertEqual(cleanup.count("delegate_deferred_cleanup || true"), 2)
        self.assertIn('"${REGISTRATION_API}/actions/runners/${rid}/labels"', cleanup)
        self.assertIn("-f 'labels[]=pulp-shutdown-fenced'", cleanup)
        self.assertIn('cannot fence runner dispatch', cleanup)
        self.assertIn(
            'if [ "$runner_status" = online ] || [ "$runner_status" = offline ]; then',
            cleanup,
        )
        self.assertIn('exact runner has invalid fenced busy state', cleanup)
        self.assertIn('a routing label survived dispatch fence', cleanup)
        self.assertIn('pulp-auto-linux-x64', cleanup)
        self.assertIn('pulp-pr-safe-linux-x64', cleanup)
        self.assertIn('pulp-build-linux-x64', cleanup)
        self.assertIn('pulp-host-macpro', cleanup)
        self.assertIn('shutdown label is missing after dispatch fence', cleanup)
        self.assertIn("for fence_probe in 1 2; do", cleanup)
        self.assertIn('fenced dispatch for idle runner id $rid', cleanup)
        self.assertNotIn('online dispatch runner cannot be fenced', cleanup)
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

    def test_cleanup_fences_offline_runner_and_delegates_reconnect_race(self) -> None:
        helper_start = self.script.index("\ndelegate_deferred_cleanup() {") + 1
        helper = self.script[
            helper_start : self.script.index("trap cleanup EXIT")
        ]
        with tempfile.TemporaryDirectory() as raw_tmp:
            tmp = pathlib.Path(raw_tmp)
            delegated = tmp / "delegated"
            qm_called = tmp / "qm-called"
            harness = tmp / "harness.sh"
            harness.write_text(
                "#!/usr/bin/env bash\n"
                "set -u\n"
                "log() { printf '%s\\n' \"$*\"; }\n"
                f"systemd-run() {{ printf '%s\\n' \"$*\" > '{delegated}'; }}\n"
                f"qm() {{ : > '{qm_called}'; }}\n"
                "github_api() {\n"
                "  if [ \"${1:-}\" = --paginate ]; then\n"
                "    printf '17\\tpulp-pr-safe-ephemeral-200-test\\tfalse\\toffline\\n'\n"
                "  elif [ \"${1:-}\" = --method ]; then\n"
                "    :\n"
                "  else\n"
                "    printf 'pulp-pr-safe-ephemeral-200-test\\ttrue\\tonline\\tpulp-shutdown-fenced\\n'\n"
                "  fi\n"
                "}\n"
                "CLONED=1\nKEEP=0\nGITHUB_API_READY=1\n"
                "VMID=200\nRUNNER_NAME='pulp-pr-safe-ephemeral-200-test'\n"
                "REGISTRATION_API='orgs/Generous-Corp'\n"
                "GITHUB_AUTH_MODE='app-helper'\n"
                "ORG_PAT_FILE='/root/.config/pulp/secrets/gh-org-runner-pat'\n"
                "GH_CLI='/usr/local/bin/ghapp'\n"
                f"FIREWALL_DIR='{tmp}'\n"
                + helper
                + "\ncleanup\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                ["/bin/bash", str(harness)],
                capture_output=True,
                text=True,
                timeout=5,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(delegated.exists())
            self.assertIn("--deferred-cleanup 200", delegated.read_text())
            self.assertIn("delegated clone 200", result.stdout)
            self.assertFalse(qm_called.exists())

    def test_deferred_cleanup_is_bounded_and_preserves_busy_work(self) -> None:
        helper = self.script[
            self.script.index("deferred_cleanup() {") : self.script.index(
                'if [ "${1:-}" = "--deferred-cleanup" ]'
            )
        ]
        self.assertIn('case "$busy" in', helper)
        self.assertIn("true) sleep 15; continue", helper)
        self.assertIn("deadline=$((SECONDS + 4500))", helper)
        self.assertIn('legacy_credential="${4:-}"', helper)
        self.assertIn("invalid legacy deferred-cleanup credential path", helper)
        self.assertIn("labels[]=pulp-shutdown-fenced", helper)
        self.assertIn(
            'if [ "$status" = online ] || [ "$status" = offline ]; then', helper
        )
        self.assertIn("for fence_probe in 1 2; do", helper)
        self.assertIn("deferred-cleanup runner became busy before dispatch fence", helper)
        self.assertIn("shutdown label is missing after deferred-cleanup fence", helper)
        self.assertIn('exec 9>"$VMID_LOCK"', helper)
        self.assertIn('flock -w 300 9', helper)
        self.assertIn("description: pulp-runner-generation=", helper)
        self.assertIn('[ "$clone_generation" = "$runner_name" ]', helper)
        self.assertIn("belongs to a different clone generation", helper)
        self.assertIn("deferred cleanup found clone $vmid already absent", helper)
        self.assertIn(
            'die "cannot determine deferred-cleanup clone status"', helper
        )
        self.assertLess(
            helper.index('true) sleep 15; continue'),
            helper.index('exec 9>"$VMID_LOCK"'),
        )
        self.assertLess(helper.index('flock -w 300 9'), helper.index('qm stop "$vmid"'))
        self.assertLess(helper.index('qm stop "$vmid"'), helper.index('qm destroy "$vmid"'))

    def test_vmid_reuse_cannot_race_firewall_policy_removal(self) -> None:
        destroy_helper = self.script[
            self.script.index("destroy_clone_and_firewall_policy() {") :
            self.script.index("deferred_cleanup() {")
        ]
        self.assertIn('VMID_LOCK=/var/lock/pulp-ephemeral-vmid.lock', self.script)
        self.assertIn('exec 9>"$VMID_LOCK"', destroy_helper)
        self.assertIn('flock -w 300 9', destroy_helper)
        self.assertIn('qm destroy "$vmid" --purge', destroy_helper)
        self.assertIn('rm -f -- "$firewall_file"', destroy_helper)
        self.assertIn('flock -u 9', destroy_helper)
        self.assertLess(
            destroy_helper.index('flock -w 300 9'),
            destroy_helper.index('qm destroy "$vmid" --purge'),
        )
        self.assertLess(
            destroy_helper.index('qm destroy "$vmid" --purge'),
            destroy_helper.index('rm -f -- "$firewall_file"'),
        )
        self.assertLess(
            destroy_helper.index('rm -f -- "$firewall_file"'),
            destroy_helper.index('flock -u 9'),
        )
        self.assertEqual(
            self.script.count("destroy_clone_and_firewall_policy "),
            1,
            "normal cleanup must use the locked destroy helper",
        )

    def test_deferred_cleanup_keeps_firewall_when_vm_status_is_unknown(self) -> None:
        helper = self.script[
            self.script.index("deferred_cleanup() {") : self.script.index(
                'if [ "${1:-}" = "--deferred-cleanup" ]'
            )
        ]
        with tempfile.TemporaryDirectory() as raw_tmp:
            tmp = pathlib.Path(raw_tmp)
            firewall = tmp / "200.fw"
            firewall.write_text("protected", encoding="utf-8")
            destroyed = tmp / "destroyed"
            fake_qm = tmp / "qm"
            fake_qm.write_text(
                "#!/usr/bin/env bash\n"
                "case \"$1\" in\n"
                "  stop) exit 0 ;;\n"
                "  status) exit 1 ;;\n"
                f"  destroy) : > '{destroyed}'; exit 0 ;;\n"
                "  *) exit 1 ;;\n"
                "esac\n",
                encoding="utf-8",
            )
            fake_qm.chmod(0o755)
            harness = tmp / "harness.sh"
            harness.write_text(
                "#!/usr/bin/env bash\n"
                "set -u\n"
                "die() { printf '%s\\n' \"$*\"; exit 1; }\n"
                "flock() { :; }\n"
                "configure_github_auth() { :; }\n"
                "github_api() { :; }\n"
                f"ORG='Generous-Corp'\nREPO='Generous-Corp/pulp'\n"
                f"PAT_FILE='{tmp}/repo-token'\nORG_PAT_FILE='{tmp}/org-token'\n"
                "GITHUB_AUTH_MODE='token-file'\n"
                f"FIREWALL_DIR='{tmp}'\n"
                f"VMID_LOCK='{tmp}/vmid.lock'\n"
                + helper
                + "\ndeferred_cleanup 200 pulp-pr-safe-ephemeral-200-test orgs/Generous-Corp\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                ["/bin/bash", str(harness)],
                capture_output=True,
                text=True,
                env={**os.environ, "PATH": f"{tmp}:{os.environ['PATH']}"},
                # Production deliberately bounds `qm stop` at 20 seconds.
                # Leave enough harness headroom to observe that fail-closed path.
                timeout=25,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("cannot inspect VM inventory", result.stdout)
            self.assertTrue(firewall.exists())
            self.assertFalse(destroyed.exists())

    def test_deferred_cleanup_fences_offline_runner_before_reconnect_race(self) -> None:
        helper = self.script[
            self.script.index("deferred_cleanup() {") : self.script.index(
                'if [ "${1:-}" = "--deferred-cleanup" ]'
            )
        ]
        with tempfile.TemporaryDirectory() as raw_tmp:
            tmp = pathlib.Path(raw_tmp)
            firewall = tmp / "200.fw"
            firewall.write_text("protected", encoding="utf-8")
            qm_called = tmp / "qm-called"
            fake_qm = tmp / "qm"
            fake_qm.write_text(
                "#!/usr/bin/env bash\n"
                f": > '{qm_called}'\n"
                "exit 0\n",
                encoding="utf-8",
            )
            fake_qm.chmod(0o755)
            harness = tmp / "harness.sh"
            harness.write_text(
                "#!/usr/bin/env bash\n"
                "set -u\n"
                "die() { printf '%s\\n' \"$*\"; exit 1; }\n"
                "configure_github_auth() { :; }\n"
                "github_api() {\n"
                "  if [ \"${1:-}\" = --paginate ]; then\n"
                "    printf '17\\tpulp-pr-safe-ephemeral-200-test\\tfalse\\toffline\\n'\n"
                "  elif [ \"${1:-}\" = --method ]; then\n"
                "    :\n"
                "  else\n"
                "    printf 'pulp-pr-safe-ephemeral-200-test\\ttrue\\tonline\\tpulp-shutdown-fenced\\n'\n"
                "  fi\n"
                "}\n"
                f"ORG='Generous-Corp'\nREPO='Generous-Corp/pulp'\n"
                f"PAT_FILE='{tmp}/repo-token'\nORG_PAT_FILE='{tmp}/org-token'\n"
                "GITHUB_AUTH_MODE='token-file'\n"
                f"FIREWALL_DIR='{tmp}'\n"
                + helper
                + "\ndeferred_cleanup 200 pulp-pr-safe-ephemeral-200-test orgs/Generous-Corp\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                ["/bin/bash", str(harness)],
                capture_output=True,
                text=True,
                env={**os.environ, "PATH": f"{tmp}:{os.environ['PATH']}"},
                timeout=5,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "deferred-cleanup runner became busy before dispatch fence",
                result.stdout,
            )
            self.assertTrue(firewall.exists())
            self.assertFalse(qm_called.exists())

    def test_deferred_cleanup_rejects_insecure_credential_file(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            tmp = pathlib.Path(raw_tmp)
            token = tmp / "org-token"
            token.write_text("test-token", encoding="utf-8")
            token.chmod(0o644)
            fake_gh = tmp / "gh"
            fake_gh.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            fake_gh.chmod(0o755)
            env = os.environ.copy()
            env["PULP_LINUX_ORG_PAT_FILE"] = str(token)
            env["PULP_LINUX_GH_CLI"] = str(fake_gh)
            result = subprocess.run(
                [
                    "/bin/bash",
                    str(SCRIPT),
                    "--deferred-cleanup",
                    "200",
                    "pulp-pr-safe-ephemeral-200-test",
                    "orgs/Generous-Corp",
                ],
                capture_output=True,
                text=True,
                env=env,
                timeout=5,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("root-owned mode-0600 regular file", result.stdout)

    def test_deferred_cleanup_validates_legacy_fifth_argument(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            token = pathlib.Path(raw_tmp) / "org-token"
            token.write_text("test-token", encoding="utf-8")
            token.chmod(0o600)
            env = os.environ.copy()
            env["PULP_LINUX_ORG_PAT_FILE"] = str(token)
            result = subprocess.run(
                [
                    "/bin/bash",
                    str(SCRIPT),
                    "--deferred-cleanup",
                    "200",
                    "pulp-pr-safe-ephemeral-200-test",
                    "orgs/Generous-Corp",
                    str(token) + "-wrong",
                ],
                capture_output=True,
                text=True,
                env=env,
                timeout=5,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "invalid legacy deferred-cleanup credential path", result.stdout
            )

    @unittest.skipUnless(
        os.geteuid() == 0 and sys.platform.startswith("linux"),
        "requires Linux root ownership for the production credential contract",
    )
    def test_deferred_cleanup_waits_for_busy_runner_then_destroys_clone(self) -> None:
        with tempfile.TemporaryDirectory() as raw_tmp:
            tmp = pathlib.Path(raw_tmp)
            token = tmp / "org-token"
            token.write_text("test-token", encoding="utf-8")
            token.chmod(0o600)
            gh_count = tmp / "gh-count"
            qm_state = tmp / "qm-state"
            destroyed = tmp / "destroyed"
            qm_state.write_text("running", encoding="utf-8")
            fake_gh = tmp / "gh"
            fake_gh.write_text(
                "#!/usr/bin/env bash\n"
                f"count=$(cat '{gh_count}' 2>/dev/null || echo 0)\n"
                "count=$((count + 1))\n"
                f"printf '%s' \"$count\" > '{gh_count}'\n"
                "if [ \"$count\" = 1 ]; then\n"
                "  printf '17\\tpulp-pr-safe-ephemeral-200-test\\ttrue\\tonline\\n'\n"
                "fi\n",
                encoding="utf-8",
            )
            fake_qm = tmp / "qm"
            fake_qm.write_text(
                "#!/usr/bin/env bash\n"
                f"state=$(cat '{qm_state}')\n"
                "case \"$1\" in\n"
                "  status) printf 'status: %s\\n' \"$state\" ;;\n"
                f"  stop) printf stopped > '{qm_state}' ;;\n"
                f"  destroy) : > '{destroyed}' ;;\n"
                "  *) exit 1 ;;\n"
                "esac\n",
                encoding="utf-8",
            )
            fake_sleep = tmp / "sleep"
            fake_sleep.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            for executable in (fake_gh, fake_qm, fake_sleep):
                executable.chmod(0o755)
            env = os.environ.copy()
            env.update(
                {
                    "PATH": f"{tmp}:{env['PATH']}",
                    "PULP_LINUX_GH_CLI": str(fake_gh),
                    "PULP_LINUX_ORG_PAT_FILE": str(token),
                    "PULP_LINUX_FIREWALL_DIR": str(tmp),
                }
            )
            result = subprocess.run(
                [
                    "/bin/bash",
                    str(SCRIPT),
                    "--deferred-cleanup",
                    "200",
                    "pulp-pr-safe-ephemeral-200-test",
                    "orgs/Generous-Corp",
                ],
                capture_output=True,
                text=True,
                env=env,
                timeout=5,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(gh_count.read_text(encoding="utf-8"), "3")
            self.assertTrue(destroyed.exists())

    def test_controller_dependencies_and_org_credential_are_fail_closed(self) -> None:
        self.assertIn('command -v "$GH_CLI"', self.script)
        self.assertLess(
            self.script.index('command -v "$GH_CLI"'),
            self.script.index("# ── admission"),
        )
        self.assertIn("gh-org-runner-pat", self.script)
        self.assertIn("credential_file_secure", self.script)
        self.assertIn("stat -c '%u:%a'", self.script)
        self.assertIn('[ ! -L "$path" ]', self.script)
        self.assertIn('[ "$metadata" = "0:600" ]', self.script)
        self.assertIn(
            '$scope runner credential must be a root-owned mode-0600 regular file',
            self.script,
        )
        self.assertNotIn('Authorization: Bearer $PAT', self.script)
        self.assertIn('GH_TOKEN="$PAT" "$GH_CLI" api "$@"', self.script)
        self.assertIn('RT="$(github_api --method POST', self.script)

    def test_registration_token_is_not_exposed_in_process_arguments(self) -> None:
        registration = self.script[
            self.script.index('log "minting registration token"') : self.script.index(
                "# ── run exactly one job"
            )
        ]
        self.assertIn("printf '%s\\n' \"$RT\" | ssh", registration)
        self.assertIn("IFS= read -r ACTIONS_RUNNER_INPUT_TOKEN", registration)
        self.assertIn("export ACTIONS_RUNNER_INPUT_TOKEN", registration)
        self.assertNotIn("--token", registration)
        self.assertNotIn("${RT}", registration)

    def test_runner_transport_failure_restarts_one_job_service(self) -> None:
        run_block = self.script.split('log "waiting for one job', 1)[1]
        self.assertIn("if ! ssh -o BatchMode=yes", run_block)
        self.assertIn("runner transport failed before one-job completion", run_block)
        self.assertLess(
            run_block.index("runner transport failed before one-job completion"),
            run_block.index('log "job finished on $VMID"'),
        )
        self.assertIn("Restart=on-failure", self.release_control_service)

    def test_automatic_pool_can_use_the_root_owned_github_app_helper(self) -> None:
        self.assertIn(
            'GITHUB_AUTH_MODE="${PULP_LINUX_GITHUB_AUTH_MODE:-token-file}"',
            self.script,
        )
        helper = self.script[
            self.script.index("app_helper_secure() {") : self.script.index(
                "deferred_cleanup() {"
            )
        ]
        self.assertIn('[ "$path" = /usr/local/bin/ghapp ]', helper)
        self.assertIn('[ ! -L "$path" ]', helper)
        self.assertIn("stat -c '%u:%a'", helper)
        self.assertIn("(mode_value & 8#022) == 0", helper)
        self.assertIn(
            'GitHub App helper authentication is restricted to organization runners',
            helper,
        )
        self.assertIn(
            'env -u GH_TOKEN -u GITHUB_TOKEN -u GH_ENTERPRISE_TOKEN', helper
        )
        self.assertGreaterEqual(helper.count("HOME=/root"), 2)
        self.assertNotIn('PAT="$(cat "$credential_file")"', helper.split("app-helper)", 1)[1])

    def test_automatic_pool_requires_private_network_isolation(self) -> None:
        self.assertIn(
            'automatic Linux runners require the Proxmox firewall', self.script
        )
        self.assertIn(
            'ISOLATED_BRIDGE_PREFIX="${PULP_LINUX_ISOLATED_BRIDGE_PREFIX:-vmbr-ci}"',
            self.script,
        )
        self.assertIn('NETWORK_BRIDGE="${ISOLATED_BRIDGE_PREFIX}${VMID}"', self.script)
        self.assertIn("NETWORK_BRIDGE=vmbr0", self.script)
        self.assertIn("LEGACY_GUEST_IPV4_PREFIX=192.168.86", self.script)
        self.assertIn("LEGACY_GUEST_IPV4_GATEWAY=192.168.86.1", self.script)
        self.assertIn("AUTOMATIC_GUEST_DNS_SERVER=1.1.1.1", self.script)
        self.assertIn("GUEST_IPV4_PREFIX_LENGTH=30", self.script)
        self.assertIn("GUEST_IPV4_PREFIX_LENGTH=24", self.script)
        self.assertIn('GUEST_DNS_SERVER="$AUTOMATIC_GUEST_DNS_SERVER"', self.script)
        self.assertIn('GUEST_DNS_SERVER="$LEGACY_GUEST_IPV4_GATEWAY"', self.script)
        self.assertIn(
            '--ipconfig0 "ip=${GUEST_IP}/${GUEST_IPV4_PREFIX_LENGTH},gw=${GUEST_IPV4_GATEWAY}"',
            self.script,
        )
        self.assertIn('--nameserver "$GUEST_DNS_SERVER"', self.script)
        self.assertLess(
            self.script.index('if [ "$AUTOMATIC_NETWORK_ISOLATION" = 1 ]; then\n    NETWORK_BRIDGE='),
            self.script.index('NETWORK_BRIDGE=vmbr0'),
        )
        self.assertIn(
            'automatic runner requires dedicated isolated bridge ${NETWORK_BRIDGE}',
            self.script,
        )
        self.assertIn(
            'isolated bridge ${NETWORK_BRIDGE} already has an attached port',
            self.script,
        )
        self.assertIn(
            'isolated bridge ${NETWORK_BRIDGE} must own only ${GUEST_IPV4_GATEWAY}/30',
            self.script,
        )
        self.assertIn('sysctl -n net.ipv4.ip_forward', self.script)
        self.assertIn('automatic runner requires IPv4 forwarding', self.script)
        self.assertIn('nat_rules="$(iptables-save -t nat)"', self.script)
        self.assertIn(
            'automatic runner requires exactly one source-scoped NAT rule for ${NETWORK_BRIDGE}',
            self.script,
        )
        self.assertIn('[ "$isolated_attachment" = 1 ]', self.script)
        self.assertIn('"fwpr${VMID}p0"', self.script)
        self.assertIn('NET0="${NET0},firewall=1"', self.script)
        self.assertIn("ipfilter: 1", self.script)
        self.assertIn("layer2_protocols: ARP,IPv4", self.script)
        self.assertIn("policy_in: DROP", self.script)
        self.assertNotIn("policy_in: ACCEPT", self.script)
        self.assertIn('ip -4 route get "$GUEST_IP"', self.script)
        self.assertIn(
            "IN ACCEPT -source ${CONTROLLER_IPV4} -p tcp -dport 22",
            self.script,
        )
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
        self.assertIn('-A tap${VMID}i0-IN -j DROP', self.script)
        self.assertIn('-s ${CONTROLLER_IPV4}/32', self.script)
        self.assertIn('--dport 22', self.script)
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
            "OUT ACCEPT -dest ${GUEST_DNS_SERVER} -p udp -dport 53",
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
        self.assertIn('rm -f -- "$firewall_file"', self.script)


if __name__ == "__main__":
    unittest.main()
