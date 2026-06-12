#!/usr/bin/env python3
"""Tests for Windows remote file helpers."""

from __future__ import annotations

import base64
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("windows_remote_files.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


def completed(*, returncode: int = 0, stdout: str = "", stderr: str = ""):
    return SimpleNamespace(returncode=returncode, stdout=stdout, stderr=stderr)


class WindowsRemoteFilesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.tmp_path = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_remote_file_helpers_use_injected_callbacks(self) -> None:
        scripts: list[str] = []

        def fake_expand(raw: str) -> str:
            return f"EXPAND({raw})"

        def fake_run(_host, script, *, timeout=0):
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

        self.mod.windows_ssh_remove_path(
            "win",
            r"%TEMP%\pulp\old",
            run_windows_ssh_powershell_fn=fake_run,
            windows_contract_expand_expression_fn=fake_expand,
        )
        self.assertIn("Remove-Item", scripts[-1])

    def test_missing_remote_files_respect_optional_flag(self) -> None:
        def fake_missing(_host, _script, *, timeout=0):
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

    def test_remote_file_errors_use_stderr_unless_optional(self) -> None:
        def fake_error(_host, _script, *, timeout=0):
            return completed(returncode=7, stdout="out", stderr="err")

        with self.assertRaisesRegex(RuntimeError, "err"):
            self.mod.windows_ssh_write_text(
                "win",
                r"%TEMP%\a.txt",
                "text",
                run_windows_ssh_powershell_fn=fake_error,
            )
        self.assertFalse(
            self.mod.windows_ssh_fetch_file(
                "win",
                r"%TEMP%\a.txt",
                self.tmp_path / "a.txt",
                optional=True,
                run_windows_ssh_powershell_fn=fake_error,
            )
        )
        self.assertIsNone(
            self.mod.windows_ssh_read_json(
                "win",
                r"%TEMP%\a.json",
                optional=True,
                run_windows_ssh_powershell_fn=fake_error,
            )
        )


if __name__ == "__main__":
    unittest.main()
