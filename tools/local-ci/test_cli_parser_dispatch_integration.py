#!/usr/bin/env python3
"""Integration coverage for the local-ci parser and main dispatcher."""

from __future__ import annotations

import io
import sys
import unittest
from contextlib import redirect_stdout
from unittest import mock

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("local_ci.py", module_name="pulp_local_ci_cli_parser_dispatch_integration", add_module_dir=True)


class CliParserDispatchIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_main_dispatches_top_level_and_cloud_subcommands(self) -> None:
        top_level_cases = [
            (["enqueue", "feature/a", "--sha", "a" * 40, "--smoke"], "cmd_enqueue", 11),
            (["drain"], "cmd_drain", 12),
            (["run", "feature/a", "--targets", "mac", "--smoke"], "cmd_run", 13),
            (["ship", "feature/a", "--base", "main"], "cmd_ship", 14),
            (["check", "2752", "--smoke"], "cmd_check", 15),
            (["list"], "cmd_list", 16),
            (["bump", "job-1", "high"], "cmd_bump", 17),
            (["cancel", "job-1"], "cmd_cancel", 18),
            (["logs", "job-1", "--target", "mac", "--lines", "5"], "cmd_logs", 19),
            (["cleanup", "--dry-run", "--include-prepared"], "cmd_cleanup", 20),
            (["evidence", "feature/a", "--sha", "a" * 40], "cmd_evidence", 21),
            (["status"], "cmd_status", 22),
            (["desktop", "status", "mac", "--json"], "cmd_desktop", 23),
        ]
        for argv_tail, handler_name, expected in top_level_cases:
            with self.subTest(handler=handler_name):
                with mock.patch.object(sys, "argv", ["local_ci.py", *argv_tail]), \
                     mock.patch.object(self.mod, handler_name, return_value=expected) as handler:
                    self.assertEqual(self.mod.main(), expected)
                    handler.assert_called_once()

        cloud_cases = [
            (["cloud", "workflows"], "cmd_cloud_workflows", 31),
            (["cloud", "defaults"], "cmd_cloud_defaults", 32),
            (["cloud", "history", "--workflow", "build", "--provider", "namespace"], "cmd_cloud_history", 33),
            (["cloud", "compare", "build"], "cmd_cloud_compare", 34),
            (["cloud", "recommend", "build"], "cmd_cloud_recommend", 35),
            (["cloud", "run", "build", "feature/a", "--wait"], "cmd_cloud_run", 36),
            (["cloud", "status", "latest", "--refresh"], "cmd_cloud_status", 37),
            (["cloud", "namespace", "doctor"], "cmd_cloud_namespace_doctor", 38),
            (["cloud", "namespace", "setup"], "cmd_cloud_namespace_setup", 39),
        ]
        for argv_tail, handler_name, expected in cloud_cases:
            with self.subTest(handler=handler_name):
                with mock.patch.object(sys, "argv", ["local_ci.py", *argv_tail]), \
                     mock.patch.object(self.mod, handler_name, return_value=expected) as handler:
                    self.assertEqual(self.mod.main(), expected)
                    handler.assert_called_once()

        with mock.patch.object(sys, "argv", ["local_ci.py", "cloud"]), \
             redirect_stdout(io.StringIO()) as buf:
            self.assertEqual(self.mod.main(), 1)
        self.assertIn("missing cloud subcommand", buf.getvalue())

        with mock.patch.object(sys, "argv", ["local_ci.py", "cloud", "namespace"]), \
             redirect_stdout(io.StringIO()) as buf:
            self.assertEqual(self.mod.main(), 1)
        self.assertIn("missing cloud namespace subcommand", buf.getvalue())

        with mock.patch.object(sys, "argv", ["local_ci.py"]), redirect_stdout(io.StringIO()) as buf:
            self.assertEqual(self.mod.main(), 1)
        self.assertIn("Local CI runner for Pulp", buf.getvalue())

    def test_parser_desktop_action_source_contracts_and_defaults(self) -> None:
        parser = self.mod.build_parser()

        smoke = parser.parse_args([
            "desktop",
            "smoke",
            "mac",
            "--command",
            "open Pulp.app",
            "--source-mode",
            "exact-sha",
            "--branch",
            "feature/a",
            "--sha",
            "a" * 40,
            "--prepare-command",
            "cmake --build build",
            "--prepare-timeout",
            "12.5",
            "--capture-ui-snapshot",
            "--click-view-id",
            "root",
            "--capture-before",
            "--settle-secs",
            "0.25",
            "--timeout",
            "2.5",
            "--json",
        ])
        self.assertEqual(smoke.command, "desktop")
        self.assertEqual(smoke.desktop_command, "smoke")
        self.assertEqual(smoke.target, "mac")
        self.assertEqual(smoke.launch_command, "open Pulp.app")
        self.assertEqual(smoke.source_mode, "exact-sha")
        self.assertEqual(smoke.branch, "feature/a")
        self.assertEqual(smoke.sha, "a" * 40)
        self.assertEqual(smoke.prepare_command, "cmake --build build")
        self.assertEqual(smoke.prepare_timeout, 12.5)
        self.assertTrue(smoke.capture_ui_snapshot)
        self.assertEqual(smoke.click_view_id, "root")
        self.assertTrue(smoke.capture_before)
        self.assertEqual(smoke.settle_secs, 0.25)
        self.assertEqual(smoke.timeout, 2.5)
        self.assertTrue(smoke.json)

        click = parser.parse_args(["desktop", "click", "windows", "--bundle-id", "com.pulp.App", "--click", "12,34"])
        self.assertEqual(click.desktop_command, "click")
        self.assertEqual(click.target, "windows")
        self.assertEqual(click.bundle_id, "com.pulp.App")
        self.assertEqual(click.click, "12,34")
        self.assertEqual(click.source_mode, "live")
        self.assertEqual(click.prepare_timeout, 900.0)
        self.assertFalse(click.capture_ui_snapshot)
        self.assertFalse(click.pulp_app_automation)

        inspect_args = parser.parse_args([
            "desktop",
            "inspect",
            "ubuntu",
            "--bundle-id",
            "com.pulp.Tool",
            "--pulp-app-automation",
            "--json",
        ])
        self.assertEqual(inspect_args.desktop_command, "inspect")
        self.assertEqual(inspect_args.target, "ubuntu")
        self.assertEqual(inspect_args.bundle_id, "com.pulp.Tool")
        self.assertTrue(inspect_args.pulp_app_automation)
        self.assertTrue(inspect_args.json)
        self.assertEqual(inspect_args.source_mode, "live")
        self.assertIsNone(inspect_args.prepare_command)

        cleanup = parser.parse_args(["cleanup", "--apply", "--keep-results", "3", "--keep-logs", "4", "--keep-bundles", "5"])
        self.assertEqual(cleanup.command, "cleanup")
        self.assertTrue(cleanup.apply)
        self.assertEqual(cleanup.keep_results, 3)
        self.assertEqual(cleanup.keep_logs, 4)
        self.assertEqual(cleanup.keep_bundles, 5)
        self.assertFalse(cleanup.include_prepared)


if __name__ == "__main__":
    unittest.main()
