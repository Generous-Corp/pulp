#!/usr/bin/env python3
from __future__ import annotations

import json
from argparse import Namespace
from pathlib import Path
import subprocess
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).resolve().with_name("desktop_setup_commands_cli.py")


def load_desktop_setup_commands_cli_module():
    return load_module_from_path(MODULE_PATH)


class DesktopSetupCommandsCliTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_desktop_setup_commands_cli_module()
        self.printed: list[str] = []
        self.writes: list[tuple[Path, str]] = []
        self.saved_configs: list[dict] = []
        self.targets = {
            "mac": {
                "adapter": "macos-local",
                "bootstrap": "launchagent",
                "target_type": "local",
                "capability_tier": "full",
            },
            "ubuntu": {
                "adapter": "linux-xvfb",
                "bootstrap": "xvfb-run",
                "target_type": "ssh",
                "capability_tier": "full",
                "host": "ubuntu",
            },
            "windows": {
                "adapter": "windows-session-agent",
                "bootstrap": "scheduled-task",
                "target_type": "ssh",
                "capability_tier": "full",
                "host": "win",
                "repo_path": r"C:\Old\Pulp",
            },
        }

    def print_line(self, line: str):
        self.printed.append(line)

    def config(self):
        return {
            "defaults": {},
            "desktop_automation": {
                "artifact_root": "/tmp/desktop-artifacts",
                "targets": self.targets,
            },
        }

    def deps(self):
        def resolve(_config, name):
            return self.targets[name]

        def update_repo_path(config, name, repo_path):
            config["desktop_automation"]["targets"][name]["repo_path"] = repo_path
            self.targets[name]["repo_path"] = repo_path

        return {
            "load_config_fn": self.config,
            "resolve_desktop_target_fn": resolve,
            "check_writable_dir_fn": lambda _path: (True, ""),
            "desktop_target_contract_fn": lambda name, target: {
                "target": name,
                "task_name": f"PulpDesktopAutomationAgent-{name}",
                "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent"
                if target["adapter"] == "windows-session-agent"
                else None,
            },
            "ensure_host_reachable_fn": lambda _name, target, _defaults: target.get("host"),
            "bootstrap_windows_session_agent_fn": lambda _host, _contract: {
                "remote_root": r"C:\RemoteRoot",
                "script_path": r"C:\RemoteRoot\agent.ps1",
            },
            "probe_windows_session_agent_fn": lambda _host, _contract: {
                "task_present": True,
                "agent_root_exists": True,
                "jobs_dir_exists": True,
                "results_dir_exists": True,
                "script_exists": True,
            },
            "subprocess_run_fn": lambda *_args, **_kwargs: subprocess.CompletedProcess(["git"], 0, "a" * 40 + "\n", ""),
            "root_path": Path("/repo"),
            "new_install_job_id_fn": lambda: "install123",
            "sync_job_bundle_to_ssh_host_fn": lambda _host, job: (f"{job['id']}.git", "refs/pulp-ci-bundles/install"),
            "ensure_windows_remote_tooling_fn": lambda _host: {
                "probe": {
                    "git_found": True,
                    "git_path": r"C:\Program Files\Git\cmd\git.exe",
                    "git_version": "git version 2.49.0.windows.1",
                },
                "installed": ["git"],
            },
            "windows_remote_tooling_ready_fn": lambda probe: bool(probe.get("git_found")),
            "ensure_windows_remote_repo_checkout_fn": lambda *_args, **_kwargs: {
                "repo_path": r"C:\Users\daniel\pulp-validate",
                "repo_exists": True,
            },
            "git_origin_clone_url_fn": lambda _root: "https://github.com/danielraffel/pulp",
            "windows_repo_checkout_ready_fn": lambda probe: bool(probe.get("repo_exists")),
            "update_target_repo_path_fn": update_repo_path,
            "save_config_fn": self.saved_configs.append,
            "now_iso_fn": lambda: "2026-06-11T00:00:00Z",
            "desktop_target_receipt_path_fn": lambda name: Path(f"/receipts/{name}.json"),
            "atomic_write_text_fn": lambda path, text: self.writes.append((path, text)),
            "windows_tooling_detail_fn": lambda probe, tool_name, **_kwargs: f"{probe.get(tool_name + '_version')} ({probe.get(tool_name + '_path')})",
            "print_fn": self.print_line,
        }

    def written_receipt(self):
        self.assertEqual(len(self.writes), 1)
        return json.loads(self.writes[0][1])

    def test_install_records_local_receipt_and_text_output(self):
        result = self.mod.cmd_desktop_install(Namespace(target="mac"), **self.deps())

        self.assertEqual(result, 0)
        receipt = self.written_receipt()
        self.assertEqual(receipt["target"], "mac")
        self.assertEqual(receipt["adapter"], "macos-local")
        self.assertTrue(receipt["remote_bootstrap_ready"])
        self.assertTrue(receipt["remote_tooling_ready"])
        self.assertTrue(receipt["remote_repo_checkout_ready"])
        self.assertIn("Desktop target `mac` prepared.", self.printed)
        self.assertIn("  remote bootstrap: not required for local target", self.printed)

    def test_install_bootstraps_windows_and_updates_repo_path(self):
        result = self.mod.cmd_desktop_install(Namespace(target="windows"), **self.deps())

        self.assertEqual(result, 0)
        receipt = self.written_receipt()
        self.assertEqual(receipt["target"], "windows")
        self.assertEqual(receipt["adapter"], "windows-session-agent")
        self.assertTrue(receipt["remote_bootstrap_ready"])
        self.assertTrue(receipt["remote_tooling_ready"])
        self.assertTrue(receipt["remote_repo_checkout_ready"])
        self.assertEqual(receipt["repo_path"], r"C:\Users\daniel\pulp-validate")
        self.assertEqual(len(self.saved_configs), 1)
        self.assertIn("  remote bootstrap: ready", self.printed)
        self.assertIn("  remote tooling installed: git", self.printed)
        self.assertIn(r"  remote repo checkout: C:\Users\daniel\pulp-validate", self.printed)

    def test_install_records_pending_remote_when_host_unreachable(self):
        deps = self.deps()
        deps["ensure_host_reachable_fn"] = lambda *_args: None

        result = self.mod.cmd_desktop_install(Namespace(target="windows"), **deps)

        self.assertEqual(result, 0)
        receipt = self.written_receipt()
        self.assertFalse(receipt["remote_bootstrap_ready"])
        self.assertFalse(receipt["remote_tooling_ready"])
        self.assertFalse(receipt["remote_repo_checkout_ready"])
        self.assertIn("  remote bootstrap: pending; target profile recorded locally", self.printed)
        self.assertIn("  remote tooling: pending; run `pulp ci-local desktop doctor windows` for remediation", self.printed)

    def test_install_reports_load_and_writable_errors(self):
        deps = self.deps()
        deps["load_config_fn"] = lambda: (_ for _ in ()).throw(FileNotFoundError("missing config"))
        self.assertEqual(self.mod.cmd_desktop_install(Namespace(target="mac"), **deps), 1)
        self.assertEqual(self.printed[-1], "Error: missing config")

        self.printed.clear()
        deps = self.deps()
        deps["check_writable_dir_fn"] = lambda _path: (False, "permission denied")
        self.assertEqual(self.mod.cmd_desktop_install(Namespace(target="mac"), **deps), 1)
        self.assertEqual(self.printed[-1], "Error: desktop artifact root is not writable: permission denied")

    def test_doctor_text_and_json_outputs(self):
        checks = [
            {"name": "receipt", "ok": True, "detail": "installed"},
            {"name": "optional", "ok": False, "required": False, "detail": "missing optional"},
            {"name": "ssh", "ok": False, "detail": "down"},
        ]
        deps = {
            "load_config_fn": self.config,
            "resolve_desktop_target_fn": lambda _config, name: self.targets[name],
            "desktop_doctor_checks_fn": lambda _config, _name: checks,
            "print_fn": self.print_line,
        }

        result = self.mod.cmd_desktop_doctor(Namespace(target="ubuntu", json=False), **deps)
        self.assertEqual(result, 1)
        self.assertIn("Desktop doctor for `ubuntu`", self.printed)
        self.assertIn("  PASS  receipt: installed", self.printed)
        self.assertIn("  WARN  optional: missing optional", self.printed)
        self.assertIn("  FAIL  ssh: down", self.printed)

        self.printed.clear()
        result = self.mod.cmd_desktop_doctor(Namespace(target="ubuntu", json=True), **deps)
        self.assertEqual(result, 1)
        payload = json.loads(self.printed[0])
        self.assertEqual(payload["target"], "ubuntu")
        self.assertFalse(payload["ok"])
        self.assertEqual(payload["checks"], checks)


if __name__ == "__main__":
    unittest.main()
