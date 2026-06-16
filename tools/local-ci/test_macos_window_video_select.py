#!/usr/bin/env python3
"""No-network tests for macos_window_video_select.py (video-proof re-home of test_macos_desktop.py)."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import tempfile
import time
import unittest


from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("macos_window_video_select.py", add_module_dir=True)



class MacosWindowVideoSelectTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_wait_for_bundle_window_title_filters_terminal_window(self) -> None:
        now = [0.0]
        payloads = [
            {"pid": 456, "windows": [{"title": "Other", "windowId": 1}]},
            {"pid": 456, "windows": [{"title": "Pulp Video Proof abcd1234", "windowId": 2}]},
        ]

        result = self.mod.wait_for_macos_bundle_window_title(
            "com.apple.Terminal",
            "Pulp Video Proof abcd1234",
            1.0,
            macos_window_info_for_bundle_id_fn=lambda _bundle_id: payloads.pop(0),
            activate_macos_bundle_id_fn=lambda _bundle_id: {"activated": True, "stderr": ""},
            time_fn=lambda: now[0],
            sleep_fn=lambda amount: now.__setitem__(0, now[0] + amount),
        )

        self.assertEqual(result, (456, {"title": "Pulp Video Proof abcd1234", "windowId": 2}))

    def test_wait_for_bundle_secondary_window_prefers_medium_floating_editor(self) -> None:
        now = [0.0]
        payloads = [
            {"pid": 456, "windows": [{"windowId": 1, "bounds": {"width": 1000, "height": 700}}]},
            {
                "pid": 456,
                "windows": [
                    {"windowId": 1, "bounds": {"width": 1000, "height": 700}},
                    {"windowId": 2, "bounds": {"width": 724, "height": 394}},
                    {"windowId": 3, "bounds": {"width": 724, "height": 394}},
                    {"windowId": 4, "bounds": {"width": 400, "height": 353}},
                ],
            },
        ]

        result = self.mod.wait_for_macos_bundle_secondary_window(
            "com.cockos.reaper",
            1.0,
            macos_window_info_for_bundle_id_fn=lambda _bundle_id: payloads.pop(0),
            activate_macos_bundle_id_fn=lambda _bundle_id: {"activated": True, "stderr": ""},
            time_fn=lambda: now[0],
            sleep_fn=lambda amount: now.__setitem__(0, now[0] + amount),
        )

        self.assertEqual(result, (456, {"windowId": 4, "bounds": {"width": 400, "height": 353}}))


if __name__ == "__main__":
    unittest.main()
