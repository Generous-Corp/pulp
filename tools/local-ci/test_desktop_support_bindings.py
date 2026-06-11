#!/usr/bin/env python3
"""Tests for generic desktop support facade bindings."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("desktop_support_bindings.py")


def load_module():
    spec = importlib.util.spec_from_file_location("desktop_support_bindings_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class DesktopSupportBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, *, artifacts=None, doctor=None, actions=None):
        return {
            "_desktop_artifacts": artifacts or types.SimpleNamespace(),
            "_desktop_doctor": doctor or types.SimpleNamespace(),
            "_desktop_actions": actions or types.SimpleNamespace(),
            "desktop_receipts_dir": object(),
            "desktop_target_receipt_path": object(),
        }

    def test_artifact_wrappers_bind_facade_dependencies(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        artifacts = types.SimpleNamespace(
            desktop_target_receipt_path=capture("receipt_path", Path("/receipts/mac.json")),
            desktop_receipt_for=capture("receipt", {"installed": True}),
            desktop_artifact_root=capture("artifact_root", Path("/artifacts")),
            create_desktop_run_bundle=capture("run_bundle", Path("/artifacts/mac/smoke/run")),
            desktop_publish_root=capture("publish_root", Path("/artifacts/_published")),
            create_desktop_publish_bundle=capture("publish_bundle", Path("/artifacts/_published/run")),
        )
        bindings = self._bindings(artifacts=artifacts)
        config = {"desktop_automation": {"artifact_root": "/artifacts"}}

        self.assertEqual(self.mod.desktop_target_receipt_path(bindings, "mac"), Path("/receipts/mac.json"))
        self.assertIs(captured["receipt_path"][1]["desktop_receipts_dir_fn"], bindings["desktop_receipts_dir"])
        self.assertEqual(self.mod.desktop_receipt_for(bindings, "mac"), {"installed": True})
        self.assertIs(captured["receipt"][1]["desktop_target_receipt_path_fn"], bindings["desktop_target_receipt_path"])
        self.assertEqual(self.mod.desktop_artifact_root(bindings, config), Path("/artifacts"))
        self.assertEqual(captured["artifact_root"][0], (config,))
        self.assertEqual(self.mod.create_desktop_run_bundle(bindings, config, "mac", "smoke"), Path("/artifacts/mac/smoke/run"))
        self.assertEqual(captured["run_bundle"][0], (config, "mac", "smoke"))
        self.assertEqual(self.mod.desktop_publish_root(bindings, config), Path("/artifacts/_published"))
        self.assertEqual(self.mod.create_desktop_publish_bundle(bindings, config), Path("/artifacts/_published/run"))

    def test_resolve_desktop_target_preserves_selection_errors(self) -> None:
        config = {
            "desktop_automation": {
                "targets": {
                    "mac": {"adapter": "macos-local"},
                    "linux": {"adapter": "linux-xvfb", "enabled": False},
                }
            }
        }

        self.assertEqual(
            self.mod.resolve_desktop_target(self._bindings(), config, "mac"),
            {"adapter": "macos-local"},
        )
        with self.assertRaisesRegex(ValueError, "Unknown desktop target 'windows'"):
            self.mod.resolve_desktop_target(self._bindings(), config, "windows")
        with self.assertRaisesRegex(ValueError, "Desktop target 'linux' is disabled"):
            self.mod.resolve_desktop_target(self._bindings(), config, "linux")

    def test_doctor_and_action_wrappers_delegate_arguments(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        doctor = types.SimpleNamespace(
            desktop_optional_capabilities=capture("optional", ["webview_dom"]),
            desktop_capabilities_for=capture("caps", ["launch_app"]),
            desktop_check=capture("check", {"ok": True}),
            check_writable_dir=capture("writable", (True, "/tmp")),
            webdriver_status_url=capture("webdriver", "http://host/status"),
        )
        actions = types.SimpleNamespace(
            count_view_tree_nodes=capture("count", 3),
            parse_coordinate_pair=capture("coord", (1.0, 2.0)),
            iter_view_tree_nodes=lambda *args, **kwargs: iter(captured.setdefault("iter", (args, kwargs)) and [("node", {"x": 1})]),
            resolve_view_tree_click_point=capture("resolve", (10.0, 20.0)),
            screen_point_for_content_point=capture("screen", (30.0, 40.0)),
            default_desktop_label=capture("label", "Demo"),
        )
        bindings = self._bindings(doctor=doctor, actions=actions)
        node = {"children": []}

        self.assertEqual(self.mod.desktop_optional_capabilities(bindings, {"webview_driver": True}), ["webview_dom"])
        self.assertEqual(captured["optional"][0], ({"webview_driver": True},))
        self.assertEqual(self.mod.desktop_capabilities_for(bindings, "macos-local", "v2", {"debug_attach": True}), ["launch_app"])
        self.assertEqual(captured["caps"][0], ("macos-local", "v2", {"debug_attach": True}))
        self.assertEqual(self.mod.desktop_check(bindings, "ssh", True, "ok", required=False), {"ok": True})
        self.assertEqual(captured["check"][0], ("ssh", True, "ok"))
        self.assertEqual(captured["check"][1], {"required": False})
        self.assertEqual(self.mod.check_writable_dir(bindings, Path("/tmp")), (True, "/tmp"))
        self.assertEqual(self.mod.webdriver_status_url(bindings, "http://host"), "http://host/status")

        self.assertEqual(self.mod.count_view_tree_nodes(bindings, node), 3)
        self.assertEqual(self.mod.parse_coordinate_pair(bindings, "1,2", flag_name="--click"), (1.0, 2.0))
        self.assertEqual(list(self.mod.iter_view_tree_nodes(bindings, node, offset_x=4.0, offset_y=5.0)), [("node", {"x": 1})])
        iter_args, iter_kwargs = captured["iter"]
        self.assertEqual(iter_args, (node,))
        self.assertEqual(iter_kwargs, {"offset_x": 4.0, "offset_y": 5.0})
        self.assertEqual(
            self.mod.resolve_view_tree_click_point(
                bindings,
                node,
                view_id="gain",
                view_type="Slider",
                view_text="Gain",
                view_label="Gain slider",
            ),
            (10.0, 20.0),
        )
        self.assertEqual(captured["resolve"][1]["view_label"], "Gain slider")
        self.assertEqual(self.mod.screen_point_for_content_point(bindings, {"bounds": {}}, (100, 50), (1, 2)), (30.0, 40.0))
        self.assertEqual(self.mod.default_desktop_label(bindings, "./Demo", bundle_id="com.example.Demo"), "Demo")
        self.assertEqual(captured["label"][0], ("./Demo",))
        self.assertEqual(captured["label"][1], {"bundle_id": "com.example.Demo"})


if __name__ == "__main__":
    unittest.main()
