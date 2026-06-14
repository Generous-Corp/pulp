#!/usr/bin/env python3
"""Tests for macOS window facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("macos_window_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class MacosWindowBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, macos_desktop):
        return {
            "_macos_desktop": macos_desktop,
            "SCRIPT_DIR": Path("/repo/tools/local-ci"),
            "subprocess": types.SimpleNamespace(run=object()),
            "time": types.SimpleNamespace(time=object(), sleep=object()),
            "macos_window_probe_path": object(),
            "macos_window_info_for_pid": object(),
            "macos_window_info_for_bundle_id": object(),
            "activate_macos_bundle_id": object(),
        }

    def test_simple_wrappers_delegate_to_macos_desktop_module(self) -> None:
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
            terminate_process=capture("terminate", None),
        )
        bindings = self._bindings(macos_desktop)
        proc = object()

        self.assertEqual(self.mod.detect_macos_app_bundle(bindings, "/Demo.app/Contents/MacOS/Demo"), Path("/Demo.app"))
        self.assertEqual(captured["detect"][0], ("/Demo.app/Contents/MacOS/Demo",))
        self.assertEqual(self.mod.macos_bundle_id_for_app_path(bindings, Path("/Demo.app")), "com.example.demo")
        self.assertEqual(captured["bundle_id"][0], (Path("/Demo.app"),))
        self.assertEqual(self.mod.macos_window_probe_path(bindings), Path("/repo/tools/local-ci/macos_window_probe.swift"))
        self.assertEqual(captured["probe_path"][0], (Path("/repo/tools/local-ci"),))
        self.mod.terminate_process(bindings, proc, timeout_secs=1.25)
        self.assertEqual(captured["terminate"][0], (proc,))
        self.assertEqual(captured["terminate"][1], {"timeout_secs": 1.25})

    def test_probe_wait_capture_activation_and_click_bind_facade_dependencies(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        run_fn = object()
        time_fn = object()
        sleep_fn = object()
        macos_desktop = types.SimpleNamespace(
            macos_window_info_for_pid=capture("pid_info", {"windows": [{"id": 7}]}),
            macos_window_info_for_bundle_id=capture("bundle_info", {"pid": 99, "windows": [{"id": 8}]}),
            macos_accessibility_trusted=capture("trusted", True),
            wait_for_macos_window=capture("wait_pid", {"id": 7}),
            wait_for_macos_bundle_window=capture("wait_bundle", (99, {"id": 8})),
            capture_macos_window=capture("capture", None),
            activate_macos_pid=capture("activate_pid", {"activated": True}),
            activate_macos_bundle_id=capture("activate_bundle", {"activated": True}),
            dispatch_macos_click=capture("click", {"clicked": True}),
            quit_macos_bundle_id=capture("quit", None),
        )
        bindings = self._bindings(macos_desktop)
        bindings["subprocess"] = types.SimpleNamespace(run=run_fn)
        bindings["time"] = types.SimpleNamespace(time=time_fn, sleep=sleep_fn)

        self.assertEqual(self.mod.macos_window_info_for_pid(bindings, 123), {"windows": [{"id": 7}]})
        self.assertIs(captured["pid_info"][1]["probe_path_fn"], bindings["macos_window_probe_path"])
        self.assertIs(captured["pid_info"][1]["run_fn"], run_fn)
        self.assertEqual(self.mod.macos_window_info_for_bundle_id(bindings, "com.example.demo"), {"pid": 99, "windows": [{"id": 8}]})
        self.assertIs(captured["bundle_info"][1]["probe_path_fn"], bindings["macos_window_probe_path"])
        self.assertTrue(self.mod.macos_accessibility_trusted(bindings))
        self.assertIs(captured["trusted"][1]["run_fn"], run_fn)
        self.assertEqual(self.mod.wait_for_macos_window(bindings, 123, 2.0), {"id": 7})
        self.assertIs(captured["wait_pid"][1]["macos_window_info_for_pid_fn"], bindings["macos_window_info_for_pid"])
        self.assertIs(captured["wait_pid"][1]["time_fn"], time_fn)
        self.assertIs(captured["wait_pid"][1]["sleep_fn"], sleep_fn)
        self.assertEqual(self.mod.wait_for_macos_bundle_window(bindings, "com.example.demo", 2.0), (99, {"id": 8}))
        self.assertIs(captured["wait_bundle"][1]["activate_macos_bundle_id_fn"], bindings["activate_macos_bundle_id"])

        self.mod.capture_macos_window(bindings, 7, Path("/tmp/window.png"))
        self.assertIs(captured["capture"][1]["run_fn"], run_fn)
        self.assertIs(captured["capture"][1]["sleep_fn"], sleep_fn)
        self.assertEqual(self.mod.activate_macos_pid(bindings, 123), {"activated": True})
        self.assertIs(captured["activate_pid"][1]["probe_path_fn"], bindings["macos_window_probe_path"])
        self.assertEqual(self.mod.activate_macos_bundle_id(bindings, "com.example.demo"), {"activated": True})
        self.assertIs(captured["activate_bundle"][1]["run_fn"], run_fn)
        self.assertEqual(self.mod.dispatch_macos_click(bindings, 10.0, 20.0), {"clicked": True})
        self.assertIs(captured["click"][1]["probe_path_fn"], bindings["macos_window_probe_path"])
        self.mod.quit_macos_bundle_id(bindings, "com.example.demo")
        self.assertIs(captured["quit"][1]["run_fn"], run_fn)

    def test_install_macos_window_helpers_wires_named_exports(self) -> None:
        macos_desktop = types.SimpleNamespace(
            detect_macos_app_bundle=lambda command: Path("/Demo.app") if command else None,
            macos_bundle_id_for_app_path=lambda app_path: f"id:{app_path.name}",
        )
        bindings = self._bindings(macos_desktop)

        self.mod.install_macos_window_helpers(
            bindings,
            ("detect_macos_app_bundle", "macos_bundle_id_for_app_path"),
        )

        self.assertEqual(bindings["detect_macos_app_bundle"]("/Demo.app/Contents/MacOS/Demo"), Path("/Demo.app"))
        self.assertEqual(bindings["macos_bundle_id_for_app_path"](Path("/Demo.app")), "id:Demo.app")

    def test_window_exports_compose_focused_groups(self) -> None:
        expected = (
            *self.mod.MACOS_WINDOW_APP_EXPORTS,
            *self.mod.MACOS_WINDOW_PROBE_EXPORTS,
            *self.mod.MACOS_WINDOW_ACTION_EXPORTS,
        )

        self.assertEqual(self.mod.MACOS_WINDOW_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_macos_window_helpers_routes_selected_groups(self) -> None:
        macos_desktop = types.SimpleNamespace(
            detect_macos_app_bundle=lambda command: Path("/Demo.app") if command else None,
            macos_accessibility_trusted=lambda **kwargs: True,
            activate_macos_bundle_id=lambda bundle_id, **kwargs: {"bundle_id": bundle_id},
        )
        bindings = self._bindings(macos_desktop)

        self.mod.install_macos_window_helpers(
            bindings,
            ("detect_macos_app_bundle", "macos_accessibility_trusted", "activate_macos_bundle_id"),
        )

        self.assertEqual(bindings["detect_macos_app_bundle"]("/Demo.app/Contents/MacOS/Demo"), Path("/Demo.app"))
        self.assertTrue(bindings["macos_accessibility_trusted"]())
        self.assertEqual(bindings["activate_macos_bundle_id"]("com.example.demo"), {"bundle_id": "com.example.demo"})
        self.assertNotIn("macos_bundle_id_for_app_path", bindings)
        self.assertNotIn("wait_for_macos_window", bindings)
        self.assertNotIn("dispatch_macos_click", bindings)


    def test_install_macos_window_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_macos_window_helper = lambda _bindings: "future"

        self.mod.install_macos_window_helpers(bindings, ("future_macos_window_helper",))

        self.assertEqual(bindings["future_macos_window_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
