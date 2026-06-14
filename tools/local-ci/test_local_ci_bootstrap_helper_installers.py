#!/usr/bin/env python3
"""Tests for ordered local_ci bootstrap helper installer groups."""

from __future__ import annotations

import types
import unittest

from module_test_utils import load_local_ci_module


helper_installers = load_local_ci_module(
    "local_ci_bootstrap_helper_installers.py",
    module_name="local_ci_bootstrap_helper_installers",
    add_module_dir=True,
)


def recorder(method_name: str, calls: list[str]):
    return types.SimpleNamespace(**{method_name: lambda _bindings: calls.append(method_name)})


class LocalCiBootstrapHelperInstallersTests(unittest.TestCase):
    def test_install_foundation_helpers_preserves_order(self) -> None:
        calls: list[str] = []

        helper_installers.install_foundation_helpers(
            {},
            state_path_bindings=recorder("install_state_path_helpers", calls),
            footprint_bindings=recorder("install_footprint_helpers", calls),
            ssh_subprocess_bindings=recorder("install_ssh_subprocess_helpers", calls),
            ssh_bundle_bindings=recorder("install_ssh_bundle_helpers", calls),
        )

        self.assertEqual(
            calls,
            [
                "install_state_path_helpers",
                "install_footprint_helpers",
                "install_ssh_subprocess_helpers",
                "install_ssh_bundle_helpers",
            ],
        )

    def test_install_core_helpers_preserves_order(self) -> None:
        calls: list[str] = []

        helper_installers.install_core_helpers(
            {},
            windows_target_bindings=recorder("install_windows_target_helpers", calls),
            io_utils_bindings=recorder("install_io_utils_helpers", calls),
            git_helpers_bindings=recorder("install_git_helpers", calls),
            normalize_bindings=recorder("install_normalize_helpers", calls),
            cli_parser_bindings=recorder("install_cli_parser_helpers", calls),
            config_evidence_bindings=recorder("install_config_evidence_helpers", calls),
            github_workflow_bindings=recorder("install_github_workflow_helpers", calls),
            provenance_bindings=recorder("install_provenance_helpers", calls),
            evidence_index_bindings=recorder("install_evidence_index_helpers", calls),
            job_queue_bindings=recorder("install_job_queue_helpers", calls),
            queue_bindings=recorder("install_queue_helpers", calls),
            cleanup_bindings=recorder("install_cleanup_helpers", calls),
            target_bindings=recorder("install_target_helpers", calls),
            cloud_bindings=recorder("install_cloud_helpers", calls),
        )

        self.assertEqual(
            calls,
            [
                "install_windows_target_helpers",
                "install_io_utils_helpers",
                "install_git_helpers",
                "install_normalize_helpers",
                "install_cli_parser_helpers",
                "install_config_evidence_helpers",
                "install_github_workflow_helpers",
                "install_provenance_helpers",
                "install_evidence_index_helpers",
                "install_job_queue_helpers",
                "install_queue_helpers",
                "install_cleanup_helpers",
                "install_target_helpers",
                "install_cloud_helpers",
            ],
        )

    def test_install_desktop_helpers_preserves_order(self) -> None:
        calls: list[str] = []

        helper_installers.install_desktop_helpers(
            {},
            desktop_support_bindings=recorder("install_desktop_support_helpers", calls),
            desktop_infra_bindings=recorder("install_desktop_infra_helpers", calls),
            desktop_reporting_bindings=recorder("install_desktop_reporting_helpers", calls),
            macos_window_bindings=recorder("install_macos_window_helpers", calls),
            linux_target_bindings=recorder("install_linux_target_helpers", calls),
            linux_desktop_bindings=recorder("install_linux_desktop_helpers", calls),
            source_prep_bindings=recorder("install_source_prep_helpers", calls),
            windows_probe_bindings=recorder("install_windows_probe_helpers", calls),
            desktop_probe_bindings=recorder("install_desktop_probe_helpers", calls),
            macos_desktop_bindings=recorder("install_macos_desktop_helpers", calls),
            windows_desktop_bindings=recorder("install_windows_desktop_helpers", calls),
        )

        self.assertEqual(
            calls,
            [
                "install_desktop_support_helpers",
                "install_desktop_infra_helpers",
                "install_desktop_reporting_helpers",
                "install_macos_window_helpers",
                "install_linux_target_helpers",
                "install_linux_desktop_helpers",
                "install_source_prep_helpers",
                "install_windows_probe_helpers",
                "install_desktop_probe_helpers",
                "install_macos_desktop_helpers",
                "install_windows_desktop_helpers",
            ],
        )

    def test_install_command_helpers_preserves_order(self) -> None:
        calls: list[str] = []

        helper_installers.install_command_helpers(
            {},
            target_preflight_bindings=recorder("install_target_preflight_helpers", calls),
            notification_bindings=recorder("install_notification_helpers", calls),
            execution_bindings=recorder("install_execution_helpers", calls),
            private_seams=recorder("install_execution_private_seams", calls),
            utility_command_bindings=recorder("install_utility_command_helpers", calls),
            local_ci_command_bindings=recorder("install_local_ci_command_helpers", calls),
            desktop_command_bindings=recorder("install_desktop_command_helpers", calls),
            cli_dispatch_bindings=recorder("install_cli_dispatch_helpers", calls),
        )

        self.assertEqual(
            calls,
            [
                "install_target_preflight_helpers",
                "install_notification_helpers",
                "install_execution_helpers",
                "install_execution_private_seams",
                "install_utility_command_helpers",
                "install_local_ci_command_helpers",
                "install_desktop_command_helpers",
                "install_cli_dispatch_helpers",
            ],
        )


if __name__ == "__main__":
    unittest.main()
