#!/usr/bin/env python3
"""No-network tests for local-ci desktop source preparation helpers."""

from __future__ import annotations

import argparse
import importlib.util
import pathlib
import subprocess
import tempfile
import unittest
from types import SimpleNamespace


MODULE_PATH = pathlib.Path(__file__).with_name("source_prep.py")


def load_module():
    spec = importlib.util.spec_from_file_location("source_prep_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class SourcePrepTests(unittest.TestCase):
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

    def test_request_cache_key_and_command_rewrites(self) -> None:
        args = argparse.Namespace(
            source_mode="EXACT-SHA",
            branch=None,
            sha=None,
            prepare_command="  ./scripts/build-ui.sh  ",
            prepare_timeout=42,
        )

        request = self.mod.make_desktop_source_request(
            args,
            normalize_desktop_source_mode_fn=lambda value: str(value).lower(),
            current_branch_fn=lambda: "main",
            current_sha_fn=lambda: "b" * 40,
        )

        self.assertEqual(request["mode"], "exact-sha")
        self.assertEqual(request["branch"], "main")
        self.assertEqual(request["sha"], "b" * 40)
        self.assertEqual(request["prepare_command"], "./scripts/build-ui.sh")
        self.assertEqual(request["prepare_timeout_secs"], 42.0)

        same_material = {**request, "mode": "live", "branch": "other"}
        changed_prepare = {**request, "prepare_command": "cmake --build build"}
        self.assertEqual(self.mod.desktop_source_cache_key(request), self.mod.desktop_source_cache_key(same_material))
        self.assertNotEqual(
            self.mod.desktop_source_cache_key(request),
            self.mod.desktop_source_cache_key(changed_prepare),
        )

        state = self.root / "state"
        source_root = self.mod.desktop_source_root("mac", request, state_dir_fn=lambda: state)
        self.assertEqual(source_root.parent, state / "desktop-source" / "mac")

        prepared = self.root / "prepared"
        rewritten = self.mod.rewrite_launch_command_for_source_root(
            f"{self.repo}/bin/ui-preview --label 'UI Preview'",
            prepared,
            root=self.repo,
        )
        self.assertEqual(rewritten, f"{prepared}/bin/ui-preview --label 'UI Preview'")

        posix = self.mod.rewrite_launch_command_for_posix_root(
            "./scripts/run-preview --smoke",
            "/home/dev/pulp",
            root=self.repo,
        )
        self.assertEqual(posix, "/home/dev/pulp/scripts/run-preview --smoke")

        windows = self.mod.rewrite_launch_command_for_windows_root(
            r".\scripts\run-preview.exe --smoke",
            r"C:\pulp",
            root=self.repo,
            windows_path_join_fn=lambda *parts: "\\".join(parts),
        )
        self.assertEqual(windows, r"C:\pulp\scripts\run-preview.exe --smoke")

        malformed = '"unterminated'
        self.assertEqual(
            self.mod.rewrite_launch_command_for_source_root(malformed, prepared, root=self.repo),
            malformed,
        )
        self.assertEqual(
            self.mod.rewrite_launch_command_for_source_root("/usr/bin/true --flag", prepared, root=self.repo),
            "/usr/bin/true --flag",
        )

    def test_windows_prepare_validation_and_manifest_attachment(self) -> None:
        commands = self.mod.split_windows_prepare_commands('cmake -G "Visual Studio; 17"; echo ok\nninja')
        self.assertEqual(commands, ['cmake -G "Visual Studio; 17"', "echo ok", "ninja"])

        with self.assertRaisesRegex(ValueError, "Use double quotes"):
            self.mod.validate_windows_prepare_commands(["cmake -G 'Ninja'"])

        manifest: dict = {}
        self.mod.attach_desktop_source_to_manifest(
            manifest,
            {
                "mode": "exact-sha",
                "branch": "feature/source",
                "sha": "c" * 40,
                "prepare_command": "cmake --build build",
                "prepare_timeout_secs": 120.0,
                "prepared_root": "/actual/root",
                "prepared_root_display": "~/display/root",
                "launch_cwd": "/actual/cwd",
                "launch_cwd_display": "~/display/cwd",
                "prepare_log": "prepare.log",
            },
        )

        self.assertEqual(manifest["source"]["prepared_root"], "~/display/root")
        self.assertEqual(manifest["source"]["launch_cwd"], "~/display/cwd")
        self.assertEqual(manifest["artifacts"]["prepare_log"], "prepare.log")

        unchanged = {"status": "pass"}
        self.mod.attach_desktop_source_to_manifest(unchanged, None)
        self.assertEqual(unchanged, {"status": "pass"})

    def test_local_worktree_match_and_reset_are_dependency_injected(self) -> None:
        worktree = self.root / "worktree"
        worktree.mkdir()
        self.assertFalse(
            self.mod.local_worktree_matches(
                worktree,
                "abc123",
                run_fn=lambda *_args, **_kwargs: subprocess.CompletedProcess([], 0, stdout="abc123\n", stderr=""),
            )
        )

        (worktree / ".git").write_text("gitdir: elsewhere\n")
        calls: list[tuple[list[str], dict]] = []

        def fake_run(command, **kwargs):
            calls.append((command, kwargs))
            return subprocess.CompletedProcess(command, 0, stdout="abc123\n", stderr="")

        self.assertTrue(self.mod.local_worktree_matches(worktree, "abc123", run_fn=fake_run))

        removed: list[tuple[pathlib.Path, bool]] = []
        self.mod.reset_local_worktree(
            worktree,
            root=self.repo,
            run_fn=fake_run,
            rmtree_fn=lambda path, ignore_errors=False: removed.append((path, ignore_errors)),
        )

        self.assertEqual(calls[-2][0][:3], ["git", "worktree", "remove"])
        self.assertEqual(calls[-2][1]["cwd"], self.repo)
        self.assertEqual(removed, [(worktree, True)])
        self.assertEqual(calls[-1][0], ["git", "worktree", "prune"])

    def test_prepare_macos_exact_sha_source_uses_clean_and_reused_paths(self) -> None:
        bundle_dir = self.root / "bundle-mac"
        bundle_dir.mkdir()
        prepared_root = self.root / "prepared-mac"
        source_request = self.request(prepare_command="echo prepare")
        run_calls = []

        def fake_run(command, **kwargs):
            run_calls.append((command, kwargs))
            return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

        def fake_logged_command(command, **kwargs):
            kwargs["log_path"].write_text("prepared\n")
            return {"timed_out": False, "returncode": 0}

        clean = self.mod.prepare_macos_exact_sha_source(
            bundle_dir,
            "mac",
            "./tool --flag",
            source_request,
            root=self.repo,
            desktop_source_root_fn=lambda _target, _request: prepared_root,
            local_worktree_matches_fn=lambda _path, _sha: False,
            reset_local_worktree_fn=lambda path: run_calls.append((["reset", str(path)], {})),
            run_fn=fake_run,
            run_logged_command_fn=fake_logged_command,
            tail_lines_fn=lambda _path, limit=40: ["tail"],
            rewrite_launch_command_for_source_root_fn=lambda command, root: f"{root}:{command}",
        )

        self.assertEqual(clean["prepared_state"], "clean")
        self.assertEqual(clean["launch_command"], f"{prepared_root}:./tool --flag")
        self.assertEqual(clean["prepare_log"], str(bundle_dir / "prepare.log"))
        self.assertEqual(run_calls[0][0], ["reset", str(prepared_root)])
        self.assertEqual(run_calls[1][0][:3], ["git", "worktree", "add"])

        logged = []
        reused = self.mod.prepare_macos_exact_sha_source(
            bundle_dir,
            "mac",
            "./tool",
            source_request,
            root=self.repo,
            desktop_source_root_fn=lambda _target, _request: prepared_root,
            local_worktree_matches_fn=lambda _path, _sha: True,
            reset_local_worktree_fn=lambda _path: self.fail("reset should not run for a reused worktree"),
            run_fn=fake_run,
            run_logged_command_fn=lambda *args, **kwargs: logged.append((args, kwargs)),
            tail_lines_fn=lambda _path, limit=40: ["tail"],
            rewrite_launch_command_for_source_root_fn=lambda command, root: f"{root}:{command}",
        )

        self.assertEqual(reused["prepared_state"], "reused")
        self.assertEqual(reused["launch_command"], f"{prepared_root}:./tool")
        self.assertEqual(logged, [])

    def test_prepare_linux_exact_sha_source_builds_remote_script_and_context(self) -> None:
        bundle_dir = self.root / "bundle-linux"
        bundle_dir.mkdir()
        source_request = self.request(prepare_command="./scripts/build-ui.sh")
        run_calls = []
        fetched = []

        def fake_run(command, **kwargs):
            run_calls.append((command, kwargs))
            if len(run_calls) == 1:
                return subprocess.CompletedProcess(command, 0, stdout="/home/dev\n", stderr="")
            return subprocess.CompletedProcess(command, 0, stdout="__PULP_PREPARED__:reused\n", stderr="")

        context = self.mod.prepare_linux_exact_sha_source(
            bundle_dir,
            "ubuntu",
            "host",
            "./build/ui-preview --flag",
            source_request,
            sync_job_bundle_to_ssh_host_fn=lambda host, job: ("source.bundle", "refs/pulp-ci-bundles/test"),
            git_origin_clone_url_fn=lambda root: "https://example.invalid/pulp.git",
            desktop_source_cache_key_fn=lambda request: "cache-key",
            root=self.repo,
            run_fn=fake_run,
            fetch_ssh_artifact_fn=lambda *args, **kwargs: fetched.append((args, kwargs)) or True,
            rewrite_launch_command_for_posix_root_fn=lambda command, root: f"{root}:{command}",
        )

        remote_script = run_calls[1][0][-1]
        self.assertIn('export PATH="$HOME/.local/bin:$PATH"', remote_script)
        self.assertIn("export GIT_LFS_SKIP_SMUDGE=1", remote_script)
        self.assertIn("export PULP_REQUIRE_PREPARE_STAMP=1", remote_script)
        self.assertIn("bundle_ref=refs/pulp-ci-bundles/test", remote_script)
        self.assertIn('git -C "$prepared_root" fetch "$bundle" "$bundle_ref:refs/pulp-ci-bundles/source"', remote_script)
        self.assertIn('git -C "$prepared_root" remote add origin "$remote_url"', remote_script)
        self.assertEqual(context["prepared_state"], "reused")
        self.assertEqual(context["prepared_root"], "/home/dev/.local/state/pulp/desktop-source/ubuntu/cache-key")
        self.assertEqual(context["prepared_root_display"], "~/.local/state/pulp/desktop-source/ubuntu/cache-key")
        self.assertEqual(context["launch_command"], "/home/dev/.local/state/pulp/desktop-source/ubuntu/cache-key:./build/ui-preview --flag")
        self.assertEqual(fetched[0][0][1], "/home/dev/.local/state/pulp/desktop-source/ubuntu/cache-key/prepare.log")

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
            split_windows_prepare_commands_fn=self.mod.split_windows_prepare_commands,
            validate_windows_prepare_commands_fn=self.mod.validate_windows_prepare_commands,
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
