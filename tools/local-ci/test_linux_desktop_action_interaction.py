#!/usr/bin/env python3
"""No-network tests for Linux desktop action interaction helpers."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).resolve().with_name("linux_desktop_action_interaction.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxDesktopActionInteractionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_attach_pulp_and_x11_interaction_summaries(self) -> None:
        pulp_manifest: dict = {}
        self.mod.attach_linux_interaction_summary(
            pulp_manifest,
            interaction_requested=True,
            pulp_app_automation=True,
            click_point=None,
            click_view_id="bypass",
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            parse_coordinate_pair_fn=lambda *_args, **_kwargs: self.fail("unexpected parse"),
            pulp_app_interaction_summary_fn=lambda **selector: {"mode": "pulp-app", "selector": selector},
        )
        self.assertEqual(pulp_manifest["interaction"]["selector"]["click_view_id"], "bypass")

        x11_manifest: dict = {}
        self.mod.attach_linux_interaction_summary(
            x11_manifest,
            interaction_requested=True,
            pulp_app_automation=False,
            click_point="20,30",
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            parse_coordinate_pair_fn=lambda point, **_kwargs: tuple(float(part) for part in point.split(",")),
            pulp_app_interaction_summary_fn=lambda **selector: {"mode": "pulp-app", "selector": selector},
        )
        self.assertEqual(x11_manifest["interaction"]["mode"], "x11-window-driver")
        self.assertEqual(x11_manifest["interaction"]["click"]["content_point"], {"x": 20.0, "y": 30.0})


if __name__ == "__main__":
    unittest.main()
