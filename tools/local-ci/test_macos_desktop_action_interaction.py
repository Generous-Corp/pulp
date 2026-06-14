#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("macos_desktop_action_interaction.py")


class MacosDesktopActionInteractionTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.calls: list[tuple[str, object]] = []

    def summary(self, **overrides):
        values = {
            "window": {"windowId": 88},
            "content_size": (320.0, 200.0),
            "pid": 4242,
            "click_point": "20,30",
            "click_view_id": None,
            "click_view_type": None,
            "click_view_text": None,
            "click_view_label": None,
            "view_tree": {"id": "root"},
            "parse_coordinate_pair_fn": lambda point, **_kwargs: tuple(float(part) for part in point.split(",")),
            "resolve_view_tree_click_point_fn": lambda *_args, **_kwargs: (12.0, 24.0),
            "screen_point_for_content_point_fn": lambda _window, _content_size, point: (point[0] + 10.0, point[1] + 20.0),
            "activate_macos_pid_fn": lambda pid: {"activated": pid},
            "dispatch_macos_click_fn": lambda x, y: {"clicked": True, "x": x, "y": y},
            "desktop_click_selector_fn": lambda **kwargs: kwargs,
        }
        values.update(overrides)
        return self.mod.macos_desktop_event_interaction_summary(**values)

    def test_explicit_click_point_builds_desktop_event_summary(self):
        summary = self.summary()

        self.assertEqual(summary["mode"], "desktop-event")
        self.assertEqual(summary["click"]["content_point"], {"x": 20.0, "y": 30.0})
        self.assertEqual(summary["click"]["screen_point"], {"x": 30.0, "y": 50.0})
        self.assertEqual(summary["click"]["activation"], {"activated": 4242})
        self.assertEqual(summary["click"]["dispatch"], {"clicked": True, "x": 30.0, "y": 50.0})
        self.assertFalse(summary["click"]["selector"]["include_point"])

    def test_view_selector_uses_resolved_view_tree_point(self):
        def resolver(view_tree, **kwargs):
            self.calls.append(("resolver", (view_tree, kwargs)))
            return (46.0, 125.0)

        summary = self.summary(
            click_point=None,
            click_view_id="bypass-toggle",
            resolve_view_tree_click_point_fn=resolver,
        )

        self.assertEqual(summary["click"]["content_point"], {"x": 46.0, "y": 125.0})
        self.assertEqual(summary["click"]["screen_point"], {"x": 56.0, "y": 145.0})
        self.assertEqual(self.calls[0][1][1]["view_id"], "bypass-toggle")
        self.assertEqual(summary["click"]["selector"]["click_view_id"], "bypass-toggle")

    def test_missing_pid_skips_activation_call(self):
        def activate(_pid):
            raise AssertionError("activate should not be called without a pid")

        summary = self.summary(pid=None, activate_macos_pid_fn=activate)

        self.assertEqual(summary["click"]["activation"], {"activated": False})


if __name__ == "__main__":
    unittest.main()
