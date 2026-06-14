#!/usr/bin/env python3
"""No-network tests for macOS desktop action capture flow."""

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).resolve().with_name("macos_desktop_action_capture.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class MacosDesktopActionCaptureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)
        self.paths = {
            "screenshot_path": self.root / "after.png",
            "before_screenshot_path": self.root / "before.png",
            "ui_snapshot_path": self.root / "ui-tree.json",
        }

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def complete(self, **overrides):
        waited = overrides.pop("waited", [])
        captured = overrides.pop("captured", [])
        slept = overrides.pop("slept", [])
        bundle_waits = overrides.pop("bundle_waits", [])
        window = overrides.pop("window", {"windowId": 88, "bounds": {"width": 320, "height": 200}})

        def wait_for_path(path: Path, _timeout: float) -> None:
            waited.append(path.name)
            path.parent.mkdir(parents=True, exist_ok=True)
            if path.name == "ui-tree.json":
                path.write_text(json.dumps({"id": "root", "type": "Window", "contentSize": [640, 480]}))
            else:
                path.write_bytes(b"png")

        def capture_window(window_id: int, path: Path) -> None:
            captured.append((window_id, path.name))
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"png")

        kwargs = {
            "window": window,
            "pid": 4242,
            "bundle_id": None,
            "launch_descriptor": {"command": ["/repo/build/ui-preview"]},
            "capture_ui_snapshot": False,
            "use_pulp_app_automation": False,
            "interaction_requested": False,
            "capture_before": False,
            "settle_secs": 0.0,
            "timeout_secs": 5.0,
            "click_point": None,
            "click_view_id": None,
            "click_view_type": None,
            "click_view_text": None,
            "click_view_label": None,
            **self.paths,
            "content_size_from_window_fn": lambda _window: (320.0, 200.0),
            "wait_for_path_fn": wait_for_path,
            "content_size_from_view_tree_fn": lambda _tree, _fallback: (640.0, 480.0),
            "view_tree_inspector_summary_fn": lambda tree: {"root_type": tree["type"], "node_count": 1},
            "pulp_app_interaction_summary_fn": lambda **selector: {"mode": "pulp-app", "selector": selector},
            "capture_macos_window_fn": capture_window,
            "parse_coordinate_pair_fn": lambda point, **_kwargs: tuple(float(part) for part in point.split(",")),
            "resolve_view_tree_click_point_fn": lambda *_args, **_kwargs: (12.0, 24.0),
            "screen_point_for_content_point_fn": lambda _window, _content_size, content_point: (
                content_point[0] + 10.0,
                content_point[1] + 20.0,
            ),
            "activate_macos_pid_fn": lambda pid: {"activated": bool(pid)},
            "dispatch_macos_click_fn": lambda x, y: {"clicked": True, "x": x, "y": y},
            "desktop_click_selector_fn": lambda **selector: selector,
            "wait_for_macos_bundle_window_fn": lambda bundle_id, _timeout: bundle_waits.append(bundle_id)
            or (5151, {"windowId": 99, "bounds": {"width": 200, "height": 120}}),
            "sleep_fn": lambda secs: slept.append(secs),
        }
        kwargs.update(overrides)
        result = self.mod.complete_macos_desktop_action_capture(**kwargs)
        return result, waited, captured, slept, bundle_waits

    def test_loads_view_tree_snapshot_before_desktop_capture(self) -> None:
        result, waited, captured, _slept, _bundle_waits = self.complete(capture_ui_snapshot=True)

        self.assertEqual(result["content_size"], (640.0, 480.0))
        self.assertEqual(result["inspector_summary"], {"root_type": "Window", "node_count": 1})
        self.assertEqual(waited, ["ui-tree.json"])
        self.assertEqual(captured, [(88, "after.png")])
        self.assertIsNone(result["interaction_summary"])

    def test_waits_for_pulp_app_artifacts_and_returns_interaction(self) -> None:
        result, waited, captured, _slept, _bundle_waits = self.complete(
            capture_ui_snapshot=True,
            use_pulp_app_automation=True,
            interaction_requested=True,
            capture_before=True,
            click_view_id="bypass-toggle",
        )

        self.assertEqual(waited, ["before.png", "after.png", "ui-tree.json"])
        self.assertEqual(captured, [])
        self.assertEqual(result["interaction_summary"]["mode"], "pulp-app")
        self.assertEqual(result["interaction_summary"]["selector"]["click_view_id"], "bypass-toggle")
        self.assertEqual(result["inspector_summary"], {"root_type": "Window", "node_count": 1})

    def test_dispatches_desktop_click_and_settles(self) -> None:
        result, _waited, captured, slept, _bundle_waits = self.complete(
            interaction_requested=True,
            capture_before=True,
            click_point="20,30",
            settle_secs=0.5,
        )

        self.assertEqual(captured, [(88, "before.png"), (88, "after.png")])
        self.assertEqual(slept, [0.5])
        self.assertEqual(result["interaction_summary"]["mode"], "desktop-event")
        self.assertEqual(result["interaction_summary"]["click"]["screen_point"], {"x": 30.0, "y": 50.0})

    def test_recaptures_bundle_window_after_capture_failure(self) -> None:
        calls = []

        def capture_window(window_id: int, path: Path) -> None:
            calls.append((window_id, path.name))
            if len(calls) == 1:
                raise RuntimeError("stale window")
            path.write_bytes(b"png")

        result, _waited, _captured, _slept, bundle_waits = self.complete(
            bundle_id="com.example.Pulp",
            launch_descriptor={"bundle_id": "com.example.Pulp"},
            capture_macos_window_fn=capture_window,
        )

        self.assertEqual(calls, [(88, "after.png"), (99, "after.png")])
        self.assertEqual(bundle_waits, ["com.example.Pulp"])
        self.assertEqual(result["pid"], 5151)
        self.assertEqual(result["window"]["windowId"], 99)


if __name__ == "__main__":
    unittest.main()
