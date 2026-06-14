#!/usr/bin/env python3
"""Tests for pure cloud run preparation helpers."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("cloud_run_prepare.py")


class CloudRunPrepareTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_cloud_run_record_payload_preserves_dispatch_metadata(self) -> None:
        payload = self.mod.cloud_run_record_payload(
            dispatch_id="abc123",
            repository="danielraffel/pulp",
            workflow_key="build",
            workflow={"file": "build.yml", "display_name": "Build and Test"},
            branch="feature/x",
            requested_by="danielraffel",
            provider="namespace",
            selector_json='"namespace-profile-default"',
            dispatch_fields={"linux_runner_selector_json": '"namespace-profile-linux"'},
            dispatch_time="2026-04-04T12:00:00+00:00",
        )

        self.assertEqual(
            payload,
            {
                "dispatch_id": "abc123",
                "repository": "danielraffel/pulp",
                "workflow_key": "build",
                "workflow_file": "build.yml",
                "workflow_name": "Build and Test",
                "requested_ref": "feature/x",
                "requested_by": "danielraffel",
                "provider_requested": "namespace",
                "runner_selector_json": '"namespace-profile-default"',
                "dispatch_fields": {"linux_runner_selector_json": '"namespace-profile-linux"'},
                "status": "unresolved",
                "dispatched_at": "2026-04-04T12:00:00+00:00",
                "updated_at": "2026-04-04T12:00:00+00:00",
                "match_strategy": "workflow+branch+created_at",
            },
        )

    def test_cloud_workflow_dispatch_fields_include_provider_dispatch_and_selector_inputs(self) -> None:
        fields = self.mod.cloud_workflow_dispatch_fields(
            {
                "provider_input": "runner_provider",
                "selector_input": "runner_selector_json",
            },
            provider="namespace",
            dispatch_fields={"linux_runner_selector_json": '"namespace-profile-linux"'},
            selector_json='"namespace-profile-default"',
        )

        self.assertEqual(
            fields,
            {
                "runner_provider": "namespace",
                "linux_runner_selector_json": '"namespace-profile-linux"',
                "runner_selector_json": '"namespace-profile-default"',
            },
        )

    def test_cloud_workflow_dispatch_fields_skip_optional_inputs(self) -> None:
        fields = self.mod.cloud_workflow_dispatch_fields(
            {},
            provider="github-hosted",
            dispatch_fields={"macos_runner_selector_json": '"macos-15"'},
            selector_json='"ignored"',
        )

        self.assertEqual(fields, {"macos_runner_selector_json": '"macos-15"'})


if __name__ == "__main__":
    unittest.main()
