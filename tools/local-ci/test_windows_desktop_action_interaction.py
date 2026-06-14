#!/usr/bin/env python3
"""No-network tests for Windows desktop action interaction helpers."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).resolve().with_name("windows_desktop_action_interaction.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsDesktopActionInteractionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_prefers_remote_interaction_and_marks_generic_fallback(self) -> None:
        remote_manifest: dict = {}
        self.mod.attach_windows_interaction_summary(
            remote_manifest,
            remote_manifest={"interaction": {"mode": "window-capture", "click": {"point": "1,2"}}},
            interaction_requested=True,
            pulp_app_automation=False,
            click_point="1,2",
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            pulp_app_interaction_summary_fn=lambda **selector: {"mode": "pulp-app", "selector": selector},
        )
        self.assertEqual(remote_manifest["interaction"], {"mode": "window-capture", "click": {"point": "1,2"}})

        fallback: dict = {}
        self.mod.attach_windows_interaction_summary(
            fallback,
            remote_manifest={"status": "pass"},
            interaction_requested=True,
            pulp_app_automation=False,
            click_point="1,2",
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            pulp_app_interaction_summary_fn=lambda **selector: {"mode": "pulp-app", "selector": selector},
        )
        self.assertEqual(fallback["interaction"]["mode"], "window-capture")


if __name__ == "__main__":
    unittest.main()
