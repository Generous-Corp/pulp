#!/usr/bin/env python3
"""Tests for desktop reporting facade composition."""

import unittest
from unittest import mock

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_reporting_bindings.py")


class DesktopReportingBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_reporting_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_PUBLISH_EXPORTS,
            *self.mod.DESKTOP_RUN_ROLLUP_EXPORTS,
            *self.mod.DESKTOP_PROOF_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_REPORTING_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_desktop_reporting_helpers_routes_each_group(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_publish_helpers") as install_publish,
            mock.patch.object(self.mod, "install_desktop_run_rollup_helpers") as install_rollup,
            mock.patch.object(self.mod, "install_desktop_proof_helpers") as install_proof,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_reporting_helpers(
                bindings,
                ("desktop_publish_reports", "desktop_run_manifests", "desktop_proof_summaries", "unknown_helper"),
            )

        install_publish.assert_called_once_with(bindings, ("desktop_publish_reports",))
        install_rollup.assert_called_once_with(bindings, ("desktop_run_manifests",))
        install_proof.assert_called_once_with(bindings, ("desktop_proof_summaries",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
