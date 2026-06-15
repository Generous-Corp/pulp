#!/usr/bin/env python3
"""Tests for cloud subcommand parser construction."""

from __future__ import annotations

import argparse
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("cli_parser_cloud.py")


class CliParserCloudTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def build_parser(self) -> argparse.ArgumentParser:
        parser = argparse.ArgumentParser()
        sub = parser.add_subparsers(dest="command")
        self.mod.add_cloud_subcommands(sub)
        return parser

    def test_cloud_namespace_commands(self) -> None:
        args = self.build_parser().parse_args(["cloud", "namespace", "doctor"])

        self.assertEqual(args.command, "cloud")
        self.assertEqual(args.cloud_command, "namespace")
        self.assertEqual(args.cloud_namespace_command, "doctor")

    def test_cloud_run_and_status_arguments(self) -> None:
        parser = self.build_parser()

        run = parser.parse_args([
            "cloud",
            "run",
            "build",
            "feature/parser",
            "--provider",
            "github-hosted",
            "--runner-selector-json",
            '"macos-15"',
            "--linux-runner-selector-json",
            '["ubuntu-latest"]',
            "--windows-runner-selector-json",
            '["windows-latest"]',
            "--macos-runner-selector-json",
            '["macos-15"]',
            "--wait",
        ])

        self.assertEqual(run.cloud_command, "run")
        self.assertEqual(run.workflow, "build")
        self.assertEqual(run.branch, "feature/parser")
        self.assertEqual(run.provider, "github-hosted")
        self.assertEqual(run.runner_selector_json, '"macos-15"')
        self.assertEqual(run.linux_runner_selector_json, '["ubuntu-latest"]')
        self.assertEqual(run.windows_runner_selector_json, '["windows-latest"]')
        self.assertEqual(run.macos_runner_selector_json, '["macos-15"]')
        self.assertTrue(run.wait)

        status = parser.parse_args(["cloud", "status", "latest", "--refresh", "--limit", "2"])
        self.assertEqual(status.cloud_command, "status")
        self.assertEqual(status.identifier, "latest")
        self.assertTrue(status.refresh)
        self.assertEqual(status.limit, 2)

    def test_cloud_history_compare_and_recommend_defaults(self) -> None:
        parser = self.build_parser()

        history = parser.parse_args(["cloud", "history", "--workflow", "build", "--provider", "namespace", "--limit", "3"])
        self.assertEqual(history.cloud_command, "history")
        self.assertEqual(history.workflow, "build")
        self.assertEqual(history.provider, "namespace")
        self.assertEqual(history.limit, 3)

        compare = parser.parse_args(["cloud", "compare"])
        self.assertEqual(compare.cloud_command, "compare")
        self.assertIsNone(compare.workflow)

        recommend = parser.parse_args(["cloud", "recommend", "docs-check"])
        self.assertEqual(recommend.cloud_command, "recommend")
        self.assertEqual(recommend.workflow, "docs-check")


if __name__ == "__main__":
    unittest.main()
