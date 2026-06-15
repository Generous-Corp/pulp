#!/usr/bin/env python3
from __future__ import annotations

from argparse import Namespace
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_action_selectors.py")


class DesktopActionSelectorsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def args(self, **overrides):
        values = {
            "click": None,
            "click_view_id": None,
            "click_view_type": None,
            "click_view_text": None,
            "click_view_label": None,
        }
        values.update(overrides)
        return Namespace(**values)

    def test_selector_predicates_distinguish_point_and_view_targets(self):
        self.assertFalse(self.mod.windows_requires_pulp_app_selectors(self.args(click="10,20")))
        self.assertTrue(self.mod.windows_requires_pulp_app_selectors(self.args(click_view_label="Bypass")))
        self.assertFalse(self.mod.desktop_click_has_target(self.args()))
        self.assertTrue(self.mod.desktop_click_has_target(self.args(click_view_type="Button")))


if __name__ == "__main__":
    unittest.main()
