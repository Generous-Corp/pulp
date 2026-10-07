#!/usr/bin/env python3
"""Safety contract for recovery of orphaned Mac Pro JIT clones."""

from __future__ import annotations

import json
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
            ["/bin/bash", "-n", str(REAPER)], capture_output=True, text=True, encoding="utf-8"
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
            "deregister_fence",
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
        github_status: str = "online",
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
                f"name: pulp-ci-ephemeral-200\n{description}", encoding="utf-8"
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
                ), encoding="utf-8"
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
                ), encoding="utf-8"
            )
            ghapp = tmp / "ghapp"
            ghapp.write_text(
                textwrap.dedent(
                    f"""\
                    #!/bin/sh
                    echo "$*" >> {operations}
                    case "$*" in
                      *'orgs/Generous-Corp/actions/runners?per_page=100'*) {'printf "17\\tpulp-auto-ephemeral-200\\t' + github_status + '\\t' + ('true' if busy else 'false') + '\\n"' if github_present else ':'} ;;
                      *'repos/Generous-Corp/pulp/actions/runners?per_page=100'*) : ;;
                      *) exit 2 ;;
                    esac
                    """
                ), encoding="utf-8"
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
            result = subprocess.run(command, capture_output=True, text=True, env=env, encoding="utf-8")
            operation_text = operations.read_text(encoding="utf-8") if operations.exists() else ""
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

    def test_report_recovers_stopped_generation_with_exact_idle_offline_registration(self) -> None:
        result, operations = self._run_reaper(
            vm_status="stopped",
            host_generation="pulp-auto-ephemeral-200",
            busy=False,
            github_present=True,
            github_status="offline",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("stopped-idle-registration", result.stdout)
        self.assertNotIn("ci@", operations)

    def test_stopped_generation_preserves_busy_registration(self) -> None:
        result, operations = self._run_reaper(
            vm_status="stopped",
            host_generation="pulp-auto-ephemeral-200",
            busy=True,
            github_present=True,
            github_status="offline",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("registration is not idle and offline", result.stdout)
        self.assertNotIn("destroy 200", operations)


FAKE_GH_FENCE = """#!/bin/bash
# Stateful GitHub API fake for the JIT deregistration fence.
echo "$*" >> "$FENCE_OPS"
args="$*"
name="$FENCE_RUNNER"
case "$args" in
  *"--method DELETE"*"/actions/runners/17"*)
    case "$FENCE_DELETE" in
      ok) : > "$FENCE_STATE/deleted"; exit 0 ;;
      busy) echo "gh: Bad request - Runner $name is still running a job (HTTP 422)" >&2; exit 1 ;;
      *) echo "gh: Server Error (HTTP 500)" >&2; exit 1 ;;
    esac ;;
  *"orgs/Generous-Corp/actions/runners?per_page=100"*)
    [ -e "$FENCE_STATE/deleted" ] || printf '17\t%s\tonline\tfalse\n' "$name" ;;
  *"repos/Generous-Corp/pulp/actions/runners?per_page=100"*) : ;;
  *"/actions/runners/17"*)
    if [ -e "$FENCE_STATE/deleted" ]; then
      case "$FENCE_AFTER" in
        404) echo "gh: Not Found (HTTP 404)" >&2; exit 1 ;;
        exists) printf '17\t%s\tfalse\n' "$name" ;;
        *) echo "gh: Server Error (HTTP 502)" >&2; exit 1 ;;
      esac
    else
      printf '17\t%s\t%s\n' "$name" "$FENCE_BUSY"
    fi ;;
  *"actions/runs?status=in_progress"*) echo 901 ;;
  *"actions/runs/901/jobs"*) [ -z "$FENCE_JOB_RUNNER" ] || echo "$FENCE_JOB_RUNNER" ;;
  *) exit 2 ;;
esac
"""


class JitDeregistrationFenceTests(unittest.TestCase):
    """--yes on an idle JIT orphan: read, deregister, verify 404 + no job, destroy."""

    GENERATION = "pulp-ci-ephemeral-200-generation"

    def _run(
        self,
        *,
        busy: str = "false",
        delete: str = "ok",
        after: str = "404",
        job_runner: str = "",
    ) -> tuple[subprocess.CompletedProcess[str], str]:
        with tempfile.TemporaryDirectory() as tmp_name:
            tmp = pathlib.Path(tmp_name)
            vm_configs = tmp / "qemu"
            leases = tmp / "leases"
            state = tmp / "state"
            for directory in (vm_configs, leases, state):
                directory.mkdir()
            description = (
                f"description: pulp-runner-generation={self.GENERATION}"
                ";pulp-runner-scope=orgs/Generous-Corp\n"
            )
            (vm_configs / "200.conf").write_text(f"name: pulp-ci-ephemeral-200\n{description}", encoding="utf-8")
            os.utime(vm_configs / "200.conf", (1, 1))
            operations = tmp / "operations"
            qm = tmp / "qm"
            qm.write_text(
                textwrap.dedent(
                    f"""\
                    #!/bin/sh
                    echo "qm $*" >> {operations}
                    case "$1:$2" in
                      status:200) if [ -e {state}/stopped ]; then echo 'status: stopped'; else echo 'status: running'; fi ;;
                      stop:200) : > {state}/stopped ;;
                      config:200) printf 'name: pulp-ci-ephemeral-200\\n{description}' ;;
                      guest:cmd) echo '{{"ip-address" : "192.168.86.251"}}' ;;
                    esac
                    """
                ), encoding="utf-8"
            )
            ssh = tmp / "ssh"
            ssh.write_text(
                textwrap.dedent(
                    f"""\
                    #!/bin/sh
                    cat <<'EOF'
                    identity={self.GENERATION}
                    listener_count=1
                    worker_count=0
                    configurer_count=0
                    jitconfig=true
                    work_entries=0
                    EOF
                    """
                ), encoding="utf-8"
            )
            ghapp = tmp / "ghapp"
            ghapp.write_text(FAKE_GH_FENCE, encoding="utf-8")
            # macOS has no flock(1); the lock's own semantics are not under test.
            flock = tmp / "flock"
            flock.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            for executable in (qm, ssh, ghapp, flock):
                executable.chmod(0o755)
            env = os.environ.copy()
            env.update(
                {
                    "PULP_REAPER_CLONE_BASE": "200",
                    "PULP_REAPER_CLONE_MAX": "200",
                    "PULP_REAPER_MIN_STALE_SECONDS": "1",
                    "PULP_REAPER_VM_CONFIG_DIR": str(vm_configs),
                    "PULP_REAPER_LEASE_DIR": str(leases),
                    "PULP_REAPER_VMID_LOCK": str(tmp / "vmid.lock"),
                    "PULP_REAPER_FIREWALL_DIR": str(tmp),
                    "PULP_REAPER_QM": str(qm),
                    "PULP_REAPER_SSH": str(ssh),
                    "PULP_REAPER_GH_CLI": str(ghapp),
                    "PULP_REAPER_TEST_MODE": "1",
                    "FENCE_OPS": str(operations),
                    "FENCE_STATE": str(state),
                    "FENCE_RUNNER": self.GENERATION,
                    "FENCE_BUSY": busy,
                    "FENCE_DELETE": delete,
                    "FENCE_AFTER": after,
                    "FENCE_JOB_RUNNER": job_runner,
                    "PATH": f"{tmp}:{os.environ['PATH']}",
                }
            )
            result = subprocess.run(
                ["/bin/bash", str(REAPER), "--yes"],
                capture_output=True, text=True, env=env, timeout=60, encoding="utf-8"
            )
            return result, operations.read_text(encoding="utf-8") if operations.exists() else ""

    def test_idle_jit_orphan_is_reaped_through_all_four_logged_steps(self) -> None:
        result, operations = self._run()
        self.assertEqual(result.returncode, 0, result.stderr)
        for step in ("FENCE 200 1/4", "FENCE 200 2/4", "FENCE 200 3/4", "FENCE 200 4/4"):
            self.assertIn(step, result.stdout)
        self.assertIn("REAP 200", result.stdout)
        self.assertNotIn("labels", operations)
        self.assertLess(operations.index("--method DELETE"), operations.index("qm stop 200"))
        self.assertLess(operations.index("actions/runs?status=in_progress"), operations.index("qm stop 200"))
        self.assertIn("qm destroy 200 --purge", operations)
        self.assertEqual(operations.count("--method DELETE"), 1, operations)

    def test_step_one_busy_runner_is_left_without_deregistration(self) -> None:
        result, operations = self._run(busy="true")
        self.assertIn("JIT fence 1/4", result.stdout)
        self.assertNotIn("--method DELETE", operations)
        self.assertNotIn("qm stop 200", operations)

    def test_step_two_busy_refusal_leaves_the_clone_this_pass(self) -> None:
        result, operations = self._run(delete="busy")
        self.assertIn("JIT fence 2/4: GitHub refused to deregister runner 17 because it is busy", result.stdout)
        self.assertNotIn("qm stop 200", operations)
        self.assertNotIn("qm destroy", operations)

    def test_step_two_other_failure_leaves_the_clone(self) -> None:
        result, operations = self._run(delete="error")
        self.assertIn("JIT fence 2/4: deregistering runner 17 failed", result.stdout)
        self.assertNotIn("qm stop 200", operations)

    def test_step_three_requires_a_404_after_deregistration(self) -> None:
        for after, message in (
            ("exists", "still exists after deregistration"),
            ("502", "failed for a reason other than 404"),
        ):
            with self.subTest(after=after):
                result, operations = self._run(after=after)
                self.assertIn(message, result.stdout)
                self.assertNotIn("qm stop 200", operations)

    def test_step_three_wrong_id_control_an_in_progress_job_naming_the_runner(self) -> None:
        result, operations = self._run(job_runner=self.GENERATION)
        self.assertIn("in-progress job in run 901 names", result.stdout)
        self.assertNotIn("qm stop 200", operations)
        self.assertNotIn("qm destroy", operations)

    def test_an_unrelated_in_progress_job_does_not_block(self) -> None:
        result, operations = self._run(job_runner="some-other-runner")
        self.assertIn("REAP 200", result.stdout)
        self.assertIn("qm destroy 200 --purge", operations)


class GuestProbeIdentityTests(unittest.TestCase):
    """The in-guest probe must name a finished JIT clone's runner, or no
    post-job clone can ever be matched to its host generation and reaped."""

    def _probe_source(self) -> str:
        text = REAPER.read_text(encoding="utf-8")
        marker = "python3 - /home/ci/actions-runner <<'PY'\n"
        start = text.index(marker) + len(marker)
        return text[start:text.index("\nPY\n", start)]

    def _run(self, files: dict[str, str]) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as tmp_name:
            root = pathlib.Path(tmp_name) / "actions-runner"
            for rel, body in files.items():
                path = root / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(body, encoding="utf-8")
            root.mkdir(exist_ok=True)
            # A forged root in the environment must be ignored; argv decides.
            return subprocess.run(
                ["python3", "-c", self._probe_source(), str(root)],
                capture_output=True, text=True,
                env={**os.environ, "TARTCI_PROBE_RUNNER_ROOT": "/nonexistent-forged-root"},
            )

    @staticmethod
    def _identity(result: subprocess.CompletedProcess[str]) -> str:
        return next(line.split("=", 1)[1] for line in result.stdout.splitlines()
                    if line.startswith("identity="))

    def test_live_runner_identity_comes_from_runner_file(self) -> None:
        result = self._run({".runner": json.dumps({"AgentName": "pulp-ci-ephemeral-200-a"})})
        self.assertEqual(self._identity(result), "pulp-ci-ephemeral-200-a")

    def test_finished_jit_clone_is_named_by_its_listener_log(self) -> None:
        log = '[2026-10-06 10:03:29Z INFO Runner] {\n  "AgentName": "pulp-ci-ephemeral-201-86cc",\n}\n'
        result = self._run({"_diag/Runner_20261006-020000-utc.log": log})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self._identity(result), "pulp-ci-ephemeral-201-86cc")

    def test_conflicting_listener_logs_are_not_an_identity(self) -> None:
        result = self._run({
            "_diag/Runner_a.log": '"AgentName": "pulp-ci-ephemeral-201-a"',
            "_diag/Runner_b.log": '"AgentName": "pulp-ci-ephemeral-201-b"',
        })
        self.assertEqual(result.returncode, 2)

    def test_the_runner_root_is_host_argv_not_guest_environment(self) -> None:
        text = REAPER.read_text(encoding="utf-8")
        self.assertIn('"ci@${ip}" python3 - /home/ci/actions-runner <<', text)
        self.assertNotIn("TARTCI_PROBE_RUNNER_ROOT", text)
        self.assertNotIn("os.environ", self._probe_source())

    def test_no_runner_file_and_no_log_is_an_empty_identity(self) -> None:
        self.assertEqual(self._identity(self._run({})), "")


if __name__ == "__main__":
    unittest.main()
