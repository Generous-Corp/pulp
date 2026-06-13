#!/usr/bin/env python3
"""Tests for Linux remote bundle path dependency bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("linux_target_bundle_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxTargetBundleBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_bundle_exports_match_facade_helpers(self) -> None:
        expected = ("remote_linux_bundle_relpath",)

        self.assertEqual(self.mod.LINUX_TARGET_BUNDLE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_remote_bundle_relpath_delegates_to_linux_target_module(self) -> None:
        captured = {}

        def remote_linux_bundle_relpath(*args):
            captured["relpath"] = args
            return ".local/state/pulp/desktop-automation/remote/ubuntu/smoke/run"

        bindings = {"_linux_target": types.SimpleNamespace(remote_linux_bundle_relpath=remote_linux_bundle_relpath)}
        bundle_dir = Path("/tmp/run")

        self.assertEqual(
            self.mod.remote_linux_bundle_relpath(bindings, "ubuntu", "smoke", bundle_dir),
            ".local/state/pulp/desktop-automation/remote/ubuntu/smoke/run",
        )
        self.assertEqual(captured["relpath"], ("ubuntu", "smoke", bundle_dir))

    def test_install_linux_target_bundle_helpers_wires_named_exports(self) -> None:
        linux_target = types.SimpleNamespace(
            remote_linux_bundle_relpath=lambda target_name, action_name, bundle_dir: f"{target_name}/{action_name}/{bundle_dir.name}",
        )
        bindings = {"_linux_target": linux_target}

        self.mod.install_linux_target_bundle_helpers(bindings, ("remote_linux_bundle_relpath",))

        self.assertEqual(
            bindings["remote_linux_bundle_relpath"]("ubuntu", "smoke", Path("/tmp/run")),
            "ubuntu/smoke/run",
        )

    def test_install_linux_target_bundle_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_linux_bundle_helper = lambda _bindings: "future"

        self.mod.install_linux_target_bundle_helpers(bindings, ("future_linux_bundle_helper",))

        self.assertEqual(bindings["future_linux_bundle_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
