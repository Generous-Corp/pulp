#!/usr/bin/env python3
"""Tests for macOS desktop smoke artifact dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("macos_desktop_smoke_artifact_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class MacosDesktopSmokeArtifactDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_artifact_dependencies_bind_facade_values(self) -> None:
        desktop_actions = types.SimpleNamespace(
            desktop_action_artifact_paths=object(),
            desktop_interaction_requested=object(),
        )
        bindings = {"_desktop_actions": desktop_actions}
        for name in [
            "create_desktop_run_bundle",
            "now_iso",
            "prepare_macos_exact_sha_source",
            "image_change_summary",
            "attach_desktop_source_to_manifest",
            "atomic_write_text",
            "write_desktop_run_rollups",
        ]:
            bindings[name] = object()

        deps = self.mod.macos_desktop_smoke_artifact_dependencies(bindings)

        self.assertIs(deps["create_desktop_run_bundle_fn"], bindings["create_desktop_run_bundle"])
        self.assertIs(deps["desktop_action_artifact_paths_fn"], desktop_actions.desktop_action_artifact_paths)
        self.assertIs(deps["desktop_interaction_requested_fn"], desktop_actions.desktop_interaction_requested)
        self.assertIs(deps["prepare_macos_exact_sha_source_fn"], bindings["prepare_macos_exact_sha_source"])
        self.assertIs(deps["image_change_summary_fn"], bindings["image_change_summary"])
        self.assertIs(deps["write_desktop_run_rollups_fn"], bindings["write_desktop_run_rollups"])


if __name__ == "__main__":
    unittest.main()
