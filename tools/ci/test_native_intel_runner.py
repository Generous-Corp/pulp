#!/usr/bin/env python3
"""Contract tests for the advisory physical Intel runner."""

from __future__ import annotations

import json
import pathlib
import plistlib
import re
import subprocess
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "ci" / "native-intel-runner.sh"
WORKER = ROOT / "tools" / "ci" / "native-intel-runner-worker.sh"
PLIST = ROOT / "tools" / "launchd" / "pulp-native-intel-runner.plist.template"
WORKFLOW = ROOT / ".github" / "workflows" / "nightly-intel.yml"
TOPOLOGY = ROOT / "tools" / "scripts" / "runner_topology.json"
LABELS = [
    "self-hosted",
    "macOS",
    "X64",
    "pulp-intel-native",
    "pulp-host-macmini",
]


class NativeIntelRunnerContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.script = SCRIPT.read_text(encoding="utf-8")
        cls.worker = WORKER.read_text(encoding="utf-8")
        cls.plist = plistlib.loads(PLIST.read_bytes())
        cls.workflow = WORKFLOW.read_text(encoding="utf-8")
        cls.topology = json.loads(TOPOLOGY.read_text(encoding="utf-8"))
        cls.lane = next(
            lane
            for lane in cls.topology["lanes"]
            if lane["variable"] == "PULP_NATIVE_INTEL_RUNS_ON_JSON"
        )

    def test_shell_is_syntactically_valid(self) -> None:
        for script in (SCRIPT, WORKER):
            with self.subTest(script=script):
                result = subprocess.run(
                    ["/bin/bash", "-n", str(script)], capture_output=True, text=True
                )
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_lane_is_advisory_ephemeral_and_exact(self) -> None:
        self.assertEqual(self.lane["expect"], LABELS)
        self.assertEqual(self.lane["severity"], "advisory")
        self.assertEqual(self.lane["provisioning"], "ephemeral")
        self.assertEqual(self.lane["supervisor"], "native-intel-launchd")
        self.assertEqual(self.lane["unset_fallback"], "macos-15-intel")
        self.assertEqual(self.lane["hosts"], ["macmini"])

    def test_workflow_has_opt_in_selector_and_hosted_fallback(self) -> None:
        runs_on = (
            "fromJSON((github.event_name == 'workflow_dispatch' && "
            "inputs.use_physical_intel && "
            "'[\"self-hosted\",\"macOS\",\"X64\",\"pulp-intel-native\","
            "\"pulp-host-macmini\"]')"
        )
        self.assertIn(runs_on, self.workflow)
        self.assertIn("use_physical_intel:", self.workflow)
        self.assertNotIn("native_intel_runner_selector_json", self.workflow)
        self.assertIn("command -v ccache", self.workflow)
        self.assertIn("command -v ninja", self.workflow)
        self.assertIn("command -v brew", self.workflow)

    def test_launch_agent_restores_after_login(self) -> None:
        self.assertTrue(self.plist["RunAtLoad"])
        self.assertTrue(self.plist["KeepAlive"])
        self.assertEqual(
            self.plist["ProgramArguments"],
            ["/bin/bash", "$PULP_REPO/tools/ci/native-intel-runner.sh", "--loop"],
        )
        self.assertEqual(
            self.plist["EnvironmentVariables"]["PULP_NATIVE_INTEL_LABELS"],
            ",".join(LABELS),
        )
        self.assertEqual(
            self.plist["EnvironmentVariables"][
                "PULP_NATIVE_INTEL_RUNNER_GROUP_ID"
            ],
            "$RUNNER_GROUP_ID",
        )

    def test_supervisor_is_single_job_jit_with_a_unique_name(self) -> None:
        self.assertIn("actions/runners/generate-jitconfig", self.script)
        self.assertIn("./run.sh --jitconfig", self.worker)
        self.assertRegex(
            self.script,
            re.compile(
                r'CURRENT_RUNNER_NAME="\$NAME_PREFIX-\$\(date -u '
                r'\+%Y%m%d%H%M%S\)-\$\$-\$sequence"'
            ),
        )

    def test_workspace_cleanup_is_bounded_to_the_build_account_runner(self) -> None:
        self.assertIn(
            '"/private/var/tmp/pulp-native-intel-job")',
            self.worker,
        )
        self.assertIn('rm -rf "$JOB_ROOT"', self.worker)
        self.assertIn('/usr/bin/pkill -KILL -u "$uid"', self.worker)
        self.assertIn('/usr/bin/pgrep -u "$uid"', self.worker)
        self.assertIn('stop_build_processes || return 1', self.worker)
        cleanup = self.worker.split('clean_job_root() {', 1)[1].split('cleanup_on_signal()', 1)[0]
        first_stop = cleanup.index('stop_build_processes || return 1')
        crontab_remove = cleanup.index('/usr/bin/crontab -u "$BUILD_USER" -r')
        second_stop = cleanup.index('stop_build_processes || return 1', first_stop + 1)
        absent_check = cleanup.index('assert_no_crontab || return 1')
        self.assertLess(first_stop, crontab_remove)
        self.assertLess(crontab_remove, second_stop)
        self.assertLess(second_stop, absent_check)
        self.assertIn('clean_at_jobs || return 1', cleanup)
        self.assertIn('assert_at_scheduler_disabled || return 1', cleanup)
        self.assertIn(
            'for root in /private/tmp /private/var/tmp /private/var/folders',
            self.worker,
        )
        self.assertIn('/Users/Shared /Library/Caches', self.worker)
        self.assertIn('/Users/*/Public/"Drop Box"', self.worker)
        self.assertIn('find "$root" -xdev -user "$uid" -depth -delete', self.worker)
        self.assertIn('This fixed service uid owns no durable host state', self.worker)
        self.assertNotIn("rm -rf $HOME", self.worker)

    def test_job_runs_as_separate_non_admin_account_without_github_auth(self) -> None:
        self.assertIn('BUILD_USER="pulp-ci"', self.script)
        self.assertIn('BUILD_UID="499"', self.script)
        self.assertIn('BUILD_USER="pulp-ci"', self.worker)
        self.assertIn('BUILD_UID="499"', self.worker)
        self.assertNotIn("PULP_NATIVE_INTEL_BUILD_USER", self.script)
        self.assertNotIn("PULP_NATIVE_INTEL_BUILD_USER", self.worker)
        self.assertIn('/usr/bin/sudo -n "$WORKER" --run', self.script)
        self.assertIn('grep -qx admin', self.worker)
        self.assertNotIn("GH_CLI", self.worker)
        self.assertNotIn("auth status", self.worker)
        self.assertNotIn("generate-jitconfig", self.worker)
        self.assertIn("/usr/bin/env -i", self.worker)
        self.assertIn('USER="$BUILD_USER"', self.worker)
        self.assertIn('HOME="$JOB_HOME"', self.worker)
        self.assertIn('TMPDIR="$JOB_TMP/"', self.worker)
        self.assertIn('chmod 0700 "$JOB_ROOT" "$JOB_HOME" "$JOB_TMP"', self.worker)

    def test_every_job_uses_a_fresh_copy_of_root_owned_runner_and_tools(self) -> None:
        self.assertIn('RUNNER_GOLDEN="$TRUST_ROOT/actions-runner-mini"', self.worker)
        self.assertIn('TOOLS_DIR="$TRUST_ROOT/bin"', self.worker)
        self.assertIn('assert_immutable_tree "$TRUST_ROOT" "$TRUST_ROOT"', self.worker)
        self.assertIn('assert_contained_symlinks "$RUNNER_GOLDEN" "$RUNNER_GOLDEN"', self.worker)
        self.assertIn('find "$path" ! -user root', self.worker)
        self.assertIn('assert_contained_symlinks "$path" "$symlink_root"', self.worker)
        self.assertIn('"$trust_root"/*)', self.worker)
        self.assertIn('-perm -0020 -o -perm -0002', self.worker)
        self.assertIn('find "$path" -acl -print -quit', self.worker)
        self.assertIn('/usr/bin/ditto "$RUNNER_GOLDEN" "$RUNNER_DIR"', self.worker)
        self.assertIn('CCACHE_READONLY=1', self.worker)
        self.assertIn('CCACHE_NODEPEND=1', self.worker)
        self.assertNotIn('CCACHE_DEPEND=', self.worker)
        self.assertLess(
            self.worker.index("prepare_fresh_runner || return 1"),
            self.worker.index('./run.sh --jitconfig "$jit"'),
        )
        self.assertIn('probe_build_environment ||', self.worker)
        self.assertIn('"$TOOLS_DIR/ccache" --show-config', self.worker)
        self.assertIn('/usr/bin/find "$1" -mindepth 1 -print -quit', self.worker)
        self.assertIn('/usr/bin/find "$1" -type f -exec', self.worker)
        self.assertIn('[ -r "$entry" ] || exit 1', self.worker)

    def test_controller_requires_root_owned_immutable_worker(self) -> None:
        self.assertIn('[ "$worker_owner" = root ]', self.script)
        self.assertIn('protected_from_build_user "$WORKER"', self.script)
        self.assertIn('WORKER="/usr/local/libexec/pulp-native-intel-worker"', self.script)
        self.assertNotIn("PULP_NATIVE_INTEL_WORKER:-", self.script)
        self.assertIn('WORKER_INSTALL="/usr/local/libexec/pulp-native-intel-worker"', self.worker)
        self.assertIn('[ "$actual" = "$WORKER_INSTALL" ]', self.worker)
        self.assertIn('assert_not_writable_by_build_user "$path"', self.worker)
        self.assertIn('must not be group/world writable', self.script)
        self.assertIn('controller and build account must be different users', self.script)
        self.assertIn('protected_from_build_user "$controller_path"', self.script)
        self.assertIn("protected controller path is group/world writable", self.script)
        self.assertIn("protected controller path has a write-granting ACL", self.script)
        self.assertIn("write|append|delete", self.script)

    def test_controller_forwards_stop_to_active_root_worker(self) -> None:
        self.assertIn('ACTIVE_WORKER_PID=""', self.script)
        self.assertIn('ACTIVE_WORKER_PID=$!', self.script)
        self.assertIn('if [ "$STOP_REQUESTED" -ne 0 ]; then', self.script)
        self.assertIn('kill -TERM "$ACTIVE_WORKER_PID"', self.script)
        self.assertIn('wait_for_active_worker || rc=$?', self.script)
        self.assertIn('wait "$ACTIVE_WORKER_PID" || rc=$?', self.script)
        self.assertIn('kill -0 "$ACTIVE_WORKER_PID" 2>/dev/null || break', self.script)
        self.assertLess(
            self.script.index('wait_for_active_worker || rc=$?'),
            self.script.index('ACTIVE_WORKER_PID=""', self.script.index('run_one()')),
        )
        job_path = 'PATH="$TOOLS_DIR:/usr/bin:/bin:/usr/sbin:/sbin"'
        self.assertIn(job_path, self.worker)
        self.assertNotIn('PATH="$TOOLS_DIR:/usr/local/bin:', self.worker)
        self.assertLess(
            self.script.index('kill -TERM "$ACTIVE_WORKER_PID"'),
            self.script.index('remove_idle_registration\n}'),
        )

    def test_clean_revalidates_fixed_service_identity(self) -> None:
        clean_case = self.worker.split('--clean)', 1)[1].split(';;', 1)[0]
        self.assertIn('assert_root_boundary', clean_case)
        self.assertIn('assert_service_identity', clean_case)
        self.assertIn('assert_worker_install', clean_case)

    def test_delayed_job_scheduler_cannot_persist_across_cycles(self) -> None:
        self.assertIn('assert_at_scheduler_disabled || failed=1', self.worker)
        self.assertIn('com.apple.atrun.plist', self.worker)
        self.assertIn('/bin/launchctl print system/com.apple.atrun', self.worker)
        self.assertIn('/usr/bin/atq', self.worker)
        self.assertIn('/usr/bin/atrm "$job_id"', self.worker)
        self.assertIn('$1 ~ /^[0-9]+$/', self.worker)
        self.assertIn('could not verify queued at jobs are absent', self.worker)

    def test_signal_cleanup_is_terminal_and_cannot_start_another_job(self) -> None:
        self.assertIn("trap 'cleanup_on_signal 130' INT", self.worker)
        self.assertIn("trap 'cleanup_on_signal 143' TERM", self.worker)
        self.assertIn("trap - EXIT INT TERM", self.worker)
        self.assertIn('clean_job_root || true', self.worker)
        self.assertIn('exit "$rc"', self.worker)
        self.assertIn('ACTIVE_JOB_PID=$!', self.worker)
        self.assertIn('kill -TERM "$ACTIVE_JOB_PID"', self.worker)
        self.assertIn('wait "$ACTIVE_JOB_PID"', self.worker)
        self.assertIn('while kill -0 "$ACTIVE_JOB_PID"', self.worker)

    def test_preflight_requires_real_intel_xcode_and_build_tools(self) -> None:
        for needle in (
            '"$(uname -m)" = x86_64',
            '"$GH_CLI" auth status',
            'fail "$GH_CLI is not authenticated"',
            "a dedicated workflow-restricted group is required",
            "verify_native_intel_runner_group.py",
        ):
            with self.subTest(needle=needle):
                self.assertIn(needle, self.script)
        for needle in (
            '"$XCODE_DEVELOPER_DIR/usr/bin/xcodebuild"',
            '[ -x "$TOOLS_DIR/$tool" ]',
            '/usr/bin/codesign --verify --deep --strict "$XCODE_BUNDLE"',
            '/usr/sbin/spctl --assess --type execute "$XCODE_BUNDLE"',
            'fail "the Xcode license has not been accepted"',
            'fail "the selected Xcode toolchain cannot run clang"',
        ):
            with self.subTest(worker_needle=needle):
                self.assertIn(needle, self.worker)

    def test_preflight_failure_aborts_before_runner_registration(self) -> None:
        self.assertIn("preflight || exit 1", self.script)
        self.assertEqual(self.script.count("preflight || exit 1"), 1)
        self.assertIn("if ! preflight; then", self.script)

    def test_integrity_failure_holds_lane_offline_instead_of_retrying(self) -> None:
        self.assertIn('TERMINAL_CONFIG_RC=78', self.script)
        self.assertIn('TERMINAL_CONFIG_RC=78', self.worker)
        self.assertIn('check || return "$TERMINAL_CONFIG_RC"', self.worker)
        self.assertIn('if [ "$rc" -eq "$TERMINAL_CONFIG_RC" ]; then', self.script)
        self.assertIn('holding lane offline', self.script)
        self.assertIn('if ! preflight; then', self.script)
        self.assertIn('hold_lane_offline "startup preflight failed"', self.script)
        self.assertIn(
            'hold_lane_offline "worker integrity/configuration validation failed"',
            self.script,
        )
        self.assertIn('while [ "$STOP_REQUESTED" -eq 0 ]; do', self.script)
        self.assertIn('sleep 3600', self.script)
        self.assertIn('return "$TERMINAL_CONFIG_RC"', self.script)
        self.assertIn('exit $?', self.script)
        self.assertIn(
            '/usr/bin/sudo -n "$WORKER" --clean || return "$TERMINAL_CONFIG_RC"',
            self.script,
        )
        self.assertIn('rc="$TERMINAL_CONFIG_RC"', self.script)

    def test_workflow_uses_portable_timeout_and_step_runner_name(self) -> None:
        self.assertIn("tools/ci/run_with_timeout.py 4500", self.workflow)
        self.assertNotRegex(self.workflow, re.compile(r"(?m)^\s*timeout 75m "))
        self.assertNotIn("NATIVE_INTEL_RUNNER_NAME", self.workflow)
        self.assertIn("${RUNNER_NAME}", self.workflow)


if __name__ == "__main__":
    unittest.main()
