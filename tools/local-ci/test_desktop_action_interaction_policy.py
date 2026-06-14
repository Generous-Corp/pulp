#!/usr/bin/env python3
"""No-network tests for desktop action interaction policy helpers."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_action_interaction_policy.py")


class DesktopActionInteractionPolicyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_interaction_helpers_preserve_click_selector_shape(self) -> None:
        self.assertFalse(
            self.mod.desktop_interaction_requested(
                click_point=None,
                click_view_id=None,
                click_view_type=None,
                click_view_text=None,
                click_view_label=None,
            )
        )
        self.assertTrue(
            self.mod.desktop_interaction_requested(
                click_point=None,
                click_view_id="bypass",
                click_view_type=None,
                click_view_text=None,
                click_view_label=None,
            )
        )
        self.assertEqual(
            self.mod.desktop_click_selector(
                click_point="1,2",
                click_view_id="bypass",
                click_view_type="button",
                click_view_text="Bypass",
                click_view_label="Bypass toggle",
            ),
            {
                "id": "bypass",
                "type": "button",
                "text": "Bypass",
                "label": "Bypass toggle",
                "point": "1,2",
            },
        )
        self.assertEqual(
            self.mod.desktop_click_selector(click_view_id="bypass", include_point=False),
            {"id": "bypass", "type": None, "text": None, "label": None},
        )
        self.assertEqual(
            self.mod.pulp_app_interaction_summary(
                click_point="1,2",
                click_view_id="bypass",
                click_view_type="button",
                click_view_text="Bypass",
                click_view_label="Bypass toggle",
            )["click"]["selector"]["point"],
            "1,2",
        )


if __name__ == "__main__":
    unittest.main()
