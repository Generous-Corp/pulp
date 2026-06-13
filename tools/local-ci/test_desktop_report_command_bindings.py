#!/usr/bin/env python3
"""Tests for desktop report command facade bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_report_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopReportCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_command_exports_are_declared(self) -> None:
        self.assertEqual(
            self.mod.DESKTOP_REPORT_COMMAND_EXPORTS,
            (
                "cmd_desktop_recent",
                "cmd_desktop_proof",
                "cmd_desktop_publish",
                "cmd_desktop_cleanup",
            ),
        )

    def bindings(self, runner_name: str, runner):
        return {
            "_desktop_commands_cli": types.SimpleNamespace(**{runner_name: runner}),
            "_desktop_cli": types.SimpleNamespace(
                desktop_recent_lines=object(),
                desktop_proof_empty_line=object(),
                desktop_proof_lines=object(),
                desktop_publish_lines=object(),
                desktop_cleanup_empty_line=object(),
                desktop_cleanup_lines=object(),
            ),
            "load_config": object(),
            "desktop_run_manifests": object(),
            "desktop_run_summary": object(),
            "desktop_proof_summaries": object(),
            "short_sha": object(),
            "stage_desktop_publish_report": object(),
            "prune_desktop_run_manifests": object(),
            "write_desktop_run_rollups": object(),
        }

    def test_recent_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 4

        bindings = self.bindings("cmd_desktop_recent", runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_desktop_recent(bindings, args_obj), 4)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["load_config_fn"], bindings["load_config"])
        self.assertIs(captured["kwargs"]["desktop_run_manifests_fn"], bindings["desktop_run_manifests"])
        self.assertIs(captured["kwargs"]["desktop_run_summary_fn"], bindings["desktop_run_summary"])
        self.assertIs(captured["kwargs"]["desktop_recent_lines_fn"], bindings["_desktop_cli"].desktop_recent_lines)
        self.assertIs(captured["kwargs"]["short_sha_fn"], bindings["short_sha"])

    def test_proof_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 6

        bindings = self.bindings("cmd_desktop_proof", runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_desktop_proof(bindings, args_obj), 6)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["load_config_fn"], bindings["load_config"])
        self.assertIs(captured["kwargs"]["desktop_proof_summaries_fn"], bindings["desktop_proof_summaries"])
        self.assertIs(captured["kwargs"]["desktop_proof_empty_line_fn"], bindings["_desktop_cli"].desktop_proof_empty_line)
        self.assertIs(captured["kwargs"]["desktop_proof_lines_fn"], bindings["_desktop_cli"].desktop_proof_lines)
        self.assertIs(captured["kwargs"]["short_sha_fn"], bindings["short_sha"])

    def test_publish_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 8

        bindings = self.bindings("cmd_desktop_publish", runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_desktop_publish(bindings, args_obj), 8)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["load_config_fn"], bindings["load_config"])
        self.assertIs(captured["kwargs"]["desktop_run_manifests_fn"], bindings["desktop_run_manifests"])
        self.assertIs(captured["kwargs"]["stage_desktop_publish_report_fn"], bindings["stage_desktop_publish_report"])
        self.assertIs(captured["kwargs"]["desktop_publish_lines_fn"], bindings["_desktop_cli"].desktop_publish_lines)

    def test_cleanup_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 9

        bindings = self.bindings("cmd_desktop_cleanup", runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_desktop_cleanup(bindings, args_obj), 9)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["load_config_fn"], bindings["load_config"])
        self.assertIs(captured["kwargs"]["prune_desktop_run_manifests_fn"], bindings["prune_desktop_run_manifests"])
        self.assertIs(captured["kwargs"]["write_desktop_run_rollups_fn"], bindings["write_desktop_run_rollups"])
        self.assertIs(captured["kwargs"]["desktop_cleanup_empty_line_fn"], bindings["_desktop_cli"].desktop_cleanup_empty_line)
        self.assertIs(captured["kwargs"]["desktop_cleanup_lines_fn"], bindings["_desktop_cli"].desktop_cleanup_lines)

    def test_install_desktop_report_command_helpers_wires_named_exports(self) -> None:
        def runner(*args, **kwargs):
            return 19

        bindings = self.bindings("cmd_desktop_recent", runner)

        self.mod.install_desktop_report_command_helpers(bindings, ("cmd_desktop_recent",))

        self.assertEqual(bindings["cmd_desktop_recent"](object()), 19)
        self.assertNotIn("cmd_desktop_cleanup", bindings)

    def test_install_desktop_report_command_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_desktop_report_command_helper = lambda _bindings: "future"

        self.mod.install_desktop_report_command_helpers(bindings, ("future_desktop_report_command_helper",))

        self.assertEqual(bindings["future_desktop_report_command_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
