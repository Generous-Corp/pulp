#!/usr/bin/env python3
"""Tests for desktop subcommand parser construction."""

from __future__ import annotations

import argparse
import pathlib
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("cli_parser_desktop.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CliParserDesktopTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def build_parser(self) -> argparse.ArgumentParser:
        parser = argparse.ArgumentParser()
        sub = parser.add_subparsers(dest="command")
        self.mod.add_desktop_subcommands(sub)
        return parser

    def test_status_config_and_reporting_commands(self) -> None:
        parser = self.build_parser()

        status = parser.parse_args(["desktop", "status", "mac", "--json"])
        self.assertEqual(status.command, "desktop")
        self.assertEqual(status.desktop_command, "status")
        self.assertEqual(status.target, "mac")
        self.assertTrue(status.json)

        config = parser.parse_args(["desktop", "config", "set", "retention_days", "7", "--json"])
        self.assertEqual(config.desktop_command, "config")
        self.assertEqual(config.desktop_config_command, "set")
        self.assertEqual(config.key, "retention_days")
        self.assertEqual(config.value, "7")
        self.assertTrue(config.json)

        proof = parser.parse_args([
            "desktop",
            "proof",
            "windows",
            "--action",
            "inspect",
            "--source-mode",
            "exact-sha",
            "--sha",
            "a" * 40,
            "--branch",
            "feature/a",
            "--limit",
            "4",
            "--json",
        ])
        self.assertEqual(proof.desktop_command, "proof")
        self.assertEqual(proof.source_mode, "exact-sha")
        self.assertEqual(proof.limit, 4)
        self.assertTrue(proof.json)

    def test_action_commands_share_desktop_source_arguments(self) -> None:
        parser = self.build_parser()

        smoke = parser.parse_args([
            "desktop",
            "smoke",
            "mac",
            "--command",
            "./app",
            "--bundle-id",
            "com.pulp.App",
            "--label",
            "smoke",
            "--output",
            "out.png",
            "--capture-ui-snapshot",
            "--click",
            "12,34",
            "--click-view-id",
            "root",
            "--click-view-type",
            "Button",
            "--click-view-text",
            "OK",
            "--click-view-label",
            "Confirm",
            "--pulp-app-automation",
            "--capture-before",
            "--settle-secs",
            "0.25",
            "--timeout",
            "3",
            "--json",
            "--source-mode",
            "exact-sha",
            "--sha",
            "b" * 40,
            "--prepare-command",
            "cmake --build build",
        ])

        self.assertEqual(smoke.desktop_command, "smoke")
        self.assertEqual(smoke.launch_command, "./app")
        self.assertEqual(smoke.bundle_id, "com.pulp.App")
        self.assertTrue(smoke.capture_ui_snapshot)
        self.assertTrue(smoke.capture_before)
        self.assertEqual(smoke.source_mode, "exact-sha")
        self.assertEqual(smoke.sha, "b" * 40)
        self.assertEqual(smoke.prepare_command, "cmake --build build")
        self.assertEqual(smoke.prepare_timeout, 900.0)

        click = parser.parse_args(["desktop", "click", "windows", "--click", "1,2", "--settle-secs", "1.5"])
        self.assertEqual(click.desktop_command, "click")
        self.assertEqual(click.click, "1,2")
        self.assertEqual(click.settle_secs, 1.5)

        inspect_args = parser.parse_args(["desktop", "inspect", "mac", "--pulp-app-automation", "--timeout", "2"])
        self.assertEqual(inspect_args.desktop_command, "inspect")
        self.assertTrue(inspect_args.pulp_app_automation)
        self.assertEqual(inspect_args.timeout, 2.0)

    def test_cleanup_and_install_doctor_arguments(self) -> None:
        parser = self.build_parser()

        install = parser.parse_args(["desktop", "install", "ubuntu"])
        self.assertEqual(install.desktop_command, "install")
        self.assertEqual(install.target, "ubuntu")

        doctor = parser.parse_args(["desktop", "doctor", "windows", "--json"])
        self.assertEqual(doctor.desktop_command, "doctor")
        self.assertTrue(doctor.json)

        cleanup = parser.parse_args(["desktop", "cleanup", "mac", "--older-than-days", "30", "--keep-last", "2", "--json"])
        self.assertEqual(cleanup.desktop_command, "cleanup")
        self.assertEqual(cleanup.older_than_days, 30)
        self.assertEqual(cleanup.keep_last, 2)
        self.assertTrue(cleanup.json)


if __name__ == "__main__":
    unittest.main()
