#!/usr/bin/env python3
"""Tests for config evidence summary facade bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
from unittest import mock
import unittest



def load_module():
    return load_local_ci_module("config_evidence_summary_bindings.py")


class ConfigEvidenceSummaryBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_summary_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.CONFIG_EVIDENCE_INDEX_EXPORTS,
            *self.mod.CONFIG_EVIDENCE_DISPLAY_EXPORTS,
        )

        self.assertEqual(self.mod.CONFIG_EVIDENCE_SUMMARY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_config_evidence_summary_helpers_routes_focused_exports(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_config_evidence_index_helpers") as install_index,
            mock.patch.object(self.mod, "install_config_evidence_display_helpers") as install_display,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_config_evidence_summary_helpers(
                bindings,
                ("load_evidence_index", "evidence_empty_line", "custom_helper"),
            )

        install_index.assert_called_once_with(bindings, ("load_evidence_index",))
        install_display.assert_called_once_with(bindings, ("evidence_empty_line",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_helper",))


if __name__ == "__main__":
    unittest.main()
