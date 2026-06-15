#!/usr/bin/env python3
from __future__ import annotations

import json
from argparse import Namespace
from pathlib import Path
import unittest

from module_test_utils import load_local_ci_module



def load_desktop_config_commands_cli_module():
    return load_local_ci_module("desktop_config_commands_cli.py")


class DesktopConfigCommandsCliTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_desktop_config_commands_cli_module()
        self.printed: list[str] = []
        self.saved_configs: list[dict] = []

    def print_line(self, line: str):
        self.printed.append(line)

    def desktop_config(self):
        return {
            "desktop_automation": {
                "artifact_root": "runs",
                "publish_mode": "local",
                "publish_branch": "desktop-artifacts",
                "retention_days": 7,
                "targets": {
                    "mac": {
                        "enabled": True,
                        "adapter": "macos-local",
                        "bootstrap": "launchagent",
                        "target_type": "local",
                        "capability_tier": "full",
                        "optional": {"webview_driver": True},
                    }
                },
            }
        }

    def test_desktop_config_show_text_and_json(self):
        result = self.mod.cmd_desktop_config_show(
            Namespace(json=False),
            load_config_fn=self.desktop_config,
            desktop_config_show_lines_fn=lambda _cfg: ["Desktop automation config:"],
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        self.assertEqual(self.printed, ["Desktop automation config:"])

        self.printed.clear()
        result = self.mod.cmd_desktop_config_show(
            Namespace(json=True),
            load_config_fn=self.desktop_config,
            desktop_config_show_lines_fn=lambda _cfg: ["unused"],
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        self.assertEqual(json.loads(self.printed[0])["publish_mode"], "local")

    def test_desktop_config_set_updates_target_optional_field(self):
        config = self.desktop_config()
        result = self.mod.cmd_desktop_config_set(
            Namespace(key="target.mac.webview_driver", value="false", json=False),
            load_config_fn=lambda: config,
            save_config_fn=self.saved_configs.append,
            config_path_fn=lambda: Path("/tmp/config.json"),
            normalize_publish_mode_fn=lambda value: value,
            parse_config_bool_fn=lambda value: value.lower() == "true",
            normalize_desktop_config_fn=lambda payload: payload,
            desktop_config_update_lines_fn=lambda payload: [f"{payload['key']} = {payload['value']}"],
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        self.assertFalse(self.saved_configs[0]["desktop_automation"]["targets"]["mac"]["optional"]["webview_driver"])
        self.assertEqual(self.printed[-1], "target.mac.webview_driver = False")

    def test_desktop_config_set_reports_validation_errors(self):
        result = self.mod.cmd_desktop_config_set(
            Namespace(key="retention_days", value="-1", json=False),
            load_config_fn=lambda: {"desktop_automation": {}},
            save_config_fn=self.saved_configs.append,
            config_path_fn=lambda: Path("/tmp/config.json"),
            normalize_publish_mode_fn=lambda value: value,
            parse_config_bool_fn=lambda value: value == "true",
            normalize_desktop_config_fn=lambda payload: payload,
            desktop_config_update_lines_fn=lambda payload: [str(payload)],
            print_fn=self.print_line,
        )

        self.assertEqual(result, 1)
        self.assertEqual(self.printed, ["Error: retention_days must be >= 0"])
        self.assertEqual(self.saved_configs, [])

    def test_desktop_config_dispatch_requires_subcommand(self):
        calls = []
        result = self.mod.cmd_desktop_config(
            Namespace(desktop_config_command="show"),
            commands={"show": lambda args: calls.append(args) or 7},
            print_fn=self.print_line,
        )
        self.assertEqual(result, 7)
        self.assertEqual(len(calls), 1)

        result = self.mod.cmd_desktop_config(
            Namespace(desktop_config_command=None),
            commands={},
            print_fn=self.print_line,
        )
        self.assertEqual(result, 1)
        self.assertEqual(self.printed[-1], "Error: desktop config subcommand required (show, set)")


if __name__ == "__main__":
    unittest.main()
