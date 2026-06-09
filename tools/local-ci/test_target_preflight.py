#!/usr/bin/env python3
"""No-network tests for local-ci target preflight helpers."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import subprocess
import tempfile
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("target_preflight.py")


def load_module():
    spec = importlib.util.spec_from_file_location("target_preflight_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class TargetPreflightTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_ssh_helpers_use_injected_runner(self) -> None:
        calls: list[tuple[list[str], dict]] = []

        def fake_run_ssh(command, **kwargs):
            calls.append((command, kwargs))
            return subprocess.CompletedProcess(command, 0, stdout="up\n", stderr="")

        result = self.mod.ssh_probe("ubuntu", 2, run_ssh_subprocess_fn=fake_run_ssh)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(calls[-1][0], ["ssh", "-o", "ConnectTimeout=2", "-o", "BatchMode=yes", "ubuntu", "echo", "up"])
        self.assertEqual(calls[-1][1]["timeout"], 5)
        self.assertTrue(self.mod.ssh_reachable("ubuntu", 5, ssh_probe_fn=lambda _host, _timeout: result))

        def timeout_run_ssh(command, **_kwargs):
            raise subprocess.TimeoutExpired(command, 5)

        timed_out = self.mod.ssh_probe("win2", 5, run_ssh_subprocess_fn=timeout_run_ssh)
        self.assertEqual(timed_out.returncode, 124)
        self.assertIn("timed out", timed_out.stderr.lower())

        reset = subprocess.CompletedProcess(["ssh"], 255, "", "kex_exchange_identification: read: Connection reset by peer\n")
        detail = self.mod.ssh_failure_detail("win2", 5, ssh_probe_fn=lambda _host, _timeout: reset)
        self.assertEqual(detail, "win2 (SSH service reset during handshake; verify OpenSSH server on the target)")

        self.mod.ssh_command_result("ubuntu", "echo ok", timeout=90, run_ssh_subprocess_fn=fake_run_ssh)
        self.assertEqual(calls[-1][0][:4], ["ssh", "-o", "ConnectTimeout=30", "ubuntu"])
        self.assertEqual(calls[-1][1]["timeout"], 90)
        self.assertIn('export PATH="$HOME/.local/bin:$PATH"; echo ok', calls[-1][0][-1])

    def test_utm_status_and_reachability_fallbacks_are_injected(self) -> None:
        def fake_run(command, **_kwargs):
            if command == ["utmctl", "list"]:
                return subprocess.CompletedProcess(command, 0, stdout="Ubuntu stopped\nWindows started\n", stderr="")
            return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

        self.assertEqual(self.mod.utmctl_vm_status("Ubuntu", run_fn=fake_run), "stopped")
        self.assertTrue(self.mod.utmctl_start("Ubuntu", run_fn=fake_run))

        reachable_hosts: list[str] = []

        def reachable(host, _timeout):
            reachable_hosts.append(host)
            return host == "fallback"

        logs: list[str] = []
        resolved = self.mod.ensure_host_reachable(
            "ubuntu",
            {"host": "primary", "fallback_host": "fallback"},
            {},
            ssh_reachable_fn=reachable,
            utmctl_vm_status_fn=lambda _name: None,
            utmctl_start_fn=lambda _name: False,
            time_fn=lambda: 0,
            sleep_fn=lambda _secs: None,
            print_fn=logs.append,
        )
        self.assertEqual(resolved, "fallback")
        self.assertEqual(reachable_hosts, ["primary", "fallback"])
        self.assertTrue(any("trying fallback" in line for line in logs))

        start_calls: list[str] = []
        sleep_calls: list[float] = []
        probe_count = {"count": 0}

        def reachable_after_start(_host, _timeout):
            probe_count["count"] += 1
            return probe_count["count"] > 1

        resolved_after_start = self.mod.ensure_host_reachable(
            "ubuntu",
            {"host": "primary", "utm_fallback": {"vm_name": "Ubuntu", "boot_wait_secs": 0, "ssh_retry_secs": 10}},
            {},
            ssh_reachable_fn=reachable_after_start,
            utmctl_vm_status_fn=lambda _name: "stopped",
            utmctl_start_fn=lambda name: start_calls.append(name) or True,
            time_fn=iter([0, 1]).__next__,
            sleep_fn=sleep_calls.append,
            print_fn=lambda *_args, **_kwargs: None,
        )
        self.assertEqual(resolved_after_start, "primary")
        self.assertEqual(start_calls, ["Ubuntu"])
        self.assertEqual(sleep_calls, [0])

    def test_config_material_source_and_drift(self) -> None:
        shared = self.root / "shared.json"
        worktree = self.root / "worktree.json"
        shared.write_text(
            json.dumps({"targets": {"ubuntu": {"type": "ssh", "host": "shared", "repo_path": "/repo"}}})
        )
        worktree.write_text(
            json.dumps({"targets": {"ubuntu": {"type": "ssh", "host": "worktree", "repo_path": "/repo"}}})
        )

        drift = self.mod.find_material_config_drift(
            ["ubuntu"],
            shared_config_path_fn=lambda: shared,
            worktree_config_path_fn=lambda: worktree,
            config_material_for_targets_fn=self.mod.config_material_for_targets,
        )
        self.assertEqual(len(drift), 1)
        self.assertIn("shared", drift[0])
        self.assertIn("worktree", drift[0])
        self.assertEqual(
            self.mod.config_source_name(shared, environ={}, shared_config_path_fn=lambda: shared),
            "shared-state",
        )
        self.assertEqual(
            self.mod.config_source_name(worktree, environ={"PULP_LOCAL_CI_CONFIG": str(worktree)}, shared_config_path_fn=lambda: shared),
            "env-override",
        )

        worktree.write_text("{broken")
        self.assertEqual(
            self.mod.find_material_config_drift(
                ["ubuntu"],
                shared_config_path_fn=lambda: shared,
                worktree_config_path_fn=lambda: worktree,
                config_material_for_targets_fn=self.mod.config_material_for_targets,
            ),
            [],
        )

    def test_submission_metadata_records_warnings_and_namespace_failover(self) -> None:
        config_path = self.root / "config.json"
        config_path.write_text("{}\n")
        config = {
            "targets": {
                "mac": {"type": "local", "enabled": True},
                "windows": {
                    "type": "ssh",
                    "enabled": True,
                    "host": "win2",
                    "fallback_host": "win",
                    "repo_path": "C:\\Pulp",
                },
            },
            "defaults": {},
        }

        def fallback_state(name, target_cfg, defaults):
            return self.mod.preflight_target_host_state(
                name,
                target_cfg,
                defaults,
                ssh_reachable_fn=lambda host, _timeout: host == "win",
            )

        metadata = self.mod.build_submission_metadata(
            config,
            "feature/topic",
            "a" * 40,
            ["mac", "windows"],
            "normal",
            "full",
            allow_root_mismatch=False,
            allow_unreachable_targets=False,
            root=self.root.resolve(),
            cwd_fn=lambda: self.root,
            git_root_for_fn=lambda _cwd: self.root.resolve(),
            config_path_fn=lambda: config_path,
            config_source_name_fn=lambda _path: "worktree-local",
            preflight_target_host_state_fn=fallback_state,
            find_material_config_drift_fn=lambda _targets: ["windows: drift"],
            normalize_provenance_fn=lambda: {"execution_kind": "direct"},
            environ={},
        )
        self.assertEqual(metadata["target_hosts"]["mac"]["status"], "local")
        self.assertEqual(metadata["target_hosts"]["windows"]["status"], "fallback-up")
        self.assertEqual(metadata["target_hosts"]["windows"]["resolved_host"], "win")
        self.assertEqual(metadata["config_drift"], ["windows: drift"])
        self.assertTrue(any("fallback win is up" in warning for warning in metadata["warnings"]))
        self.assertTrue(any("config drift detected" in warning for warning in metadata["warnings"]))

        unreachable_config = {
            "targets": {"ubuntu": {"type": "ssh", "host": "ubuntu-primary", "repo_path": "/repo"}},
            "defaults": {},
            "github_actions": {"defaults": {"provider": "github-hosted"}},
        }

        def unreachable_state(name, target_cfg, defaults):
            return self.mod.preflight_target_host_state(
                name,
                target_cfg,
                defaults,
                ssh_reachable_fn=lambda _host, _timeout: False,
            )

        with self.assertRaisesRegex(ValueError, "Pass --allow-unreachable-targets"):
            self.mod.build_submission_metadata(
                unreachable_config,
                "feature/topic",
                "b" * 40,
                ["ubuntu"],
                "normal",
                "full",
                allow_root_mismatch=False,
                allow_unreachable_targets=False,
                root=self.root.resolve(),
                cwd_fn=lambda: self.root,
                git_root_for_fn=lambda _cwd: self.root.resolve(),
                config_path_fn=lambda: config_path,
                config_source_name_fn=lambda _path: "worktree-local",
                preflight_target_host_state_fn=unreachable_state,
                find_material_config_drift_fn=lambda _targets: [],
                normalize_provenance_fn=lambda: {"execution_kind": "direct"},
                environ={},
            )

        namespace_config = dict(unreachable_config)
        namespace_config["github_actions"] = {"defaults": {"provider": "namespace"}}
        failed_over = self.mod.build_submission_metadata(
            namespace_config,
            "feature/topic",
            "b" * 40,
            ["ubuntu"],
            "normal",
            "full",
            allow_root_mismatch=False,
            allow_unreachable_targets=False,
            root=self.root.resolve(),
            cwd_fn=lambda: self.root,
            git_root_for_fn=lambda _cwd: self.root.resolve(),
            config_path_fn=lambda: config_path,
            config_source_name_fn=lambda _path: "worktree-local",
            preflight_target_host_state_fn=unreachable_state,
            find_material_config_drift_fn=lambda _targets: [],
            normalize_provenance_fn=lambda: {"execution_kind": "direct"},
            environ={},
        )
        self.assertEqual(failed_over["target_hosts"]["ubuntu"]["status"], "namespace-failover")
        self.assertEqual(failed_over["namespace_failover_targets"], ["ubuntu"])
        self.assertNotIn("error", failed_over["target_hosts"]["ubuntu"])


if __name__ == "__main__":
    unittest.main()
