#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).resolve().with_name("reporting_publish_html.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ReportingPublishHtmlTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_run_card_escapes_metadata_and_renders_available_images(self):
        card = self.mod.desktop_publish_run_card_html(
            {
                "target": "mac<>",
                "action": "inspect",
                "label": "UI & Smoke",
                "completed_at": "2026-06-12T12:00:00Z",
                "interaction_mode": "dom",
                "artifacts": {
                    "before_screenshot": "assets/before.png",
                    "screenshot": "assets/after & final.png",
                    "diff_screenshot": None,
                    "image_change": {"changed": False},
                },
            }
        )

        self.assertIn("mac&lt;&gt;/inspect", card)
        self.assertIn("UI &amp; Smoke", card)
        self.assertIn("interaction: dom", card)
        self.assertIn("&quot;changed&quot;: false", card)
        self.assertIn("assets/before.png", card)
        self.assertIn("assets/after &amp; final.png", card)
        self.assertNotIn("<figcaption>diff</figcaption>", card)

    def test_index_html_wraps_cards_and_escapes_label(self):
        html = self.mod.desktop_publish_index_html(
            {
                "generated_at": "2026-06-12T12:00:00Z",
                "label": "Gallery <One>",
                "runs": [
                    {
                        "target": "windows",
                        "action": "smoke",
                        "label": "Run 1",
                        "completed_at": None,
                        "interaction_mode": None,
                        "artifacts": {"screenshot": "assets/window.png"},
                    }
                ],
            }
        )

        self.assertTrue(html.startswith("<!doctype html>"))
        self.assertIn("Gallery &lt;One&gt;", html)
        self.assertIn("&middot; runs: 1", html)
        self.assertIn("windows/smoke", html)
        self.assertIn("assets/window.png", html)


if __name__ == "__main__":
    unittest.main()
