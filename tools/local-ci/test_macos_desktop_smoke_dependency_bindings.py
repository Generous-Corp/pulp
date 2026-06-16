#!/usr/bin/env python3
"""Tests for macOS desktop smoke/action dependency bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("macos_desktop_smoke_dependency_bindings.py")


class MacosDesktopSmokeDependencyBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_dependency_exports_match_wrappers(self):
        expected = ("macos_desktop_smoke_dependencies",)
        focused_expected = (
            *self.mod.MACOS_DESKTOP_SMOKE_ARTIFACT_DEPENDENCY_EXPORTS,
            *self.mod.MACOS_DESKTOP_SMOKE_PROCESS_DEPENDENCY_EXPORTS,
            *self.mod.MACOS_DESKTOP_SMOKE_WINDOW_DEPENDENCY_EXPORTS,
            *self.mod.MACOS_DESKTOP_SMOKE_INTERACTION_DEPENDENCY_EXPORTS,
            *self.mod.MACOS_DESKTOP_SMOKE_VIDEO_DEPENDENCY_EXPORTS,
        )

        self.assertEqual(self.mod.MACOS_DESKTOP_SMOKE_DEPENDENCY_EXPORTS, expected)
        self.assertEqual(self.mod.MACOS_DESKTOP_SMOKE_FOCUSED_DEPENDENCY_EXPORTS, focused_expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_macos_desktop_smoke_dependencies_merges_focused_groups(self):
        bindings = {}

        with (
            mock.patch.object(
                self.mod,
                "macos_desktop_smoke_artifact_dependencies",
                return_value={"artifact": object()},
            ) as artifact_deps,
            mock.patch.object(
                self.mod,
                "macos_desktop_smoke_process_dependencies",
                return_value={"process": object()},
            ) as process_deps,
            mock.patch.object(
                self.mod,
                "macos_desktop_smoke_window_dependencies",
                return_value={"window": object()},
            ) as window_deps,
            mock.patch.object(
                self.mod,
                "macos_desktop_smoke_interaction_dependencies",
                return_value={"interaction": object()},
            ) as interaction_deps,
            mock.patch.object(
                self.mod,
                "macos_desktop_smoke_video_dependencies",
                return_value={"video": object()},
            ) as video_deps,
        ):
            deps = self.mod.macos_desktop_smoke_dependencies(bindings)

        self.assertEqual(set(deps), {"artifact", "process", "window", "interaction", "video"})
        artifact_deps.assert_called_once_with(bindings)
        process_deps.assert_called_once_with(bindings)
        window_deps.assert_called_once_with(bindings)
        interaction_deps.assert_called_once_with(bindings)
        video_deps.assert_called_once_with(bindings)

    def test_install_dependency_helpers_preserves_unknown_fallback(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_macos_desktop_smoke_dependency_helpers(bindings, ("unknown_helper",))

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ()),
                mock.call(bindings, self.mod.__dict__, ("unknown_helper",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
