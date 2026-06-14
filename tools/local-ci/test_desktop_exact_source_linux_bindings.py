#!/usr/bin/env python3
"""Tests for Linux remote exact-source preparation dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_exact_source_linux_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopExactSourceLinuxBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.root = Path("/repo")
        self.run_fn = object()

    def test_prepare_linux_exact_sha_source_binds_facade_dependencies(self):
        captured = {}

        def prepare(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"platform": "linux"}

        bindings = {
            "_source_prep": types.SimpleNamespace(prepare_linux_exact_sha_source=prepare),
            "ROOT": self.root,
            "subprocess": types.SimpleNamespace(run=self.run_fn),
            "sync_job_bundle_to_ssh_host": object(),
            "git_origin_clone_url": object(),
            "desktop_source_cache_key": object(),
            "fetch_ssh_artifact": object(),
            "rewrite_launch_command_for_posix_root": object(),
        }

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
        self.assertEqual(captured["kwargs"]["root"], self.root)
        self.assertIs(captured["kwargs"]["run_fn"], self.run_fn)
        self.assertIs(captured["kwargs"]["fetch_ssh_artifact_fn"], bindings["fetch_ssh_artifact"])
        self.assertIs(captured["kwargs"]["rewrite_launch_command_for_posix_root_fn"], bindings["rewrite_launch_command_for_posix_root"])

    def test_linux_exports_match_wrappers(self):
        expected = ("prepare_linux_exact_sha_source",)
        self.assertEqual(self.mod.DESKTOP_EXACT_SOURCE_LINUX_EXPORTS, expected)
        self.assertTrue(callable(self.mod.prepare_linux_exact_sha_source))


if __name__ == "__main__":
    unittest.main()
