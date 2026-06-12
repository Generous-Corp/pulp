#!/usr/bin/env python3
"""Tests for Namespace CLI wrappers used by cloud CI helpers."""

from __future__ import annotations

import argparse
import io
import json
import pathlib
import subprocess
import unittest
from contextlib import redirect_stdout
from types import SimpleNamespace
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("cloud_namespace.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


class CloudNamespaceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_nsc_cli_helpers_parse_availability_workspace_and_instances(self) -> None:
        history_payload = [
            {
                "cluster_id": "cluster-2",
                "created_at": "2026-04-04T12:01:00Z",
                "github_workflow": {"repository": "other/repo", "run_id": 123},
            },
            {
                "cluster_id": "cluster-1",
                "created_at": "2026-04-04T12:00:00Z",
                "github_workflow": {"repository": "danielraffel/pulp", "run_id": 123},
            },
        ]

        def fake_run(cmd, **kwargs):
            if cmd == ["nsc", "version"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="v0.0.493\n", stderr="")
            if cmd == ["nsc", "auth", "check-login"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            if cmd == ["nsc", "workspace", "describe"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="Name: Personal\nTenant ID: tenant_123\n", stderr="")
            if cmd[:5] == ["nsc", "instance", "history", "--all", "-o"]:
                return subprocess.CompletedProcess(cmd, 0, stdout=json.dumps(history_payload), stderr="")
            raise AssertionError(cmd)

        with mock.patch.object(self.mod.subprocess, "run", side_effect=fake_run):
            self.assertTrue(self.mod.nsc_available())
            self.assertEqual(self.mod.nsc_version(), "v0.0.493")
            self.assertTrue(self.mod.nsc_logged_in())
            self.assertEqual(self.mod.nsc_workspace_info()["Name"], "Personal")
            self.assertEqual(self.mod.parse_colon_separated_fields("A: one\nbad\nB: two:three"), {"A": "one", "B": "two:three"})
            self.assertEqual(len(self.mod.nsc_instance_history(max_entries=7)), 2)
            matches = self.mod.namespace_instances_for_run(
                "danielraffel/pulp",
                123,
                nsc_instance_history_fn=lambda: history_payload,
                normalize_namespace_instance_fn=lambda item: dict(item),
            )
            self.assertEqual([item["cluster_id"] for item in matches], ["cluster-1"])

    def test_nsc_cli_error_edges(self) -> None:
        with mock.patch.object(self.mod.subprocess, "run", side_effect=FileNotFoundError):
            self.assertIsNone(self.mod.nsc_run(["version"]))
            self.assertFalse(self.mod.nsc_available())
            self.assertIsNone(self.mod.nsc_version())
            self.assertFalse(self.mod.nsc_logged_in())
            self.assertIsNone(self.mod.nsc_workspace_info())
            self.assertEqual(self.mod.nsc_instance_history(), [])

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["nsc"], 0, stdout="{bad", stderr=""),
        ):
            self.assertEqual(self.mod.nsc_instance_history(), [])

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["nsc"], 0, stdout=json.dumps({"not": "a-list"}), stderr=""),
        ):
            self.assertEqual(self.mod.nsc_instance_history(), [])

    def test_namespace_doctor_and_setup_use_injected_dependencies(self) -> None:
        buf = io.StringIO()
        with redirect_stdout(buf):
            missing = self.mod.cmd_cloud_namespace_doctor(
                argparse.Namespace(),
                nsc_version_fn=lambda: None,
                print_namespace_setup_help_fn=lambda: print("setup help"),
            )
        self.assertEqual(missing, 1)
        self.assertIn("Namespace CLI: missing", buf.getvalue())
        self.assertIn("setup help", buf.getvalue())

        buf = io.StringIO()
        with redirect_stdout(buf):
            ready = self.mod.cmd_cloud_namespace_doctor(
                argparse.Namespace(),
                nsc_version_fn=lambda: "v0.0.493",
                nsc_logged_in_fn=lambda: True,
                nsc_workspace_info_fn=lambda: {"Name": "Personal", "Tenant ID": "tenant_123"},
            )
        self.assertEqual(ready, 0)
        self.assertIn("Workspace: Personal", buf.getvalue())

        calls = []
        buf = io.StringIO()
        with redirect_stdout(buf):
            failed_login = self.mod.cmd_cloud_namespace_setup(
                argparse.Namespace(),
                nsc_available_fn=lambda: True,
                nsc_logged_in_fn=lambda: False,
                nsc_run_fn=lambda args, capture_output=True: calls.append((args, capture_output)) or SimpleNamespace(returncode=1),
            )
        self.assertEqual(failed_login, 1)
        self.assertEqual(calls, [(["login"], False)])
        self.assertIn("Namespace login: failed", buf.getvalue())

        buf = io.StringIO()
        with redirect_stdout(buf):
            setup_ready = self.mod.cmd_cloud_namespace_setup(
                argparse.Namespace(),
                nsc_available_fn=lambda: True,
                nsc_logged_in_fn=lambda: True,
                cmd_cloud_namespace_doctor_fn=lambda _args: 0,
            )
        self.assertEqual(setup_ready, 0)


if __name__ == "__main__":
    unittest.main()
