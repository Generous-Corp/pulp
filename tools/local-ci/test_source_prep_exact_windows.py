#!/usr/bin/env python3
"""No-network tests for Windows exact-SHA source materialization."""

from __future__ import annotations

import pathlib
import tempfile
import unittest
from types import SimpleNamespace

from module_test_utils import load_local_ci_module
from source_prep import split_windows_prepare_commands, validate_windows_prepare_commands



def load_module():
    return load_local_ci_module("source_prep_exact_windows.py", add_module_dir=True)


class SourcePrepExactWindowsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def request(self, **overrides) -> dict:
        request = {
            "mode": "exact-sha",
            "branch": "feature/source",
            "sha": "a" * 40,
            "prepare_command": None,
            "prepare_timeout_secs": 120.0,
        }
        request.update(overrides)
        return request

    def test_prepare_windows_exact_sha_source_builds_powershell_script_and_context(self) -> None:
        bundle_dir = self.root / "bundle-windows"
        bundle_dir.mkdir()
        source_request = self.request(prepare_command=r".\scripts\build-ui.cmd; echo done")
        scripts = []
        fetched = []

        context = self.mod.prepare_windows_exact_sha_source(
            bundle_dir,
            "windows",
            "win",
            r".\build\ui-preview.exe --flag",
            source_request,
            sync_job_bundle_to_ssh_host_fn=lambda host, job: ("source.bundle", "refs/pulp-ci-bundles/test"),
            git_origin_clone_url_fn=lambda root: "",
            desktop_source_cache_key_fn=lambda request: "cache-key",
            root=self.repo,
            ps_literal_fn=lambda value: value.replace("'", "''"),
            windows_contract_expand_expression_fn=lambda raw: f"[expand]{raw}",
            split_windows_prepare_commands_fn=split_windows_prepare_commands,
            validate_windows_prepare_commands_fn=validate_windows_prepare_commands,
            run_windows_ssh_powershell_fn=lambda host, script, **kwargs: scripts.append((host, script, kwargs))
            or SimpleNamespace(returncode=0, stdout="__PULP_PREPARED__:clean\n", stderr=""),
            windows_ssh_fetch_file_fn=lambda *args, **kwargs: fetched.append((args, kwargs)) or True,
            rewrite_launch_command_for_windows_root_fn=lambda command, root: f"{root}:{command}",
        )

        self.assertEqual(scripts[0][0], "win")
        script = scripts[0][1]
        self.assertIn("$env:GIT_LFS_SKIP_SMUDGE = '1'", script)
        self.assertIn("$env:PULP_REQUIRE_PREPARE_STAMP = '1'", script)
        self.assertIn("$Bundle = Join-Path $HOME 'source.bundle'", script)
        self.assertIn("$PreparedRoot = [expand]%LOCALAPPDATA%\\Pulp\\desktop-source\\windows\\cache-key", script)
        self.assertIn("cmd.exe /c \"rmdir /s /q \\\"$PreparedRoot\\\"\"", script)
        self.assertIn("@echo off", script)
        self.assertIn(r".\scripts\build-ui.cmd", script)
        self.assertIn("Set-Content -LiteralPath $PrepareStamp -Value $Sha -Encoding UTF8", script)
        self.assertEqual(context["prepared_state"], "clean")
        self.assertEqual(context["prepared_root"], r"%LOCALAPPDATA%\Pulp\desktop-source\windows\cache-key")
        self.assertEqual(context["launch_command"], r"%LOCALAPPDATA%\Pulp\desktop-source\windows\cache-key:.\build\ui-preview.exe --flag")
        self.assertEqual(fetched[0][0][1], r"%LOCALAPPDATA%\Pulp\desktop-source\windows\cache-key\prepare.log")


if __name__ == "__main__":
    unittest.main()
