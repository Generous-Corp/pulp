#!/usr/bin/env python3
"""Tests for exact-SHA source prepare script builders."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module
from source_prep import split_windows_prepare_commands, validate_windows_prepare_commands



def load_module():
    return load_local_ci_module("source_prep_exact_scripts.py", add_module_dir=True)


class SourcePrepExactScriptsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_linux_prepare_command_includes_cache_reuse_and_prepare_stamp(self) -> None:
        script = self.mod.build_linux_exact_sha_prepare_command(
            bundle_name="source.bundle",
            bundle_ref="refs/pulp-ci-bundles/test",
            prepared_root="/home/dev/.local/state/pulp/desktop-source/ubuntu/cache",
            prepare_stamp="/home/dev/.local/state/pulp/desktop-source/ubuntu/cache/.pulp-prepare-ok",
            sha="a" * 40,
            remote_url="https://example.invalid/pulp.git",
            prepare_command="./scripts/build-ui.sh",
            remote_prepare_log="/home/dev/.local/state/pulp/desktop-source/ubuntu/cache/prepare.log",
        )

        self.assertIn('export PATH="$HOME/.local/bin:$PATH"', script)
        self.assertIn("export GIT_LFS_SKIP_SMUDGE=1", script)
        self.assertIn("export PULP_REQUIRE_PREPARE_STAMP=1", script)
        self.assertIn("bundle_ref=refs/pulp-ci-bundles/test", script)
        self.assertIn('git -C "$prepared_root" fetch "$bundle" "$bundle_ref:refs/pulp-ci-bundles/source"', script)
        self.assertIn('git -C "$prepared_root" remote add origin "$remote_url"', script)
        self.assertIn("bash -lc ./scripts/build-ui.sh", script)
        self.assertIn("printf '%s\\n' \"$sha\" > \"$prepare_stamp\"", script)

    def test_linux_prepare_command_omits_prepare_stamp_without_prepare_command(self) -> None:
        script = self.mod.build_linux_exact_sha_prepare_command(
            bundle_name="source.bundle",
            bundle_ref="refs/pulp-ci-bundles/test",
            prepared_root="/home/dev/root",
            prepare_stamp="/home/dev/root/.pulp-prepare-ok",
            sha="a" * 40,
            remote_url="",
            prepare_command=None,
            remote_prepare_log="/home/dev/root/prepare.log",
        )

        self.assertNotIn("export PULP_REQUIRE_PREPARE_STAMP=1", script)
        self.assertIn("__PULP_PREPARED__:clean", script)

    def test_windows_prepare_script_includes_prepare_batch_and_stamp(self) -> None:
        script = self.mod.build_windows_exact_sha_prepare_script(
            bundle_name="source.bundle",
            bundle_ref="refs/pulp-ci-bundles/test",
            prepared_root=r"%LOCALAPPDATA%\Pulp\desktop-source\windows\cache",
            remote_prepare_log=r"%LOCALAPPDATA%\Pulp\desktop-source\windows\cache\prepare.log",
            prepare_stamp=r"%LOCALAPPDATA%\Pulp\desktop-source\windows\cache\.pulp-prepare-ok",
            prepare_script_path=r"%LOCALAPPDATA%\Pulp\desktop-source\windows\cache\.pulp-prepare.cmd",
            sha="a" * 40,
            remote_url="",
            prepare_command=r".\scripts\build-ui.cmd; echo done",
            ps_literal_fn=lambda value: value.replace("'", "''"),
            windows_contract_expand_expression_fn=lambda raw: f"[expand]{raw}",
            split_windows_prepare_commands_fn=split_windows_prepare_commands,
            validate_windows_prepare_commands_fn=validate_windows_prepare_commands,
        )

        self.assertIn("$env:GIT_LFS_SKIP_SMUDGE = '1'", script)
        self.assertIn("$env:PULP_REQUIRE_PREPARE_STAMP = '1'", script)
        self.assertIn("$Bundle = Join-Path $HOME 'source.bundle'", script)
        self.assertIn("$PreparedRoot = [expand]%LOCALAPPDATA%\\Pulp\\desktop-source\\windows\\cache", script)
        self.assertIn("cmd.exe /c \"rmdir /s /q \\\"$PreparedRoot\\\"\"", script)
        self.assertIn("@echo off", script)
        self.assertIn(r".\scripts\build-ui.cmd", script)
        self.assertIn("Set-Content -LiteralPath $PrepareStamp -Value $Sha -Encoding UTF8", script)
        self.assertIn("__PULP_PREPARED__:clean", script)

    def test_windows_prepare_script_omits_prepare_batch_without_prepare_command(self) -> None:
        script = self.mod.build_windows_exact_sha_prepare_script(
            bundle_name="source.bundle",
            bundle_ref="refs/pulp-ci-bundles/test",
            prepared_root=r"%LOCALAPPDATA%\Pulp\desktop-source\windows\cache",
            remote_prepare_log=r"%LOCALAPPDATA%\Pulp\desktop-source\windows\cache\prepare.log",
            prepare_stamp=r"%LOCALAPPDATA%\Pulp\desktop-source\windows\cache\.pulp-prepare-ok",
            prepare_script_path=r"%LOCALAPPDATA%\Pulp\desktop-source\windows\cache\.pulp-prepare.cmd",
            sha="a" * 40,
            remote_url="",
            prepare_command=None,
            ps_literal_fn=lambda value: value,
            windows_contract_expand_expression_fn=lambda raw: raw,
            split_windows_prepare_commands_fn=split_windows_prepare_commands,
            validate_windows_prepare_commands_fn=validate_windows_prepare_commands,
        )

        self.assertNotIn("$env:PULP_REQUIRE_PREPARE_STAMP = '1'", script)
        self.assertNotIn("@echo off", script)
        self.assertIn("__PULP_PREPARED__:reused", script)


if __name__ == "__main__":
    unittest.main()
