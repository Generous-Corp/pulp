#!/usr/bin/env python3
from __future__ import annotations

import json
import tempfile
from pathlib import Path
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("reporting_publish_html.py")


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

    def test_run_card_renders_video_notes_focus_and_storyboard(self):
        with tempfile.TemporaryDirectory() as tmp:
            publish_dir = Path(tmp)
            metadata_rel = "assets/run/composed.json"
            metadata_path = publish_dir / metadata_rel
            metadata_path.parent.mkdir(parents=True, exist_ok=True)
            metadata_path.write_text(
                json.dumps(
                    {
                        "review_storyboard": {
                            "title": "Bypass toggle",
                            "subtitle": "click flips the LED",
                            "steps": [
                                {"label": "Locate Bypass", "detail": "top-right toggle"},
                                {"label": "Click", "detail": "LED turns amber"},
                            ],
                        }
                    }
                )
            )
            card = self.mod.desktop_publish_run_card_html(
                {
                    "target": "mac",
                    "action": "click",
                    "label": "video proof",
                    "completed_at": "2026-06-16T12:00:00Z",
                    "interaction_mode": "desktop-event",
                    "video_proof_notes": ["toggle flips the bypass LED"],
                    "video_proof_composition": {
                        "template": "component-zoom",
                        "focus": {"label": "Bypass"},
                        "action_marker": {"kind": "click", "label": "tap Bypass"},
                    },
                    "artifacts": {
                        "video_composed": "assets/run/proof.mp4",
                        "video_composed_metadata": metadata_rel,
                        "video_poster": "assets/run/poster.png",
                    },
                },
                publish_dir=publish_dir,
            )

        self.assertIn("<video", card)
        self.assertIn("assets/run/proof.mp4", card)
        self.assertIn("poster=\"assets/run/poster.png\"", card)
        self.assertIn("interaction: desktop-event", card)
        self.assertIn("template: component-zoom", card)
        self.assertIn("focus: Bypass", card)
        self.assertIn("action: tap Bypass", card)
        self.assertIn("toggle flips the bypass LED", card)
        self.assertIn("Bypass toggle", card)
        self.assertIn("<ol class=\"timeline\">", card)
        self.assertIn("Locate Bypass", card)
        self.assertIn("LED turns amber", card)

    def test_storyboard_skipped_without_publish_dir(self):
        card = self.mod.desktop_publish_run_card_html(
            {
                "target": "mac",
                "action": "click",
                "label": "no dir",
                "artifacts": {"video_composed_metadata": "assets/run/composed.json"},
            }
        )
        self.assertNotIn("timeline", card)


if __name__ == "__main__":
    unittest.main()
