#!/usr/bin/env python3
"""Facade-level Windows checkout and session-agent request integration tests."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(
        MODULE_PATH,
        module_name="pulp_local_ci_windows_checkout_session_integration",
        add_module_dir=True,
    )


class WindowsCheckoutSessionIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_windows_checkout_and_desktop_request_edge_paths(self) -> None:
        self.assertEqual(self.mod.windows_path_join("", r"C:\Root\\", r"\child", ""), r"C:\Root\child")
        self.assertEqual(self.mod.windows_default_repo_checkout_path(None), "pulp-validate")
        self.assertTrue(self.mod.windows_repo_path_is_unsafe(None))
        self.assertTrue(self.mod.windows_repo_path_is_unsafe(r"C:\\"))
        self.assertTrue(self.mod.windows_repo_path_is_unsafe(r"C:\Users\dev", r"C:\Users\dev"))
        self.assertFalse(self.mod.windows_repo_path_is_unsafe(r"C:\Users\dev\pulp", r"C:\Users\dev"))
        self.assertFalse(self.mod.windows_repo_checkout_ready(None))
        self.assertTrue(
            self.mod.windows_repo_checkout_ready(
                {
                    "git_dir_exists": True,
                    "head_exists": True,
                    "setup_exists": True,
                    "repo_path_unsafe": False,
                }
            )
        )

        failing_probe = subprocess.CompletedProcess([], 4, stdout="", stderr="repo failed")
        with mock.patch.object(self.mod, "run_windows_ssh_powershell", return_value=failing_probe):
            with self.assertRaisesRegex(RuntimeError, "repo failed"):
                self.mod.probe_windows_repo_checkout("win", r"C:\Pulp")

        probe_payload = (
            '{"home_dir":"C:\\\\Users\\\\dev","repo_path":"C:\\\\Users\\\\dev",'
            '"repo_exists":true,"git_dir_exists":true,"head_exists":true,'
            '"setup_exists":true,"origin_url":"https://example.test/pulp.git"}'
        )
        with mock.patch.object(
            self.mod,
            "run_windows_ssh_powershell",
            return_value=subprocess.CompletedProcess([], 0, stdout=probe_payload, stderr=""),
        ):
            probe = self.mod.probe_windows_repo_checkout("win", r"C:\Users\dev")
        self.assertTrue(probe["repo_path_unsafe"])

        bootstrap_payload = (
            '{"home_dir":"C:\\\\Users\\\\dev","repo_path":"C:\\\\Users\\\\dev\\\\pulp-validate",'
            '"repo_exists":true,"git_dir_exists":true,"head_exists":true,'
            '"setup_exists":true,"origin_url":"https://example.test/pulp.git"}'
        )
        with mock.patch.object(
            self.mod,
            "probe_windows_repo_checkout",
            return_value={
                "home_dir": r"C:\Users\dev",
                "repo_path": r"C:\Users\dev",
                "head_exists": False,
                "setup_exists": False,
            },
        ):
            with mock.patch.object(
                self.mod,
                "run_windows_ssh_powershell",
                return_value=subprocess.CompletedProcess([], 0, stdout=bootstrap_payload, stderr=""),
            ) as run_ps:
                ensured = self.mod.ensure_windows_remote_repo_checkout(
                    "win",
                    r"C:\Users\dev",
                    remote_url="https://example.test/pulp.git",
                    bundle_name="bundle.git",
                    bundle_ref="refs/pulp-ci-bundles/job",
                )
        self.assertFalse(ensured["repo_path_unsafe"])
        self.assertIn("git fetch bundle", run_ps.call_args.args[1])
        self.assertIn("pulp-validate", run_ps.call_args.args[1])

        cleanup_output = 'banner\n{"found":true,"matched":true,"killed":true,"children":[456]}\n'
        with mock.patch.object(
            self.mod,
            "run_logged_command",
            return_value={"returncode": 0, "output": cleanup_output},
        ) as run_cleanup:
            cleanup = self.mod.cleanup_stale_windows_validator("win", 123, "2026-05-01T00:00:00Z")
        self.assertTrue(cleanup["killed"])
        self.assertEqual(cleanup["children"], [456])
        self.assertIn("$PidToKill = 123", run_cleanup.call_args.kwargs["input_text"])
        self.assertIn("$ExpectedStart = '2026-05-01T00:00:00Z'", run_cleanup.call_args.kwargs["input_text"])

        with mock.patch.object(
            self.mod,
            "run_logged_command",
            return_value={"returncode": 7, "output": "not json\n"},
        ):
            cleanup = self.mod.cleanup_stale_windows_validator("win", 123, "")
        self.assertEqual(cleanup["error"], "not json")

        contract = self.mod.desktop_target_contract(
            "windows",
            {"adapter": "windows-session-agent", "remote_root": r"C:\agent", "task_name": "Pulp Agent"},
        )
        request = self.mod.build_windows_session_agent_request(
            "windows",
            contract,
            "pulp smoke",
            repo_path=r"C:\Pulp",
            action_name="smoke",
            label=None,
            pulp_app_automation=True,
            capture_ui_snapshot=True,
            click_point="10,20",
            click_view_id="ok",
            click_view_type="button",
            click_view_text="OK",
            click_view_label="Confirm",
            capture_before=True,
            settle_secs=0.75,
            timeout_secs=9.0,
        )
        self.assertEqual(request["label"], "pulp")
        self.assertEqual(
            request["outputs"]["ui_snapshot"],
            self.mod.windows_path_join(contract["results_dir"], request["job_id"], "ui-tree.json"),
        )
        self.assertEqual(request["env"]["PULP_AUTOMATION_CLICK_VIEW_TYPE"], "button")
        self.assertEqual(request["env"]["PULP_AUTOMATION_CLICK_VIEW_TEXT"], "OK")
        self.assertEqual(request["env"]["PULP_AUTOMATION_CLICK_VIEW_LABEL"], "Confirm")
        self.assertEqual(request["env"]["PULP_AUTOMATION_AFTER_DELAY_MS"], "750")
        self.assertEqual(self.mod.desktop_target_contract("mac", {"adapter": "macos-local"}), {})
        with self.assertRaisesRegex(ValueError, "Unknown desktop target"):
            self.mod.resolve_desktop_target({"desktop_automation": {"targets": {}}}, "missing")
        with self.assertRaisesRegex(ValueError, "disabled"):
            self.mod.resolve_desktop_target(
                {"desktop_automation": {"targets": {"windows": {"enabled": False}}}},
                "windows",
            )


if __name__ == "__main__":
    unittest.main()
