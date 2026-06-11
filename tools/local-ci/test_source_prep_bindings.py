#!/usr/bin/env python3
"""Tests for source-prep facade bindings."""

import importlib.util
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("source_prep_bindings.py")


def load_module():
    spec = importlib.util.spec_from_file_location("source_prep_bindings_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class SourcePrepBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.root = Path("/repo")
        self.run_fn = object()

    def _bindings(self, **overrides):
        bindings = {
            "ROOT": self.root,
            "subprocess": types.SimpleNamespace(run=self.run_fn),
        }
        bindings.update(overrides)
        return bindings

    def test_source_request_cache_root_and_rewrite_bind_dependencies(self):
        captured = {}

        def make_request(*args, **kwargs):
            captured["make_request"] = (args, kwargs)
            return {"mode": "live"}

        def source_cache_key(source_request):
            captured["cache_key"] = source_request
            return "abc123"

        def source_root(*args, **kwargs):
            captured["source_root"] = (args, kwargs)
            return Path("/state/desktop-source/mac/abc123")

        def command_candidate(*args, **kwargs):
            captured["candidate"] = (args, kwargs)
            return Path("/repo/tool")

        def rewrite_mapper(*args, **kwargs):
            captured["mapper"] = (args, kwargs)
            return "rewritten"

        def rewrite_source(*args, **kwargs):
            captured["source"] = (args, kwargs)
            return "source-root"

        def rewrite_posix(*args, **kwargs):
            captured["posix"] = (args, kwargs)
            return "posix-root"

        def rewrite_windows(*args, **kwargs):
            captured["windows"] = (args, kwargs)
            return "windows-root"

        def split_windows(command):
            captured["split"] = command
            return ["one", "two"]

        def validate_windows(commands):
            captured["validate"] = commands

        def attach_manifest(manifest, source_context):
            captured["attach"] = (manifest, source_context)

        source_prep = types.SimpleNamespace(
            make_desktop_source_request=make_request,
            desktop_source_cache_key=source_cache_key,
            desktop_source_root=source_root,
            command_path_rewrite_candidate=command_candidate,
            rewrite_launch_command_for_mapper=rewrite_mapper,
            rewrite_launch_command_for_source_root=rewrite_source,
            rewrite_launch_command_for_posix_root=rewrite_posix,
            rewrite_launch_command_for_windows_root=rewrite_windows,
            split_windows_prepare_commands=split_windows,
            validate_windows_prepare_commands=validate_windows,
            attach_desktop_source_to_manifest=attach_manifest,
        )
        bindings = self._bindings(
            _source_prep=source_prep,
            normalize_desktop_source_mode=object(),
            current_branch=object(),
            current_sha=object(),
            state_dir=object(),
            windows_path_join=object(),
        )
        args_obj = object()
        request = {"sha": "abc"}
        mapper = object()
        manifest = {}
        source_context = {"mode": "live"}

        self.assertEqual(self.mod.make_desktop_source_request(bindings, args_obj), {"mode": "live"})
        self.assertEqual(captured["make_request"][0], (args_obj,))
        self.assertIs(captured["make_request"][1]["normalize_desktop_source_mode_fn"], bindings["normalize_desktop_source_mode"])
        self.assertIs(captured["make_request"][1]["current_branch_fn"], bindings["current_branch"])
        self.assertIs(captured["make_request"][1]["current_sha_fn"], bindings["current_sha"])

        self.assertEqual(self.mod.desktop_source_cache_key(bindings, request), "abc123")
        self.assertIs(captured["cache_key"], request)

        self.assertEqual(self.mod.desktop_source_root(bindings, "mac", request), Path("/state/desktop-source/mac/abc123"))
        self.assertEqual(captured["source_root"][0], ("mac", request))
        self.assertIs(captured["source_root"][1]["state_dir_fn"], bindings["state_dir"])

        self.assertEqual(self.mod.command_path_rewrite_candidate(bindings, "./tool"), Path("/repo/tool"))
        self.assertEqual(captured["candidate"][0], ("./tool",))
        self.assertEqual(captured["candidate"][1]["root"], self.root)

        self.assertEqual(self.mod.rewrite_launch_command_for_mapper(bindings, "./tool", mapper, windows=True), "rewritten")
        self.assertEqual(captured["mapper"][0], ("./tool", mapper))
        self.assertEqual(captured["mapper"][1]["root"], self.root)
        self.assertTrue(captured["mapper"][1]["windows"])

        self.assertEqual(self.mod.rewrite_launch_command_for_source_root(bindings, "./tool", Path("/source")), "source-root")
        self.assertEqual(captured["source"][0], ("./tool", Path("/source")))
        self.assertEqual(captured["source"][1]["root"], self.root)

        self.assertEqual(self.mod.rewrite_launch_command_for_posix_root(bindings, "./tool", "/remote"), "posix-root")
        self.assertEqual(captured["posix"][0], ("./tool", "/remote"))
        self.assertEqual(captured["posix"][1]["root"], self.root)

        self.assertEqual(self.mod.rewrite_launch_command_for_windows_root(bindings, r".\tool.exe", r"C:\Pulp"), "windows-root")
        self.assertEqual(captured["windows"][0], (r".\tool.exe", r"C:\Pulp"))
        self.assertEqual(captured["windows"][1]["root"], self.root)
        self.assertIs(captured["windows"][1]["windows_path_join_fn"], bindings["windows_path_join"])

        self.assertEqual(self.mod.split_windows_prepare_commands(bindings, "one;two"), ["one", "two"])
        self.assertEqual(captured["split"], "one;two")

        self.mod.validate_windows_prepare_commands(bindings, ["one"])
        self.assertEqual(captured["validate"], ["one"])

        self.mod.attach_desktop_source_to_manifest(bindings, manifest, source_context)
        self.assertEqual(captured["attach"], (manifest, source_context))

    def test_local_worktree_helpers_bind_root_and_subprocess(self):
        captured = {}

        def local_worktree_matches(path, sha, **kwargs):
            captured["matches"] = (path, sha, kwargs)
            return True

        def reset_local_worktree(path, **kwargs):
            captured["reset"] = (path, kwargs)

        source_prep = types.SimpleNamespace(
            local_worktree_matches=local_worktree_matches,
            reset_local_worktree=reset_local_worktree,
        )
        bindings = self._bindings(_source_prep=source_prep)

        self.assertTrue(self.mod.local_worktree_matches(bindings, Path("/tmp/wt"), "abc123"))
        self.mod.reset_local_worktree(bindings, Path("/tmp/wt"))

        self.assertEqual(captured["matches"][0], Path("/tmp/wt"))
        self.assertEqual(captured["matches"][1], "abc123")
        self.assertIs(captured["matches"][2]["run_fn"], self.run_fn)
        self.assertEqual(captured["reset"][0], Path("/tmp/wt"))
        self.assertIs(captured["reset"][1]["run_fn"], self.run_fn)
        self.assertEqual(captured["reset"][1]["root"], self.root)

    def test_prepare_macos_exact_sha_source_binds_facade_dependencies(self):
        captured = {}

        def prepare(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"platform": "mac"}

        bindings = self._bindings(
            _source_prep=types.SimpleNamespace(prepare_macos_exact_sha_source=prepare),
            desktop_source_root=object(),
            _local_worktree_matches=object(),
            _reset_local_worktree=object(),
            run_logged_command=object(),
            tail_lines=object(),
            rewrite_launch_command_for_source_root=object(),
        )

        result = self.mod.prepare_macos_exact_sha_source(
            bindings,
            Path("/bundle"),
            "mac",
            "./tool",
            {"sha": "abc123"},
        )

        self.assertEqual(result, {"platform": "mac"})
        self.assertEqual(captured["args"], (Path("/bundle"), "mac", "./tool", {"sha": "abc123"}))
        self.assertEqual(captured["kwargs"]["root"], self.root)
        self.assertIs(captured["kwargs"]["run_fn"], self.run_fn)
        self.assertIs(captured["kwargs"]["desktop_source_root_fn"], bindings["desktop_source_root"])
        self.assertIs(captured["kwargs"]["local_worktree_matches_fn"], bindings["_local_worktree_matches"])
        self.assertIs(captured["kwargs"]["reset_local_worktree_fn"], bindings["_reset_local_worktree"])
        self.assertIs(captured["kwargs"]["rewrite_launch_command_for_source_root_fn"], bindings["rewrite_launch_command_for_source_root"])

    def test_prepare_linux_exact_sha_source_binds_facade_dependencies(self):
        captured = {}

        def prepare(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"platform": "linux"}

        bindings = self._bindings(
            _source_prep=types.SimpleNamespace(prepare_linux_exact_sha_source=prepare),
            sync_job_bundle_to_ssh_host=object(),
            git_origin_clone_url=object(),
            desktop_source_cache_key=object(),
            fetch_ssh_artifact=object(),
            rewrite_launch_command_for_posix_root=object(),
        )

        result = self.mod.prepare_linux_exact_sha_source(
            bindings,
            Path("/bundle"),
            "ubuntu",
            "host",
            "./tool",
            {"sha": "abc123"},
        )

        self.assertEqual(result, {"platform": "linux"})
        self.assertEqual(captured["args"], (Path("/bundle"), "ubuntu", "host", "./tool", {"sha": "abc123"}))
        self.assertIs(captured["kwargs"]["sync_job_bundle_to_ssh_host_fn"], bindings["sync_job_bundle_to_ssh_host"])
        self.assertIs(captured["kwargs"]["git_origin_clone_url_fn"], bindings["git_origin_clone_url"])
        self.assertIs(captured["kwargs"]["desktop_source_cache_key_fn"], bindings["desktop_source_cache_key"])
        self.assertIs(captured["kwargs"]["run_fn"], self.run_fn)
        self.assertIs(captured["kwargs"]["fetch_ssh_artifact_fn"], bindings["fetch_ssh_artifact"])

    def test_prepare_windows_exact_sha_source_binds_facade_dependencies(self):
        captured = {}

        def prepare(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"platform": "windows"}

        bindings = self._bindings(
            _source_prep=types.SimpleNamespace(prepare_windows_exact_sha_source=prepare),
            sync_job_bundle_to_ssh_host=object(),
            git_origin_clone_url=object(),
            desktop_source_cache_key=object(),
            ps_literal=object(),
            windows_contract_expand_expression=object(),
            split_windows_prepare_commands=object(),
            validate_windows_prepare_commands=object(),
            run_windows_ssh_powershell=object(),
            windows_ssh_fetch_file=object(),
            rewrite_launch_command_for_windows_root=object(),
        )

        result = self.mod.prepare_windows_exact_sha_source(
            bindings,
            Path("/bundle"),
            "windows",
            "host",
            r".\tool.exe",
            {"sha": "abc123"},
        )

        self.assertEqual(result, {"platform": "windows"})
        self.assertEqual(captured["args"], (Path("/bundle"), "windows", "host", r".\tool.exe", {"sha": "abc123"}))
        self.assertIs(captured["kwargs"]["ps_literal_fn"], bindings["ps_literal"])
        self.assertIs(captured["kwargs"]["windows_contract_expand_expression_fn"], bindings["windows_contract_expand_expression"])
        self.assertIs(captured["kwargs"]["split_windows_prepare_commands_fn"], bindings["split_windows_prepare_commands"])
        self.assertIs(captured["kwargs"]["validate_windows_prepare_commands_fn"], bindings["validate_windows_prepare_commands"])
        self.assertIs(captured["kwargs"]["run_windows_ssh_powershell_fn"], bindings["run_windows_ssh_powershell"])
        self.assertIs(captured["kwargs"]["windows_ssh_fetch_file_fn"], bindings["windows_ssh_fetch_file"])


if __name__ == "__main__":
    unittest.main()
