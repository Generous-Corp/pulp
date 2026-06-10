#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).resolve().with_name("cli_parser.py")


def load_cli_parser_module():
    spec = importlib.util.spec_from_file_location("cli_parser_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class CliParserTests(unittest.TestCase):
    def build_parser(self, *, keep_completed_jobs: int = 25):
        module = load_cli_parser_module()
        return module.build_local_ci_parser(
            priority_values={"low", "normal", "high"},
            keep_completed_jobs=keep_completed_jobs,
            epilog="test epilog",
        )

    def test_run_submission_args(self):
        parser = self.build_parser()

        args = parser.parse_args([
            "run",
            "feature/local-ci",
            "--priority",
            "high",
            "--targets",
            "mac,ubuntu",
            "--sha",
            "a" * 40,
            "--smoke",
            "--allow-root-mismatch",
            "--allow-unreachable-targets",
        ])

        self.assertEqual(args.command, "run")
        self.assertEqual(args.branch, "feature/local-ci")
        self.assertEqual(args.priority, "high")
        self.assertEqual(args.targets, "mac,ubuntu")
        self.assertEqual(args.sha, "a" * 40)
        self.assertTrue(args.smoke)
        self.assertTrue(args.allow_root_mismatch)
        self.assertTrue(args.allow_unreachable_targets)

    def test_cloud_namespace_commands(self):
        parser = self.build_parser()

        args = parser.parse_args(["cloud", "namespace", "doctor"])

        self.assertEqual(args.command, "cloud")
        self.assertEqual(args.cloud_command, "namespace")
        self.assertEqual(args.cloud_namespace_command, "doctor")

    def test_desktop_source_args_are_shared(self):
        parser = self.build_parser()

        args = parser.parse_args([
            "desktop",
            "click",
            "mac",
            "--source-mode",
            "exact-sha",
            "--sha",
            "b" * 40,
            "--prepare-command",
            "cmake --build build",
            "--click",
            "12,34",
        ])

        self.assertEqual(args.command, "desktop")
        self.assertEqual(args.desktop_command, "click")
        self.assertEqual(args.target, "mac")
        self.assertEqual(args.source_mode, "exact-sha")
        self.assertEqual(args.sha, "b" * 40)
        self.assertEqual(args.prepare_command, "cmake --build build")
        self.assertEqual(args.prepare_timeout, 900.0)
        self.assertEqual(args.click, "12,34")

    def test_cleanup_defaults_use_injected_retention(self):
        parser = self.build_parser(keep_completed_jobs=7)

        args = parser.parse_args(["cleanup"])

        self.assertEqual(args.command, "cleanup")
        self.assertEqual(args.keep_results, 7)
        self.assertEqual(args.keep_logs, 7)
        self.assertEqual(args.keep_bundles, 0)
        self.assertFalse(args.include_prepared)
        self.assertFalse(args.dry_run)
        self.assertFalse(args.apply)


if __name__ == "__main__":
    unittest.main()
