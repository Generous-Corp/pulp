#!/usr/bin/env python3
"""No-network tests for local-ci Windows SSH/PowerShell probe helpers."""

from __future__ import annotations

import base64
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("windows_probe.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


def completed(*, returncode: int = 0, stdout: str = "", stderr: str = ""):
    return SimpleNamespace(returncode=returncode, stdout=stdout, stderr=stderr)


class WindowsProbeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.tmp_path = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_literal_branch_command_and_subprocess_wrapper(self) -> None:
        self.assertEqual(self.mod.ps_literal("O'Brien"), "O''Brien")
        self.assertEqual(self.mod.validate_ci_branch_name(" feature/windows "), "feature/windows")
        with self.assertRaises(ValueError):
            self.mod.validate_ci_branch_name("feature/windows; rm -rf")

        self.assertEqual(
            self.mod.windows_ssh_powershell_command("win2"),
            [
                "ssh",
                "win2",
                "powershell",
                "-NoProfile",
                "-NonInteractive",
                "-Command",
                "$script = [Console]::In.ReadToEnd(); Invoke-Expression $script",
            ],
        )

        captured: dict = {}

        def fake_run(args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return completed(stdout="ok")

        result = self.mod.run_windows_ssh_powershell(
            "win2",
            "Get-Date",
            timeout=17,
            run_ssh_subprocess_fn=fake_run,
        )

        self.assertEqual(result.stdout, "ok")
        self.assertEqual(captured["args"][0:2], ["ssh", "win2"])
        self.assertEqual(captured["kwargs"], {"input": "Get-Date", "timeout": 17})

    def test_parse_windows_ssh_json_uses_last_object(self) -> None:
        self.assertEqual(
            self.mod.parse_windows_ssh_json('noise\n{"first":1}\nmore\n{"second":2}\n'),
            {"second": 2},
        )
        with self.assertRaisesRegex(RuntimeError, "no JSON"):
            self.mod.parse_windows_ssh_json("noise\nnull\n")

    def test_repo_probe_and_bootstrap_use_callbacks(self) -> None:
        scripts: list[dict] = []
        unsafe_calls: list[tuple[str | None, str | None]] = []

        def fake_run(_host, script, *, timeout=0):
            scripts.append({"script": script, "timeout": timeout})
            return completed(
                stdout='{"home_dir":"C:\\\\Users\\\\dev","repo_path":"C:\\\\Pulp","head_exists":true,"setup_exists":true}\n'
            )

        def fake_unsafe(repo_path, home_dir):
            unsafe_calls.append((repo_path, home_dir))
            return repo_path == r"C:\Users\dev"

        probe = self.mod.probe_windows_repo_checkout(
            "win",
            "Owner's Repo",
            run_windows_ssh_powershell_fn=fake_run,
            windows_repo_path_is_unsafe_fn=fake_unsafe,
        )

        self.assertFalse(probe["repo_path_unsafe"])
        self.assertIn("$RepoRaw = 'Owner''s Repo'", scripts[0]["script"])
        self.assertIn("git -C $Repo remote 2>$null", scripts[0]["script"])
        self.assertIn("Where-Object { $_ -eq 'origin' }", scripts[0]["script"])
        self.assertEqual(scripts[0]["timeout"], 60)
        self.assertEqual(unsafe_calls, [(r"C:\Pulp", r"C:\Users\dev")])

        bootstrap_scripts: list[dict] = []

        def fake_bootstrap_run(_host, script, *, timeout=0):
            bootstrap_scripts.append({"script": script, "timeout": timeout})
            return completed(
                stdout=(
                    '{"home_dir":"C:\\\\Users\\\\dev",'
                    '"repo_path":"C:\\\\Users\\\\dev\\\\pulp-validate",'
                    '"head_exists":true,"setup_exists":true}\n'
                )
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
            run_windows_ssh_powershell_fn=fake_bootstrap_run,
        )
        self.assertEqual(ensured["repo_path"], r"C:\Users\dev\pulp-validate")
        self.assertIn("& git -C $Repo fetch $BundlePath", bootstrap_scripts[0]["script"])
        self.assertIn(r"C:\Users\dev\pulp-validate", bootstrap_scripts[0]["script"])
        self.assertEqual(bootstrap_scripts[0]["timeout"], 120)

    def test_session_tooling_and_install_helpers_use_callbacks(self) -> None:
        scripts: list[dict] = []

        def fake_run(_host, script, *, timeout=0):
            scripts.append({"script": script, "timeout": timeout})
            if "Get-ScheduledTask" in script:
                return completed(stdout='{"task_present":true,"interactive_user":"dev"}\n')
            if "Get-Command git" in script:
                return completed(stdout='{"git_found":true,"winget_found":true}\n')
            return completed(returncode=7, stdout="out", stderr="err")

        session = self.mod.probe_windows_session_agent(
            "win",
            {
                "task_name": "PulpTask",
                "remote_root": r"%LOCALAPPDATA%\Pulp",
                "script_path": r"%LOCALAPPDATA%\Pulp\agent.ps1",
            },
            run_windows_ssh_powershell_fn=fake_run,
        )
        self.assertTrue(session["task_present"])
        self.assertEqual(session["interactive_user"], "dev")
        self.assertIn("Get-ScheduledTask", scripts[-1]["script"])
        self.assertIn("quser", scripts[-1]["script"])

        tooling = self.mod.probe_windows_remote_tooling("win", run_windows_ssh_powershell_fn=fake_run)
        self.assertTrue(tooling["git_found"])
        self.assertIn("Get-Command git", scripts[-1]["script"])
        self.assertIn("auth status", scripts[-1]["script"])

        with self.assertRaisesRegex(RuntimeError, "err"):
            self.mod.install_windows_remote_tool(
                "win",
                "Git.Git",
                timeout=5,
                run_windows_ssh_powershell_fn=fake_run,
            )
        self.assertIn("$PackageId = 'Git.Git'", scripts[-1]["script"])
        self.assertIn("'--id'", scripts[-1]["script"])
        self.assertIn("& $Winget.Source @InstallArgs", scripts[-1]["script"])
        self.assertEqual(scripts[-1]["timeout"], 5)

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
            required_tools={"git": {"winget_id": "Git.Git", "required": True}},
            optional_tools={"gh": {"winget_id": "GitHub.cli", "required": False}},
            probe_windows_remote_tooling_fn=lambda _host: next(probes),
            install_windows_remote_tool_fn=fake_install,
        )
        self.assertEqual(installs, ["Git.Git", "GitHub.cli"])
        self.assertEqual(ensured["installed"], ["git"])

        with self.assertRaisesRegex(RuntimeError, "winget.*unavailable"):
            self.mod.ensure_windows_remote_tooling(
                "win",
                install_optional=False,
                required_tools={"git": {"winget_id": "Git.Git", "required": True}},
                optional_tools={},
                probe_windows_remote_tooling_fn=lambda _host: {"git_found": False, "winget_found": False},
                install_windows_remote_tool_fn=fake_install,
            )

    def test_remote_file_helpers_use_callbacks(self) -> None:
        scripts: list[str] = []

        def fake_expand(raw: str) -> str:
            return f"EXPAND({raw})"

        def fake_run(host, script, *, timeout=0):
            scripts.append(script)
            if "WriteAllBytes" in script:
                return completed(stdout='{"exists":true}\n')
            if "ReadAllBytes" in script:
                payload = base64.b64encode(b"hello from windows").decode("ascii")
                return completed(stdout=payload + "\n")
            if "Get-Content" in script:
                return completed(stdout=json.dumps({"status": "ok"}) + "\n")
            return completed()

        self.mod.windows_ssh_write_text(
            "win",
            r"%TEMP%\pulp\agent.ps1",
            "agent text",
            run_windows_ssh_powershell_fn=fake_run,
            windows_contract_expand_expression_fn=fake_expand,
        )
        self.assertIn("[System.IO.File]::WriteAllBytes", scripts[-1])
        self.assertIn("EXPAND(%TEMP%\\pulp\\agent.ps1)", scripts[-1])

        local_file = self.tmp_path / "fetched" / "out.txt"
        self.assertTrue(
            self.mod.windows_ssh_fetch_file(
                "win",
                r"%TEMP%\pulp\out.txt",
                local_file,
                run_windows_ssh_powershell_fn=fake_run,
                windows_contract_expand_expression_fn=fake_expand,
            )
        )
        self.assertEqual(local_file.read_text(), "hello from windows")

        self.assertEqual(
            self.mod.windows_ssh_read_json(
                "win",
                r"%TEMP%\pulp\manifest.json",
                run_windows_ssh_powershell_fn=fake_run,
                windows_contract_expand_expression_fn=fake_expand,
            ),
            {"status": "ok"},
        )

    def test_missing_remote_files_respect_optional_flag(self) -> None:
        def fake_missing(host, script, *, timeout=0):
            return completed(stdout="__PULP_MISSING__\n")

        local_file = self.tmp_path / "missing.txt"
        self.assertFalse(
            self.mod.windows_ssh_fetch_file(
                "win",
                r"%TEMP%\missing.txt",
                local_file,
                optional=True,
                run_windows_ssh_powershell_fn=fake_missing,
            )
        )
        self.assertIsNone(
            self.mod.windows_ssh_read_json(
                "win",
                r"%TEMP%\missing.json",
                optional=True,
                run_windows_ssh_powershell_fn=fake_missing,
            )
        )
        with self.assertRaisesRegex(RuntimeError, "does not exist"):
            self.mod.windows_ssh_fetch_file(
                "win",
                r"%TEMP%\missing.txt",
                local_file,
                run_windows_ssh_powershell_fn=fake_missing,
            )

    def test_session_agent_bootstrap_and_start_use_callbacks(self) -> None:
        template = self.tmp_path / "windows_session_agent.ps1"
        template.write_text("agent body")
        contract = {
            "task_name": "PulpDesktopAutomationAgent-win",
            "remote_root": r"%LOCALAPPDATA%\Pulp\agent",
            "script_path": r"%LOCALAPPDATA%\Pulp\agent\agent.ps1",
        }
        writes: list[tuple[str, str, str]] = []
        scripts: list[str] = []

        def fake_write(host, remote_path, content):
            writes.append((host, remote_path, content))

        def fake_run(host, script, *, timeout=0):
            scripts.append(script)
            return completed(stdout='{"task_present":true,"task_name":"PulpDesktopAutomationAgent-win"}\n')

        result = self.mod.bootstrap_windows_session_agent(
            "win",
            contract,
            windows_session_agent_template_path_fn=lambda: template,
            windows_ssh_write_text_fn=fake_write,
            run_windows_ssh_powershell_fn=fake_run,
        )

        self.assertEqual(result["task_present"], True)
        self.assertEqual(writes, [("win", contract["script_path"], "agent body")])
        self.assertIn("Register-ScheduledTask", scripts[-1])
        self.assertIn("PulpDesktopAutomationAgent-win", scripts[-1])

        self.mod.start_windows_session_agent_task(
            "win",
            contract,
            run_windows_ssh_powershell_fn=fake_run,
        )
        self.assertIn("Start-ScheduledTask", scripts[-1])

    def test_probe_windows_ssh_cmake_settings_parses_and_falls_back(self) -> None:
        def fail_if_called(*args, **kwargs):
            raise AssertionError("probe should not run when both values are supplied")

        self.assertEqual(
            self.mod.probe_windows_ssh_cmake_settings(
                "win",
                "Visual Studio 17 2022",
                "x64",
                "C:/VS",
                windows_ssh_powershell_command_fn=lambda host: ["ssh", host],
                run_fn=fail_if_called,
            ),
            ("x64", "C:/VS"),
        )

        captured: dict = {}

        def fake_run(args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return completed(stdout='noise\n{"platform":"ARM64","generator_instance":"C:/VS/Community"}\n')

        self.assertEqual(
            self.mod.probe_windows_ssh_cmake_settings(
                "win",
                "Visual Studio 17 2022",
                "",
                "",
                windows_ssh_powershell_command_fn=lambda host: ["ssh", host],
                run_fn=fake_run,
            ),
            ("ARM64", "C:/VS/Community"),
        )
        self.assertEqual(captured["args"], ["ssh", "win"])
        self.assertEqual(captured["kwargs"]["timeout"], 60)

        def fake_raises(*args, **kwargs):
            raise OSError("ssh unavailable")

        self.assertEqual(
            self.mod.probe_windows_ssh_cmake_settings(
                "win",
                "Ninja",
                "x64",
                "",
                windows_ssh_powershell_command_fn=lambda host: ["ssh", host],
                run_fn=fake_raises,
            ),
            ("x64", ""),
        )


if __name__ == "__main__":
    unittest.main()
