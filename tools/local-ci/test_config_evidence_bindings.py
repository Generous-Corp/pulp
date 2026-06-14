#!/usr/bin/env python3
"""Tests for config/evidence facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
from unittest import mock
import unittest


MODULE_PATH = Path(__file__).with_name("config_evidence_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ConfigEvidenceBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_compose_focused_groups(self) -> None:
        expected = (
            *self.mod.CONFIG_FILE_EXPORTS,
            *self.mod.CONFIG_EVIDENCE_SUMMARY_EXPORTS,
        )

        self.assertEqual(self.mod.CONFIG_EVIDENCE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_config_evidence_helpers_routes_focused_exports(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_config_file_helpers") as install_config,
            mock.patch.object(self.mod, "install_config_evidence_summary_helpers") as install_evidence,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_config_evidence_helpers(
                bindings,
                ("save_config", "evidence_empty_line", "custom_helper"),
            )

        install_config.assert_called_once_with(bindings, ("save_config",))
        install_evidence.assert_called_once_with(bindings, ("evidence_empty_line",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_helper",))


if __name__ == "__main__":
    unittest.main()
