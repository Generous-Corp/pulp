#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
from argparse import Namespace
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).resolve().with_name("desktop_action_commands_cli.py")


def load_desktop_action_commands_cli_module():
    spec = importlib.util.spec_from_file_location("desktop_action_commands_cli_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class DesktopActionCommandsCliTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_desktop_action_commands_cli_module()
        self.printed: list[str] = []
        self.calls: list[tuple[str, tuple, dict]] = []
        self.adapters = {
            "mac": "macos-local",
            "ubuntu": "linux-xvfb",
            "windows": "windows-session-agent",
            "other": "remote-session-agent",
        }

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
            "video_audio": "none",
            "video_audio_device": None,
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

    def deps(self, *, manifest: dict | None = None):
        manifest = manifest or {"label": "preview", "artifacts": {"screenshot": "/tmp/window.png"}}

        def load_config():
            return {"desktop_automation": {"targets": {}}}

        def resolve_desktop_target(_config, name):
            return {"adapter": self.adapters[name], "target_type": "local" if name == "mac" else "ssh"}

        def make_desktop_source_request(args):
            return {"target": args.target, "command": args.launch_command}

        def runner(name):
            def _runner(*args, **kwargs):
                self.calls.append((name, args, kwargs))
                return manifest

            return _runner

        return {
            "load_config_fn": load_config,
            "resolve_desktop_target_fn": resolve_desktop_target,
            "make_desktop_source_request_fn": make_desktop_source_request,
            "run_macos_local_smoke_fn": runner("macos"),
            "run_linux_xvfb_remote_action_fn": runner("linux"),
            "run_windows_session_agent_action_fn": runner("windows"),
            "desktop_action_success_lines_fn": lambda action, target, payload: [f"{action}:{target}:{payload['label']}"],
            "sys_platform": "darwin",
            "print_fn": self.print_line,
        }

    def test_windows_selector_helper_detects_view_selectors_only(self):
        self.assertFalse(self.mod.windows_requires_pulp_app_selectors(self.args(click="10,20")))
        self.assertTrue(self.mod.windows_requires_pulp_app_selectors(self.args(click_view_id="root")))
        self.assertTrue(self.mod.windows_requires_pulp_app_selectors(self.args(click_view_label="Bypass")))

    def test_smoke_dispatches_macos_text_and_preserves_options(self):
        result = self.mod.cmd_desktop_smoke(
            self.args(capture_ui_snapshot=True, click_view_text="Bypass", capture_before=True, pulp_app_automation=True),
            **self.deps(),
        )

        self.assertEqual(result, 0)
        self.assertEqual(self.printed, ["smoke:mac:preview"])
        self.assertEqual(self.calls[0][0], "macos")
        self.assertEqual(self.calls[0][1][1], "app")
        self.assertEqual(self.calls[0][2]["action_name"], "smoke")
        self.assertTrue(self.calls[0][2]["capture_ui_snapshot"])
        self.assertEqual(self.calls[0][2]["click_view_text"], "Bypass")
        self.assertTrue(self.calls[0][2]["capture_before"])
        self.assertFalse(self.calls[0][2]["record_video"])
        self.assertEqual(self.calls[0][2]["source_request"], {"target": "mac", "command": "app"})

    def test_smoke_forwards_capture_bundle_id(self):
        result = self.mod.cmd_desktop_smoke(
            self.args(
                capture_bundle_id="com.cockos.reaper",
                record_video=True,
                video_note=["PulpSynth inserted"],
                recipe="reaper-plugin-editor",
                host_app="REAPER",
                plugin="PulpSynth",
                plugin_format="clap",
            ),
            **self.deps(),
        )

        self.assertEqual(result, 0)
        self.assertEqual(self.calls[0][2]["capture_bundle_id"], "com.cockos.reaper")
        self.assertEqual(self.calls[0][2]["video_capture_target"], "app")
        self.assertEqual(self.calls[0][2]["video_notes"], ["PulpSynth inserted"])
        self.assertEqual(
            self.calls[0][2]["video_context"],
            {
                "recipe": "reaper-plugin-editor",
                "host": "REAPER",
                "plugin": "PulpSynth",
                "format": "clap",
                "capture_bundle_id": "com.cockos.reaper",
                "launch": "command",
            },
        )

    def test_smoke_json_and_error_paths(self):
        result = self.mod.cmd_desktop_smoke(
            self.args(json=True),
            **self.deps(manifest={"label": "json-run", "artifacts": {"screenshot": "/tmp/json.png"}}),
        )
        self.assertEqual(result, 0)
        self.assertEqual(json.loads(self.printed[-1])["label"], "json-run")

        self.printed.clear()
        deps = self.deps()
        deps["run_macos_local_smoke_fn"] = lambda *_args, **_kwargs: (_ for _ in ()).throw(RuntimeError("boom"))
        result = self.mod.cmd_desktop_smoke(self.args(), **deps)
        self.assertEqual(result, 1)
        self.assertEqual(self.printed, ["Error: boom"])

    def test_adapter_validation_errors_match_existing_commands(self):
        result = self.mod.cmd_desktop_smoke(self.args(target="ubuntu", launch_command=None), **self.deps())
        self.assertEqual(result, 1)
        self.assertIn("requires --command for linux-xvfb", self.printed[-1])

        self.printed.clear()
        result = self.mod.cmd_desktop_smoke(self.args(target="ubuntu", record_video=True), **self.deps())
        self.assertEqual(result, 1)
        self.assertIn("video recording is not implemented for linux-xvfb", self.printed[-1])

        result = self.mod.cmd_desktop_smoke(
            self.args(target="windows", capture_ui_snapshot=True, pulp_app_automation=False),
            **self.deps(),
        )
        self.assertEqual(result, 1)
        self.assertIn("supports --capture-ui-snapshot only with --pulp-app-automation", self.printed[-1])

        deps = self.deps()
        deps["sys_platform"] = "linux"
        result = self.mod.cmd_desktop_smoke(self.args(target="mac"), **deps)
        self.assertEqual(result, 1)
        self.assertIn("must run on macOS", self.printed[-1])

    def test_click_dispatches_after_selector_validation(self):
        result = self.mod.cmd_desktop_click(self.args(click_view_id="root", capture_ui_snapshot=True), **self.deps())

        self.assertEqual(result, 0)
        self.assertEqual(self.printed, ["click:mac:preview"])
        self.assertEqual(self.calls[0][0], "macos")
        self.assertEqual(self.calls[0][2]["action_name"], "click")
        self.assertTrue(self.calls[0][2]["capture_before"])
        self.assertEqual(self.calls[0][2]["click_view_id"], "root")

        self.printed.clear()
        self.calls.clear()
        result = self.mod.cmd_desktop_click(self.args(click=None), **self.deps())
        self.assertEqual(result, 1)
        self.assertEqual(self.calls, [])
        self.assertIn("requires --click or one view-target selector", self.printed[-1])

    def test_click_windows_rejects_generic_view_selector_but_allows_point(self):
        result = self.mod.cmd_desktop_click(
            self.args(target="windows", click_view_id="root", pulp_app_automation=False),
            **self.deps(),
        )
        self.assertEqual(result, 1)
        self.assertIn("supports view-target selectors only with --pulp-app-automation", self.printed[-1])

        self.printed.clear()
        result = self.mod.cmd_desktop_click(
            self.args(target="windows", click="10,20", pulp_app_automation=False),
            **self.deps(),
        )
        self.assertEqual(result, 0)
        self.assertEqual(self.calls[-1][0], "windows")
        self.assertFalse(self.calls[-1][2]["pulp_app_automation"])
        self.assertEqual(self.calls[-1][2]["click_point"], "10,20")

        self.printed.clear()
        result = self.mod.cmd_desktop_click(
            self.args(target="windows", click="10,20", record_video=True),
            **self.deps(),
        )
        self.assertEqual(result, 1)
        self.assertIn("video recording is not implemented for windows-session-agent", self.printed[-1])

    def test_inspect_dispatches_adapter_specific_snapshot_policy(self):
        result = self.mod.cmd_desktop_inspect(self.args(bundle_id="com.example.App", launch_command=None), **self.deps())
        self.assertEqual(result, 0)
        self.assertEqual(self.calls[0][0], "macos")
        self.assertEqual(self.calls[0][2]["action_name"], "inspect")
        self.assertFalse(self.calls[0][2]["capture_ui_snapshot"])
        self.assertFalse(self.calls[0][2]["capture_before"])
        self.assertEqual(self.calls[0][2]["settle_secs"], 0.0)

        self.calls.clear()
        result = self.mod.cmd_desktop_inspect(
            self.args(target="ubuntu", pulp_app_automation=True),
            **self.deps(),
        )
        self.assertEqual(result, 0)
        self.assertEqual(self.calls[0][0], "linux")
        self.assertTrue(self.calls[0][2]["capture_ui_snapshot"])

        self.calls.clear()
        result = self.mod.cmd_desktop_inspect(
            self.args(target="windows", pulp_app_automation=False),
            **self.deps(),
        )
        self.assertEqual(result, 0)
        self.assertEqual(self.calls[0][0], "windows")
        self.assertFalse(self.calls[0][2]["capture_ui_snapshot"])

    def test_inspect_reports_launch_mode_and_adapter_errors(self):
        result = self.mod.cmd_desktop_inspect(self.args(launch_command=None, bundle_id=None), **self.deps())
        self.assertEqual(result, 1)
        self.assertIn("requires exactly one of --command or --bundle-id", self.printed[-1])

        result = self.mod.cmd_desktop_inspect(self.args(target="other"), **self.deps())
        self.assertEqual(result, 1)
        self.assertIn("desktop inspect is not implemented", self.printed[-1])

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

    def test_video_command_dispatches_action_and_rejects_plugin_audio_mode(self):
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
        self.assertIn("video audio source `plugin` is not implemented yet", self.printed[-1])

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
        self.assertEqual(call["label"], "component-threshold-proof")
        self.assertEqual(call["video_title"], "Component validation")

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

    def test_record_video_rejects_reserved_audio_modes(self):
        result = self.mod.cmd_desktop_click(
            self.args(click_view_id="root", record_video=True, video_audio="plugin"),
            **self.deps(),
        )
        self.assertEqual(result, 1)
        self.assertIn("video audio source `plugin` is not implemented yet", self.printed[-1])


if __name__ == "__main__":
    unittest.main()
