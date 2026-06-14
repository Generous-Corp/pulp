#!/usr/bin/env python3
"""Tests for Linux desktop SSH artifact facade bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
from pathlib import Path
import types
import unittest



def load_module():
    return load_local_ci_module("linux_desktop_artifact_bindings.py")


class LinuxDesktopArtifactBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_match_artifact_helpers(self) -> None:
        self.assertEqual(
            self.mod.LINUX_DESKTOP_ARTIFACT_EXPORTS,
            (
                "fetch_ssh_artifact",
                "cleanup_remote_ssh_dir",
            ),
        )
        self.assertEqual(len(self.mod.LINUX_DESKTOP_ARTIFACT_EXPORTS), len(set(self.mod.LINUX_DESKTOP_ARTIFACT_EXPORTS)))

    def test_ssh_artifact_helpers_bind_facade_dependencies(self) -> None:
        captured = {}

        def fetch(*args, **kwargs):
            captured["fetch"] = (args, kwargs)
            return True

        def cleanup(*args, **kwargs):
            captured["cleanup"] = (args, kwargs)

        subprocess_mod = types.SimpleNamespace(run=object())
        ssh_command_result = object()
        bindings = {
            "_linux_desktop_action": types.SimpleNamespace(
                fetch_ssh_artifact=fetch,
                cleanup_remote_ssh_dir=cleanup,
            ),
            "subprocess": subprocess_mod,
            "ssh_command_result": ssh_command_result,
        }
        local_path = Path("/tmp/out.txt")

        self.assertTrue(
            self.mod.fetch_ssh_artifact(
                bindings,
                "host",
                "/tmp/remote.txt",
                local_path,
                optional=True,
                timeout=5,
            )
        )
        self.assertEqual(captured["fetch"][0], ("host", "/tmp/remote.txt", local_path))
        self.assertEqual(captured["fetch"][1]["optional"], True)
        self.assertEqual(captured["fetch"][1]["timeout"], 5)
        self.assertIs(captured["fetch"][1]["run_fn"], subprocess_mod.run)

        self.mod.cleanup_remote_ssh_dir(bindings, "host", '"$HOME/bundle"')
        self.assertEqual(captured["cleanup"][0], ("host", '"$HOME/bundle"'))
        self.assertIs(captured["cleanup"][1]["ssh_command_result_fn"], ssh_command_result)


if __name__ == "__main__":
    unittest.main()
