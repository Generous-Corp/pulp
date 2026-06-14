#!/usr/bin/env python3
from __future__ import annotations

from argparse import Namespace
import json
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_action_command_flow.py")


class DesktopActionCommandFlowTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.printed: list[str] = []
        self.args = Namespace(target="mac", launch_command="app")

    def print_line(self, line: str) -> None:
        self.printed.append(line)

    def test_load_context_success_and_config_errors(self) -> None:
        config, target, source, status = self.mod.load_desktop_action_command_context(
            self.args,
            load_config_fn=lambda: {"targets": {}},
            resolve_desktop_target_fn=lambda _config, name: {"name": name},
            make_desktop_source_request_fn=lambda args: {"command": args.launch_command},
            print_fn=self.print_line,
        )

        self.assertIsNone(status)
        self.assertEqual(config, {"targets": {}})
        self.assertEqual(target, {"name": "mac"})
        self.assertEqual(source, {"command": "app"})

        config, target, source, status = self.mod.load_desktop_action_command_context(
            self.args,
            load_config_fn=lambda: (_ for _ in ()).throw(ValueError("bad config")),
            resolve_desktop_target_fn=lambda *_args: {},
            make_desktop_source_request_fn=lambda _args: {},
            print_fn=self.print_line,
        )

        self.assertIsNone(config)
        self.assertIsNone(target)
        self.assertIsNone(source)
        self.assertEqual(status, 1)
        self.assertEqual(self.printed[-1], "Error: bad config")

    def test_runner_exception_handling(self) -> None:
        manifest, status = self.mod.run_desktop_action_command_runner(lambda: {"label": "preview"}, print_fn=self.print_line)
        self.assertEqual(manifest, {"label": "preview"})
        self.assertIsNone(status)

        manifest, status = self.mod.run_desktop_action_command_runner(
            lambda: (_ for _ in ()).throw(RuntimeError("boom")),
            print_fn=self.print_line,
        )
        self.assertIsNone(manifest)
        self.assertEqual(status, 1)
        self.assertEqual(self.printed[-1], "Error: boom")

    def test_emit_result_json_and_text(self) -> None:
        manifest = {"label": "preview"}
        status = self.mod.emit_desktop_action_command_result(
            action_name="smoke",
            target_name="mac",
            manifest=manifest,
            json_output=True,
            desktop_action_success_lines_fn=lambda *_args: ["unused"],
            print_fn=self.print_line,
        )
        self.assertEqual(status, 0)
        self.assertEqual(json.loads(self.printed[-1]), manifest)

        self.printed.clear()
        status = self.mod.emit_desktop_action_command_result(
            action_name="click",
            target_name="mac",
            manifest=manifest,
            json_output=False,
            desktop_action_success_lines_fn=lambda action, target, payload: [f"{action}:{target}:{payload['label']}"],
            print_fn=self.print_line,
        )
        self.assertEqual(status, 0)
        self.assertEqual(self.printed, ["click:mac:preview"])


if __name__ == "__main__":
    unittest.main()
