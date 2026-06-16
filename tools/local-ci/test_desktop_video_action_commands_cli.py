#!/usr/bin/env python3
"""Tests for the desktop `video` action command (recipe + record + dispatch)."""

from __future__ import annotations

import json
from argparse import Namespace
from pathlib import Path
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("desktop_video_action_commands_cli.py", add_module_dir=True)


class DesktopVideoActionCommandsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.printed: list[str] = []
        self.calls: list[tuple] = []

    def print_line(self, line: str):
        self.printed.append(line)

    def args(self, **overrides):
        values = {
            "target": "mac",
            "launch_command": "app",
            "bundle_id": None,
            "label": "preview",
            "output": "/tmp/window.png",
            "capture_ui_snapshot": False,
            "click": None,
            "click_view_id": None,
            "click_view_type": None,
            "click_view_text": None,
            "click_view_label": None,
            "capture_before": False,
            "settle_secs": 0.5,
            "timeout": 5.0,
            "pulp_app_automation": False,
            "record_video": False,
            "video_duration": 8.0,
            "video_fps": 30.0,
            "video_attachment_budget_mb": 100.0,
            "small_video": False,
            "small_video_budget_mb": 10.0,
            "video_audio": "none",
            "video_audio_file": None,
            "video_audio_device": None,
            "video_recorder": "auto",
            "video_capture_target": "app",
            "capture_bundle_id": None,
            "compose_video_proof": False,
            "video_template": None,
            "source_image": None,
            "source_label": None,
            "video_title": None,
            "video_note": [],
            "recipe": None,
            "plugin": None,
            "plugin_format": None,
            "host_app": None,
            "component_id": None,
            "action": "click",
            "json": False,
        }
        values.update(overrides)
        return Namespace(**values)


    def test_video_command_forces_recording_and_composition(self):
        result = self.mod.cmd_desktop_video(
            self.args(click_view_id="root", label="video-proof"),
            cmd_desktop_smoke_fn=lambda _args: self.fail("smoke should not run"),
            cmd_desktop_click_fn=lambda args: self.calls.append(("click-wrapper", (), vars(args).copy())) or 0,
            cmd_desktop_inspect_fn=lambda _args: self.fail("inspect should not run"),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 0)
        self.assertEqual(self.calls[0][0], "click-wrapper")
        self.assertTrue(self.calls[0][2]["record_video"])
        self.assertTrue(self.calls[0][2]["compose_video_proof"])

    def test_video_command_dispatches_action_and_validates_plugin_audio_mode(self):
        result = self.mod.cmd_desktop_video(
            self.args(action="inspect"),
            cmd_desktop_smoke_fn=lambda _args: self.fail("smoke should not run"),
            cmd_desktop_click_fn=lambda _args: self.fail("click should not run"),
            cmd_desktop_inspect_fn=lambda args: self.calls.append(("inspect-wrapper", (), vars(args).copy())) or 0,
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        self.assertEqual(self.calls[0][0], "inspect-wrapper")

        self.calls.clear()
        result = self.mod.cmd_desktop_video(
            self.args(action="smoke", video_audio="system", video_audio_device="2"),
            cmd_desktop_smoke_fn=lambda args: self.calls.append(("smoke-wrapper", (), vars(args).copy())) or 0,
            cmd_desktop_click_fn=lambda _args: self.fail("click should not run"),
            cmd_desktop_inspect_fn=lambda _args: self.fail("inspect should not run"),
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        self.assertEqual(self.calls[0][2]["video_audio"], "system")
        self.assertEqual(self.calls[0][2]["video_audio_device"], "2")

        self.printed.clear()
        result = self.mod.cmd_desktop_video(
            self.args(video_audio="plugin"),
            cmd_desktop_smoke_fn=lambda _args: self.fail("smoke should not run"),
            cmd_desktop_click_fn=lambda _args: self.fail("click should not run"),
            cmd_desktop_inspect_fn=lambda _args: self.fail("inspect should not run"),
            print_fn=self.print_line,
        )
        self.assertEqual(result, 1)
        self.assertIn("--video-audio plugin requires --video-audio-file", self.printed[-1])

        self.printed.clear()
        self.calls.clear()
        result = self.mod.cmd_desktop_video(
            self.args(action="smoke", video_audio="plugin", video_audio_file="/tmp/plugin.wav"),
            cmd_desktop_smoke_fn=lambda args: self.calls.append(("smoke-wrapper", (), vars(args).copy())) or 0,
            cmd_desktop_click_fn=lambda _args: self.fail("click should not run"),
            cmd_desktop_inspect_fn=lambda _args: self.fail("inspect should not run"),
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        self.assertEqual(self.calls[0][2]["video_audio"], "plugin")
        self.assertEqual(self.calls[0][2]["video_audio_file"], "/tmp/plugin.wav")

        self.printed.clear()
        self.calls.clear()
        result = self.mod.cmd_desktop_video(
            self.args(action="smoke", small_video=True, small_video_budget_mb=8.0),
            cmd_desktop_smoke_fn=lambda args: self.calls.append(("smoke-wrapper", (), vars(args).copy())) or 0,
            cmd_desktop_click_fn=lambda _args: self.fail("click should not run"),
            cmd_desktop_inspect_fn=lambda _args: self.fail("inspect should not run"),
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        self.assertTrue(self.calls[0][2]["small_video"])
        self.assertEqual(self.calls[0][2]["small_video_budget_mb"], 8.0)

    def test_video_command_applies_component_recipe(self):
        result = self.mod.cmd_desktop_video(
            self.args(recipe="component-zoom", component_id="threshold", label=None),
            cmd_desktop_smoke_fn=lambda _args: self.fail("smoke should not run"),
            cmd_desktop_click_fn=lambda args: self.calls.append(("click-wrapper", (), vars(args).copy())) or 0,
            cmd_desktop_inspect_fn=lambda _args: self.fail("inspect should not run"),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 0)
        call = self.calls[0][2]
        self.assertEqual(call["click_view_id"], "threshold")
        self.assertTrue(call["capture_ui_snapshot"])
        self.assertTrue(call["capture_before"])
        self.assertEqual(call["video_template"], "component-zoom")
        self.assertEqual(call["video_duration"], 6.0)
        self.assertEqual(call["video_fps"], 8.0)
        self.assertEqual(call["video_recorder"], "frame-sequence")
        self.assertEqual(call["label"], "component-threshold-proof")
        self.assertEqual(call["video_title"], "Component validation")
        self.assertIn("full-window context", call["video_note"][0])
        self.assertIn("focus box", call["video_note"][1])

    def test_video_command_preserves_explicit_component_capture_settings(self):
        result = self.mod.cmd_desktop_video(
            self.args(
                recipe="component-zoom",
                component_id="threshold",
                video_duration=3.0,
                video_fps=12.0,
                video_recorder="avfoundation",
            ),
            cmd_desktop_smoke_fn=lambda _args: self.fail("smoke should not run"),
            cmd_desktop_click_fn=lambda args: self.calls.append(("click-wrapper", (), vars(args).copy())) or 0,
            cmd_desktop_inspect_fn=lambda _args: self.fail("inspect should not run"),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 0)
        call = self.calls[0][2]
        self.assertEqual(call["video_duration"], 3.0)
        self.assertEqual(call["video_fps"], 12.0)
        self.assertEqual(call["video_recorder"], "avfoundation")

    def test_video_command_applies_audio_inspector_recipe(self):
        result = self.mod.cmd_desktop_video(
            self.args(recipe="audio-inspector-demo", label=None),
            cmd_desktop_smoke_fn=lambda args: self.calls.append(("smoke-wrapper", (), vars(args).copy())) or 0,
            cmd_desktop_click_fn=lambda _args: self.fail("click should not run"),
            cmd_desktop_inspect_fn=lambda _args: self.fail("inspect should not run"),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 0)
        call = self.calls[0][2]
        self.assertEqual(self.calls[0][0], "smoke-wrapper")
        self.assertEqual(call["action"], "smoke")
        self.assertFalse(call["capture_ui_snapshot"])
        self.assertEqual(call["label"], "audio-inspector-demo-proof")
        self.assertEqual(call["video_title"], "Standalone Audio Inspector Demo")
        self.assertIn("audio-inspector surface", call["video_note"][0])
        self.assertIn("storyboard", call["video_note"][1])

    def test_video_command_applies_inspector_workflow_recipe(self):
        result = self.mod.cmd_desktop_video(
            self.args(recipe="inspector-workflow", label=None, capture_ui_snapshot=True, action="inspect"),
            cmd_desktop_smoke_fn=lambda args: self.calls.append(("smoke-wrapper", (), vars(args).copy())) or 0,
            cmd_desktop_click_fn=lambda _args: self.fail("click should not run"),
            cmd_desktop_inspect_fn=lambda _args: self.fail("inspect should not run"),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 0)
        call = self.calls[0][2]
        self.assertEqual(self.calls[0][0], "smoke-wrapper")
        self.assertEqual(call["action"], "smoke")
        self.assertFalse(call["capture_ui_snapshot"])
        self.assertEqual(call["label"], "inspector-workflow-proof")
        self.assertEqual(call["video_title"], "Inspector workflow proof")
        self.assertEqual(call["video_template"], "inspector-workflow")
        self.assertIn("inspector pane", call["video_note"][0])

    def test_video_command_applies_reaper_recipe(self):
        result = self.mod.cmd_desktop_video(
            self.args(recipe="reaper-plugin-editor", launch_command=None, label=None, plugin="PulpEffect", plugin_format="vst3"),
            cmd_desktop_smoke_fn=lambda args: self.calls.append(("smoke-wrapper", (), vars(args).copy())) or 0,
            cmd_desktop_click_fn=lambda args: self.calls.append(("click-wrapper", (), vars(args).copy())) or 0,
            cmd_desktop_inspect_fn=lambda _args: self.fail("inspect should not run"),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 0)
        self.assertEqual(self.calls[0][0], "smoke-wrapper")
        call = self.calls[0][2]
        self.assertEqual(call["action"], "smoke")
        self.assertIsNone(call["bundle_id"])
        self.assertTrue(call["launch_command"].endswith("run-reaper-proof.zsh"))
        self.assertEqual(call["capture_bundle_id"], "com.cockos.reaper")
        self.assertEqual(call["host_app"], "REAPER")
        self.assertEqual(call["label"], "reaper-vst3-PulpEffect-proof")
        self.assertEqual(call["video_title"], "PulpEffect VST3 editor in REAPER")
        self.assertEqual(call["video_template"], "plugin-host")
        self.assertIn("REAPER launched from a generated wrapper", call["video_note"][0])
        self.assertIn("REAPER host chrome", call["video_note"][1])
        self.assertIn("blank project window", call["video_note"][2])
        self.assertEqual(call["reaper_recipe_files"]["command"], call["launch_command"])
        self.assertTrue(Path(call["reaper_recipe_files"]["lua_script"]).exists())

    def test_video_command_reaper_recipe_rejects_view_selectors(self):
        result = self.mod.cmd_desktop_video(
            self.args(
                recipe="reaper-plugin-editor",
                launch_command=None,
                label=None,
                plugin="PulpEffect",
                plugin_format="vst3",
                click_view_id="drive-knob",
            ),
            cmd_desktop_smoke_fn=lambda _args: self.fail("smoke should not run"),
            cmd_desktop_click_fn=lambda _args: self.fail("click should not run"),
            cmd_desktop_inspect_fn=lambda _args: self.fail("inspect should not run"),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 1)
        self.assertIn("use --click X,Y instead of ViewInspector selectors", self.printed[-1])

    def test_video_command_reaper_recipe_reports_missing_clap_bundle(self):
        result = self.mod.cmd_desktop_video(
            self.args(
                recipe="reaper-plugin-editor",
                launch_command=None,
                label=None,
                plugin="DefinitelyMissingPulpPlugin",
                plugin_format="clap",
            ),
            cmd_desktop_smoke_fn=lambda _args: self.fail("smoke should not run"),
            cmd_desktop_click_fn=lambda _args: self.fail("click should not run"),
            cmd_desktop_inspect_fn=lambda _args: self.fail("inspect should not run"),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 1)
        self.assertIn("requires an installed DefinitelyMissingPulpPlugin CLAP bundle", self.printed[-1])
        self.assertIn("~/Library/Audio/Plug-Ins/CLAP", self.printed[-1])

    def test_video_command_reaper_recipe_reports_stale_clap_cache(self):
        self.mod.reaper_video_recipe.installed_clap_bundle_status = lambda _plugin: (True, "installed")
        self.mod.reaper_video_recipe.reaper_clap_cache_status = lambda _plugin: (False, "cache stanza exists but no plugin descriptor")

        result = self.mod.cmd_desktop_video(
            self.args(
                recipe="reaper-plugin-editor",
                launch_command=None,
                label=None,
                plugin="PulpSynth",
                plugin_format="clap",
            ),
            cmd_desktop_smoke_fn=lambda _args: self.fail("smoke should not run"),
            cmd_desktop_click_fn=lambda _args: self.fail("click should not run"),
            cmd_desktop_inspect_fn=lambda _args: self.fail("inspect should not run"),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 1)
        self.assertIn("requires REAPER to have a valid PulpSynth CLAP cache entry", self.printed[-1])
        self.assertIn("rescan", self.printed[-1])

    def test_video_command_reaper_recipe_keeps_explicit_command(self):
        result = self.mod.cmd_desktop_video(
            self.args(
                recipe="reaper-plugin-editor",
                launch_command="/Applications/REAPER.app/Contents/MacOS/REAPER -new script.lua",
                label=None,
                plugin="PulpSynth",
                plugin_format="clap",
            ),
            cmd_desktop_smoke_fn=lambda args: self.calls.append(("smoke-wrapper", (), vars(args).copy())) or 0,
            cmd_desktop_click_fn=lambda args: self.calls.append(("click-wrapper", (), vars(args).copy())) or 0,
            cmd_desktop_inspect_fn=lambda _args: self.fail("inspect should not run"),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 0)
        self.assertEqual(self.calls[0][0], "smoke-wrapper")
        call = self.calls[0][2]
        self.assertEqual(call["action"], "smoke")
        self.assertEqual(call["launch_command"], "/Applications/REAPER.app/Contents/MacOS/REAPER -new script.lua")
        self.assertIsNone(call["bundle_id"])
        self.assertEqual(call["label"], "reaper-clap-PulpSynth-proof")
        self.assertEqual(call["video_title"], "PulpSynth CLAP editor in REAPER")
        self.assertIn("REAPER host chrome", call["video_note"][0])

    def test_video_command_validates_recipe_requirements(self):
        result = self.mod.cmd_desktop_video(
            self.args(recipe="design-parity"),
            cmd_desktop_smoke_fn=lambda _args: self.fail("smoke should not run"),
            cmd_desktop_click_fn=lambda _args: self.fail("click should not run"),
            cmd_desktop_inspect_fn=lambda _args: self.fail("inspect should not run"),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 1)
        self.assertIn("recipe `design-parity` requires --source-image", self.printed[-1])

if __name__ == "__main__":
    unittest.main()
