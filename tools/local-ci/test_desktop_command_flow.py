#!/usr/bin/env python3
from __future__ import annotations

import json
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_command_flow.py")


class DesktopCommandFlowTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.printed: list[str] = []

    def print_line(self, line: str) -> None:
        self.printed.append(line)

    def test_load_config_success_and_missing_config(self) -> None:
        config, status = self.mod.load_desktop_command_config(
            load_config_fn=lambda: {"desktop_automation": {}},
            print_fn=self.print_line,
        )
        self.assertEqual(config, {"desktop_automation": {}})
        self.assertIsNone(status)

        config, status = self.mod.load_desktop_command_config(
            load_config_fn=lambda: (_ for _ in ()).throw(FileNotFoundError("missing config")),
            print_fn=self.print_line,
        )
        self.assertIsNone(config)
        self.assertEqual(status, 1)
        self.assertEqual(self.printed[-1], "Error: missing config")

    def test_load_target_context_success_and_errors(self) -> None:
        config, target, status = self.mod.load_desktop_target_command_context(
            "mac",
            load_config_fn=lambda: {"desktop_automation": {}},
            resolve_desktop_target_fn=lambda _config, name: {"name": name},
            print_fn=self.print_line,
        )
        self.assertEqual(config, {"desktop_automation": {}})
        self.assertEqual(target, {"name": "mac"})
        self.assertIsNone(status)

        config, target, status = self.mod.load_desktop_target_command_context(
            "missing",
            load_config_fn=lambda: {"desktop_automation": {}},
            resolve_desktop_target_fn=lambda *_args: (_ for _ in ()).throw(ValueError("unknown target")),
            print_fn=self.print_line,
        )
        self.assertIsNone(config)
        self.assertIsNone(target)
        self.assertEqual(status, 1)
        self.assertEqual(self.printed[-1], "Error: unknown target")

    def test_run_step_success_and_error_prefix(self) -> None:
        payload, status = self.mod.run_desktop_command_step(lambda: {"ok": True}, print_fn=self.print_line)
        self.assertEqual(payload, {"ok": True})
        self.assertIsNone(status)

        payload, status = self.mod.run_desktop_command_step(
            lambda: (_ for _ in ()).throw(ValueError("unknown target")),
            print_fn=self.print_line,
            error_prefix="\nError: ",
        )
        self.assertIsNone(payload)
        self.assertEqual(status, 1)
        self.assertEqual(self.printed[-1], "\nError: unknown target")

        with self.assertRaises(RuntimeError):
            self.mod.run_desktop_command_step(
                lambda: (_ for _ in ()).throw(RuntimeError("bug")),
                print_fn=self.print_line,
                handled_exceptions=(ValueError,),
            )

    def test_emit_result_json_and_text(self) -> None:
        status = self.mod.emit_desktop_command_result(
            payload={"runs": [{"label": "run"}]},
            json_output=True,
            text_lines=["unused"],
            print_fn=self.print_line,
        )
        self.assertEqual(status, 0)
        self.assertEqual(json.loads(self.printed[-1])["runs"][0]["label"], "run")

        self.printed.clear()
        status = self.mod.emit_desktop_command_result(
            payload={"unused": True},
            json_output=False,
            text_lines=["line 1", "line 2"],
            print_fn=self.print_line,
        )
        self.assertEqual(status, 0)
        self.assertEqual(self.printed, ["line 1", "line 2"])

    def test_require_manifests_prints_empty_line(self) -> None:
        self.assertTrue(
            self.mod.require_desktop_run_manifests(
                [{"label": "run"}],
                empty_line="none",
                print_fn=self.print_line,
            )
        )
        self.assertEqual(self.printed, [])

        self.assertFalse(
            self.mod.require_desktop_run_manifests(
                [],
                empty_line="No desktop automation runs found.",
                print_fn=self.print_line,
            )
        )
        self.assertEqual(self.printed, ["No desktop automation runs found."])


if __name__ == "__main__":
    unittest.main()
