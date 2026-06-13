#!/usr/bin/env python3
"""Tests for macOS app bundle facade bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("macos_window_app_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class MacosWindowAppBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_app_wrappers_delegate_to_macos_desktop_module(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        macos_desktop = types.SimpleNamespace(
            detect_macos_app_bundle=capture("detect", Path("/Demo.app")),
            macos_bundle_id_for_app_path=capture("bundle_id", "com.example.demo"),
            macos_window_probe_path=capture("probe_path", Path("/repo/tools/local-ci/macos_window_probe.swift")),
        )
        bindings = {
            "_macos_desktop": macos_desktop,
            "SCRIPT_DIR": Path("/repo/tools/local-ci"),
        }

        self.assertEqual(self.mod.detect_macos_app_bundle(bindings, "/Demo.app/Contents/MacOS/Demo"), Path("/Demo.app"))
        self.assertEqual(captured["detect"][0], ("/Demo.app/Contents/MacOS/Demo",))
        self.assertEqual(self.mod.macos_bundle_id_for_app_path(bindings, Path("/Demo.app")), "com.example.demo")
        self.assertEqual(captured["bundle_id"][0], (Path("/Demo.app"),))
        self.assertEqual(self.mod.macos_window_probe_path(bindings), Path("/repo/tools/local-ci/macos_window_probe.swift"))
        self.assertEqual(captured["probe_path"][0], (Path("/repo/tools/local-ci"),))

    def test_app_exports_and_installer_wire_named_helpers(self) -> None:
        expected = (
            "detect_macos_app_bundle",
            "macos_bundle_id_for_app_path",
            "macos_window_probe_path",
        )
        self.assertEqual(self.mod.MACOS_WINDOW_APP_EXPORTS, expected)

        macos_desktop = types.SimpleNamespace(
            detect_macos_app_bundle=lambda command: Path("/Demo.app") if command else None,
            macos_bundle_id_for_app_path=lambda app_path: f"id:{app_path.name}",
            macos_window_probe_path=lambda script_dir: script_dir / "macos_window_probe.swift",
        )
        bindings = {
            "_macos_desktop": macos_desktop,
            "SCRIPT_DIR": Path("/repo/tools/local-ci"),
        }

        self.mod.install_macos_window_app_helpers(bindings, ("detect_macos_app_bundle", "macos_window_probe_path"))

        self.assertEqual(bindings["detect_macos_app_bundle"]("/Demo.app/Contents/MacOS/Demo"), Path("/Demo.app"))
        self.assertEqual(bindings["macos_window_probe_path"](), Path("/repo/tools/local-ci/macos_window_probe.swift"))
        self.assertNotIn("macos_bundle_id_for_app_path", bindings)

    def test_app_installer_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_macos_window_app_helper = lambda _bindings: "future"

        self.mod.install_macos_window_app_helpers(bindings, ("future_macos_window_app_helper",))

        self.assertEqual(bindings["future_macos_window_app_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
