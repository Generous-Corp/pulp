#!/usr/bin/env python3
"""No-network tests for local-ci Windows probe helpers."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import unittest


MODULE_PATH = Path(__file__).with_name("windows_probe.py")


def load_module():
    spec = importlib.util.spec_from_file_location("windows_probe_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class WindowsProbeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_powershell_transport_and_json_helpers(self) -> None:
        self.assertEqual(self.mod.ps_literal("owner's"), "owner''s")
        self.assertEqual(
            self.mod.windows_contract_expand_expression(r"%LOCALAPPDATA%\Pulp"),
            r"[Environment]::ExpandEnvironmentVariables('%LOCALAPPDATA%\Pulp')",
        )
        self.assertEqual(
            self.mod.windows_ssh_powershell_command("win"),
            [
                "ssh",
                "win",
                "powershell",
                "-NoProfile",
                "-NonInteractive",
                "-Command",
                "$script = [Console]::In.ReadToEnd(); Invoke-Expression $script",
            ],
        )

        calls: list[dict] = []

        def fake_run_ssh(args, **kwargs):
            calls.append({"args": args, "kwargs": kwargs})
            return subprocess.CompletedProcess(args, 0, stdout='{"ok": true}\n', stderr="")

        result = self.mod.run_windows_ssh_powershell(
            "win",
            "Get-ChildItem",
            timeout=12,
            run_ssh_subprocess_fn=fake_run_ssh,
            windows_ssh_powershell_command_fn=self.mod.windows_ssh_powershell_command,
        )
        self.assertEqual(result.returncode, 0)
        self.assertEqual(calls[0]["args"][0:3], ["ssh", "win", "powershell"])
        self.assertEqual(calls[0]["kwargs"]["input"], "Get-ChildItem")
        self.assertEqual(calls[0]["kwargs"]["timeout"], 12)

        self.assertEqual(self.mod.parse_windows_ssh_json("noise\n{\"ok\": true}\n"), {"ok": True})
        with self.assertRaisesRegex(RuntimeError, "no JSON payload"):
            self.mod.parse_windows_ssh_json("[]\n")
        with self.assertRaisesRegex(RuntimeError, "no JSON payload"):
            self.mod.parse_windows_ssh_json("not json\n")

    def test_repo_probe_and_bootstrap_use_injected_dependencies(self) -> None:
        run_calls: list[dict] = []
        unsafe_calls: list[tuple[str | None, str | None]] = []

        def fake_run(_host, ps_script, *, timeout):
            run_calls.append({"script": ps_script, "timeout": timeout})
            return subprocess.CompletedProcess(
                [],
                0,
                stdout='{"home_dir":"C:\\\\Users\\\\dev","repo_path":"C:\\\\Pulp","head_exists":true,"setup_exists":true}\n',
                stderr="",
            )

        def fake_unsafe(repo_path, home_dir):
            unsafe_calls.append((repo_path, home_dir))
            return repo_path == r"C:\Users\dev"

        probe = self.mod.probe_windows_repo_checkout(
            "win",
            "Owner's Repo",
            ps_literal_fn=self.mod.ps_literal,
            run_windows_ssh_powershell_fn=fake_run,
            parse_windows_ssh_json_fn=self.mod.parse_windows_ssh_json,
            windows_repo_path_is_unsafe_fn=fake_unsafe,
        )
        self.assertFalse(probe["repo_path_unsafe"])
        self.assertIn("$RepoRaw = 'Owner''s Repo'", run_calls[0]["script"])
        self.assertEqual(run_calls[0]["timeout"], 60)
        self.assertEqual(unsafe_calls, [(r"C:\Pulp", r"C:\Users\dev")])

        bootstrap_calls: list[dict] = []

        def fake_bootstrap_run(_host, ps_script, *, timeout):
            bootstrap_calls.append({"script": ps_script, "timeout": timeout})
            return subprocess.CompletedProcess(
                [],
                0,
                stdout='{"home_dir":"C:\\\\Users\\\\dev","repo_path":"C:\\\\Users\\\\dev\\\\pulp-validate","head_exists":true,"setup_exists":true}\n',
                stderr="",
            )

        ensured = self.mod.ensure_windows_remote_repo_checkout(
            "win",
            r"C:\Users\dev",
            remote_url="https://example.invalid/pulp.git",
            bundle_name="bundle.git",
            bundle_ref="refs/pulp/source",
            probe_windows_repo_checkout_fn=lambda _host, _repo_path: {
                "home_dir": r"C:\Users\dev",
                "repo_path": r"C:\Users\dev",
                "head_exists": False,
                "setup_exists": False,
            },
            windows_repo_path_is_unsafe_fn=fake_unsafe,
            windows_default_repo_checkout_path_fn=lambda home: home + r"\pulp-validate",
            windows_contract_expand_expression_fn=self.mod.windows_contract_expand_expression,
            ps_literal_fn=self.mod.ps_literal,
            run_windows_ssh_powershell_fn=fake_bootstrap_run,
            parse_windows_ssh_json_fn=self.mod.parse_windows_ssh_json,
        )
        self.assertEqual(ensured["repo_path"], r"C:\Users\dev\pulp-validate")
        self.assertIn("& git -C $Repo fetch $BundlePath", bootstrap_calls[0]["script"])
        self.assertIn(r"C:\Users\dev\pulp-validate", bootstrap_calls[0]["script"])
        self.assertEqual(bootstrap_calls[0]["timeout"], 120)

    def test_session_tooling_and_install_helpers(self) -> None:
        run_calls: list[dict] = []

        def fake_run(_host, ps_script, *, timeout):
            run_calls.append({"script": ps_script, "timeout": timeout})
            if "Get-ScheduledTask" in ps_script:
                return subprocess.CompletedProcess([], 0, stdout='{"task_present":true}\n', stderr="")
            if "Get-Command git" in ps_script:
                return subprocess.CompletedProcess([], 0, stdout='{"git_found":true,"winget_found":true}\n', stderr="")
            return subprocess.CompletedProcess([], 7, stdout="out", stderr="err")

        session = self.mod.probe_windows_session_agent(
            "win",
            {"task_name": "PulpTask", "remote_root": r"%LOCALAPPDATA%\Pulp", "script_path": r"%LOCALAPPDATA%\Pulp\agent.ps1"},
            ps_literal_fn=self.mod.ps_literal,
            windows_contract_expand_expression_fn=self.mod.windows_contract_expand_expression,
            run_windows_ssh_powershell_fn=fake_run,
            parse_windows_ssh_json_fn=self.mod.parse_windows_ssh_json,
        )
        self.assertTrue(session["task_present"])
        tooling = self.mod.probe_windows_remote_tooling(
            "win",
            run_windows_ssh_powershell_fn=fake_run,
            parse_windows_ssh_json_fn=self.mod.parse_windows_ssh_json,
        )
        self.assertTrue(tooling["git_found"])

        with self.assertRaisesRegex(RuntimeError, "err"):
            self.mod.install_windows_remote_tool(
                "win",
                "Git.Git",
                timeout=5,
                ps_literal_fn=self.mod.ps_literal,
                run_windows_ssh_powershell_fn=fake_run,
            )
        self.assertIn("--id 'Git.Git'", run_calls[-1]["script"])
        self.assertEqual(run_calls[-1]["timeout"], 5)

    def test_ensure_remote_tooling_required_and_optional_install_flow(self) -> None:
        probes = iter(
            [
                {"git_found": False, "winget_found": True},
                {"git_found": True, "gh_found": False, "winget_found": True},
                {"git_found": True, "gh_found": False, "winget_found": True},
            ]
        )
        installs: list[str] = []

        def fake_install(_host, package_id):
            installs.append(package_id)
            if package_id == "GitHub.cli":
                raise RuntimeError("optional failed")

        ensured = self.mod.ensure_windows_remote_tooling(
            "win",
            install_optional=True,
            probe_windows_remote_tooling_fn=lambda _host: next(probes),
            install_windows_remote_tool_fn=fake_install,
            required_tools={"git": {"winget_id": "Git.Git", "required": True}},
            optional_tools={"gh": {"winget_id": "GitHub.cli", "required": False}},
        )
        self.assertEqual(installs, ["Git.Git", "GitHub.cli"])
        self.assertEqual(ensured["installed"], ["git"])

        with self.assertRaisesRegex(RuntimeError, "winget.*unavailable"):
            self.mod.ensure_windows_remote_tooling(
                "win",
                install_optional=False,
                probe_windows_remote_tooling_fn=lambda _host: {"git_found": False, "winget_found": False},
                install_windows_remote_tool_fn=fake_install,
                required_tools={"git": {"winget_id": "Git.Git", "required": True}},
                optional_tools={},
            )


if __name__ == "__main__":
    unittest.main()
