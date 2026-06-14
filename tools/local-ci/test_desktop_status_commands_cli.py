#!/usr/bin/env python3
from __future__ import annotations

import json
from argparse import Namespace
from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).resolve().with_name("desktop_status_commands_cli.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopStatusCommandsCliTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.printed: list[str] = []

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
                    }
                },
            }
        }

    def common_deps(self):
        return {
            "desktop_receipt_for_fn": lambda _name: {"installed_at": "then"},
            "desktop_capabilities_for_fn": lambda *_args: ["screenshot"],
            "desktop_optional_capabilities_fn": lambda _optional: [],
            "desktop_run_manifests_fn": lambda *_args, **_kwargs: [],
            "desktop_run_summary_fn": lambda _config, _manifest: {},
            "desktop_proof_summaries_fn": lambda *_args, **_kwargs: [],
            "normalize_desktop_optional_config_fn": lambda optional: dict(optional or {}),
            "desktop_target_contract_fn": lambda name, _target: {"name": name},
            "desktop_publish_reports_fn": lambda *_args, **_kwargs: [],
            "desktop_status_lines_fn": lambda _cfg, targets, **_kwargs: [f"target={targets[0]['name']}"],
            "short_sha_fn": lambda value: value[:12],
            "windows_tooling_detail_fn": lambda *_args, **_kwargs: "",
            "windows_repo_checkout_detail_fn": lambda *_args, **_kwargs: "",
            "print_fn": self.print_line,
        }

    def test_status_builds_text_and_json_payloads(self):
        result = self.mod.cmd_desktop_status(
            Namespace(target=None, json=False),
            load_config_fn=self.desktop_config,
            **self.common_deps(),
        )
        self.assertEqual(result, 0)
        self.assertEqual(self.printed, ["target=mac"])

        self.printed.clear()
        result = self.mod.cmd_desktop_status(
            Namespace(target="mac", json=True),
            load_config_fn=self.desktop_config,
            **self.common_deps(),
        )
        self.assertEqual(result, 0)
        self.assertEqual(json.loads(self.printed[0])["targets"][0]["name"], "mac")

    def test_status_reports_missing_config_and_unknown_target(self):
        result = self.mod.cmd_desktop_status(
            Namespace(target=None, json=False),
            load_config_fn=lambda: (_ for _ in ()).throw(FileNotFoundError("missing config")),
            **self.common_deps(),
        )
        self.assertEqual(result, 1)
        self.assertEqual(self.printed, ["Error: missing config"])

        self.printed.clear()
        result = self.mod.cmd_desktop_status(
            Namespace(target="windows", json=False),
            load_config_fn=self.desktop_config,
            **self.common_deps(),
        )
        self.assertEqual(result, 1)
        self.assertIn("unknown desktop target", self.printed[0])


if __name__ == "__main__":
    unittest.main()
