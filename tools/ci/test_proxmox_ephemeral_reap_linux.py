#!/usr/bin/env python3
"""Safety contract for recovery of orphaned Mac Pro JIT clones."""

from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
REAPER = ROOT / "tools" / "ci" / "proxmox-ephemeral-reap-linux.sh"
SUPERVISOR = ROOT / "tools" / "ci" / "proxmox-ephemeral-runner-linux.sh"
SERVICE = ROOT / "tools" / "ci" / "pulp-ephemeral-reap.service"
TIMER = ROOT / "tools" / "ci" / "pulp-ephemeral-reap.timer"


class ProxmoxEphemeralReapTests(unittest.TestCase):
    def test_shell_is_syntactically_valid(self) -> None:
        result = subprocess.run(
            ["/bin/bash", "-n", str(REAPER)], capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_timer_executes_fail_closed_reaper_with_github_app_auth(self) -> None:
        reaper = REAPER.read_text(encoding="utf-8")
        service = SERVICE.read_text(encoding="utf-8")
        timer = TIMER.read_text(encoding="utf-8")
        self.assertIn("/usr/local/bin/ghapp", reaper)
        self.assertNotIn("gh-runner-pat", reaper)
        self.assertIn("pulp-ephemeral-reap.sh --yes", service)
        self.assertIn("OnUnitActiveSec=15min", timer)
        self.assertIn("Persistent=true", timer)

    def test_reaper_requires_exact_idle_unused_jit_and_generation_proofs(self) -> None:
        reaper = REAPER.read_text(encoding="utf-8")
        for marker in (
            "listener_count",
            "worker_count",
            "configurer_count",
            "jitconfig",
            "work_entries",
            "pulp-shutdown-fenced",
            "config_digest",
            "clone generation, ownership, or keep disposition changed before mutation",
            "supervisor lease is active or ambiguous",
            "guest identity does not match the host generation",
            "legacy guest identity is not bound to the VMID",
        ):
            self.assertIn(marker, reaper)

    def test_supervisor_publishes_and_owns_exact_generation_lease(self) -> None:
        supervisor = SUPERVISOR.read_text(encoding="utf-8")
        self.assertIn("RUNNER_LEASE_DIR=/run/pulp-ephemeral-runner", supervisor)
        self.assertIn("RUNNER_KEEP_DIR=/var/lib/pulp/ephemeral-runner-keep", supervisor)
        self.assertIn("pulp-runner-scope=${REGISTRATION_API}", supervisor)
        self.assertIn("pid=%s\\nrunner=%s\\n", supervisor)
        self.assertIn("grep -Fxq \"pid=$$\"", supervisor)
        self.assertIn("grep -Fxq \"runner=${RUNNER_NAME}\"", supervisor)
        self.assertIn("durable keep marker", supervisor)
        self.assertIn("trap 'cleanup; remove_runner_lease' EXIT", supervisor)
        keep_publish = 'mv -f -- "$KEEP_TMP" "${RUNNER_KEEP_DIR}/${VMID}.keep"'
        recovery_provenance = (
            '--description "pulp-runner-generation=${RUNNER_NAME};'
            'pulp-runner-scope=${REGISTRATION_API}"'
        )
        self.assertLess(supervisor.index(keep_publish), supervisor.index(recovery_provenance))

    def _run_reaper(
        self,
        *,
        busy: bool = False,
        work_entries: int = 0,
        execute: bool = False,
        listener_count: int = 1,
        vm_status: str = "running",
        host_generation: str = "",
        host_scope: str = "",
        guest_identity: str = "pulp-auto-ephemeral-200",
        github_present: bool = True,
    ) -> tuple[subprocess.CompletedProcess[str], str]:
        with tempfile.TemporaryDirectory() as tmp_name:
            tmp = pathlib.Path(tmp_name)
            vm_configs = tmp / "qemu"
            leases = tmp / "leases"
            vm_configs.mkdir()
            leases.mkdir()
            description = (
                "description: "
                f"pulp-runner-generation={host_generation}"
                f"{';pulp-runner-scope=' + host_scope if host_scope else ''}\n"
                if host_generation
                else ""
            )
            (vm_configs / "200.conf").write_text(
                f"name: pulp-ci-ephemeral-200\n{description}"
            )
            os.utime(vm_configs / "200.conf", (1, 1))
            operations = tmp / "operations"

            qm = tmp / "qm"
            qm.write_text(
                textwrap.dedent(
                    f"""\
                    #!/bin/sh
                    echo "$*" >> {operations}
                    case "$1:$2" in
                      status:200) echo 'status: {vm_status}' ;;
                      config:200) printf 'name: pulp-ci-ephemeral-200\\n{description}' ;;
                      guest:cmd) echo '{{"ip-address" : "192.168.86.251"}}' ;;
                    esac
                    """
                )
            )
            ssh = tmp / "ssh"
            ssh.write_text(
                textwrap.dedent(
                    f"""\
                    #!/bin/sh
                    cat <<'EOF'
                    identity={guest_identity}
                    listener_count={listener_count}
                    worker_count=0
                    configurer_count=0
                    jitconfig={'true' if listener_count == 1 else 'false'}
                    work_entries={work_entries}
                    EOF
                    """
                )
            )
            ghapp = tmp / "ghapp"
            ghapp.write_text(
                textwrap.dedent(
                    f"""\
                    #!/bin/sh
                    echo "$*" >> {operations}
                    case "$*" in
                      *'orgs/Generous-Corp/actions/runners?per_page=100'*) {'printf "17\\tpulp-auto-ephemeral-200\\tonline\\t' + ('true' if busy else 'false') + '\\n"' if github_present else ':'} ;;
                      *'repos/Generous-Corp/pulp/actions/runners?per_page=100'*) : ;;
                      *) exit 2 ;;
                    esac
                    """
                )
            )
            for executable in (qm, ssh, ghapp):
                executable.chmod(0o755)

            env = os.environ.copy()
            env.update(
                {
                    "PULP_REAPER_CLONE_BASE": "200",
                    "PULP_REAPER_CLONE_MAX": "200",
                    "PULP_REAPER_MIN_STALE_SECONDS": "1",
                    "PULP_REAPER_VM_CONFIG_DIR": str(vm_configs),
                    "PULP_REAPER_LEASE_DIR": str(leases),
                    "PULP_REAPER_QM": str(qm),
                    "PULP_REAPER_SSH": str(ssh),
                    "PULP_REAPER_GH_CLI": str(ghapp),
                    "PULP_REAPER_TEST_MODE": "1",
                }
            )
            command = ["/bin/bash", str(REAPER)]
            if execute:
                command.append("--yes")
            result = subprocess.run(command, capture_output=True, text=True, env=env)
            operation_text = operations.read_text() if operations.exists() else ""
            return result, operation_text

    def test_report_only_identifies_exact_stale_unused_runner_without_mutation(self) -> None:
        result, operations = self._run_reaper(busy=False, work_entries=0, execute=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("WOULD REAP 200", result.stdout)
        self.assertNotIn("stop 200", operations)
        self.assertNotIn("destroy 200", operations)
        self.assertNotIn("--method PUT", operations)

    def test_execute_mode_refuses_busy_or_nonempty_runner_before_fencing(self) -> None:
        for busy, work_entries in ((True, 0), (False, 1)):
            with self.subTest(busy=busy, work_entries=work_entries):
                result, operations = self._run_reaper(
                    busy=busy, work_entries=work_entries, execute=True
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("SKIP 200", result.stdout)
                self.assertNotIn("stop 200", operations)
                self.assertNotIn("destroy 200", operations)
                self.assertNotIn("--method PUT", operations)

    def test_execute_mode_preserves_pre_upgrade_clone_without_recovery_scope(self) -> None:
        result, operations = self._run_reaper(
            busy=False,
            work_entries=0,
            execute=True,
            host_generation="pulp-auto-ephemeral-200",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("legacy clone lacks an explicit automatic-recovery scope", result.stdout)
        self.assertNotIn("stop 200", operations)
        self.assertNotIn("destroy 200", operations)
        self.assertNotIn("--method PUT", operations)

    def test_report_recovers_running_post_job_generation_without_registration(self) -> None:
        generation = "pulp-ci-ephemeral-200-generation"
        result, operations = self._run_reaper(
            listener_count=0,
            host_generation=generation,
            guest_identity=generation,
            github_present=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("running-post-job", result.stdout)
        self.assertNotIn("stop 200", operations)

    def test_running_post_job_rejects_guest_identity_mismatch(self) -> None:
        result, operations = self._run_reaper(
            listener_count=0,
            host_generation="pulp-ci-ephemeral-200-generation-a",
            guest_identity="pulp-ci-ephemeral-200-generation-b",
            github_present=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("post-job guest identity does not match", result.stdout)
        self.assertNotIn("stop 200", operations)
        self.assertNotIn("destroy 200", operations)

    def test_keep_disposition_is_rechecked_under_vmid_lock(self) -> None:
        reaper = REAPER.read_text(encoding="utf-8")
        locked = reaper.split('exec 9>"$VMID_LOCK"', 1)[1]
        self.assertLess(
            locked.index('durable_keep_matches "$id" "$host_generation"'),
            locked.index('"$QM" stop "$id"'),
        )
        self.assertIn('[ "$locked_keep_status" -eq 1 ]', locked)

    def test_report_recovers_stopped_unregistered_generation_without_guest_probe(self) -> None:
        result, operations = self._run_reaper(
            vm_status="stopped",
            host_generation="pulp-ci-ephemeral-200-generation",
            github_present=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("stopped-post-job", result.stdout)
        self.assertNotIn("ci@", operations)


if __name__ == "__main__":
    unittest.main()
