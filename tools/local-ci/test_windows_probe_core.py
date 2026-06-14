#!/usr/bin/env python3
"""Tests for core Windows SSH/PowerShell helper primitives."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("windows_probe_core.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsProbeCoreTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_literal_branch_and_expand_helpers(self) -> None:
        self.assertEqual(self.mod.ps_literal("O'Brien"), "O''Brien")
        self.assertEqual(self.mod.validate_ci_branch_name(" feature/windows "), "feature/windows")
        with self.assertRaisesRegex(ValueError, "CI branch name is required"):
            self.mod.validate_ci_branch_name(" ")
        with self.assertRaisesRegex(ValueError, "Unsupported branch name"):
            self.mod.validate_ci_branch_name("feature/windows; rm -rf")
        self.assertEqual(
            self.mod.windows_contract_expand_expression("%TEMP%\\O'Brien"),
            "[Environment]::ExpandEnvironmentVariables('%TEMP%\\O''Brien')",
        )
        self.assertEqual(
            self.mod.windows_session_agent_template_path(Path("/repo/tools/local-ci")),
            Path("/repo/tools/local-ci/windows_session_agent.ps1"),
        )

    def test_windows_ssh_powershell_command_and_runner(self) -> None:
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
            return SimpleNamespace(returncode=0, stdout="ok", stderr="")

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
        with self.assertRaisesRegex(RuntimeError, "no JSON"):
            self.mod.parse_windows_ssh_json("noise\n[1,2]\n")


if __name__ == "__main__":
    unittest.main()
