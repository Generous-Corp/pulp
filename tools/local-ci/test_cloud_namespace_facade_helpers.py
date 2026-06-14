#!/usr/bin/env python3
"""Tests for Namespace cloud facade dependency wiring helpers."""

import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("cloud_namespace_facade_helpers.py")


class CloudNamespaceFacadeHelpersTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_namespace_usage_helpers_forward_dependencies(self):
        calls = []

        def duration_fn(instance, **kwargs):
            calls.append(("duration", instance, kwargs))
            return 12.5

        def normalize_fn(instance, **kwargs):
            calls.append(("normalize", instance, kwargs))
            return {"normalized": instance["id"]}

        def namespace_matches_fn(repository, run_id, **kwargs):
            calls.append(("matches", repository, run_id, kwargs))
            return [{"cluster_id": "cluster-1"}]

        self.assertEqual(
            self.mod.namespace_instance_duration_secs_with_deps(
                {"id": "instance-1"},
                namespace_instance_duration_secs_fn=duration_fn,
                now_iso_fn=lambda: "now",
            ),
            12.5,
        )
        self.assertEqual(
            self.mod.normalize_namespace_instance_with_deps(
                {"id": "instance-1"},
                normalize_namespace_instance_fn=normalize_fn,
                now_iso_fn=lambda: "now",
            ),
            {"normalized": "instance-1"},
        )
        self.assertEqual(
            self.mod.namespace_instances_for_run_with_deps(
                "danielraffel/pulp",
                123,
                namespace_instances_for_run_fn=namespace_matches_fn,
                nsc_instance_history_fn=lambda: [{"id": "raw"}],
                normalize_namespace_instance_fn=lambda item: item,
            ),
            [{"cluster_id": "cluster-1"}],
        )

        self.assertEqual([call[0] for call in calls], ["duration", "normalize", "matches"])
        self.assertIn("now_iso_fn", calls[0][2])
        self.assertIn("now_iso_fn", calls[1][2])
        self.assertIn("nsc_instance_history_fn", calls[2][3])

    def test_namespace_cli_helpers_forward_nsc_run(self):
        calls = []

        def nsc_run(args, **kwargs):
            calls.append(("run", args, kwargs))
            return None

        self.assertTrue(
            self.mod.nsc_available_with_deps(
                nsc_available_fn=lambda **kwargs: calls.append(("available", kwargs)) or True,
                nsc_run_fn=nsc_run,
            )
        )
        self.assertEqual(
            self.mod.nsc_version_with_deps(
                nsc_version_fn=lambda **kwargs: calls.append(("version", kwargs)) or "v0.0.493",
                nsc_run_fn=nsc_run,
            ),
            "v0.0.493",
        )
        self.assertTrue(
            self.mod.nsc_logged_in_with_deps(
                nsc_logged_in_fn=lambda **kwargs: calls.append(("logged-in", kwargs)) or True,
                nsc_run_fn=nsc_run,
            )
        )
        self.assertEqual(
            self.mod.nsc_workspace_info_with_deps(
                nsc_workspace_info_fn=lambda **kwargs: calls.append(("workspace", kwargs)) or {"Name": "Personal"},
                nsc_run_fn=nsc_run,
            ),
            {"Name": "Personal"},
        )
        self.assertEqual(
            self.mod.nsc_instance_history_with_deps(
                7,
                nsc_instance_history_fn=lambda **kwargs: calls.append(("history", kwargs)) or [{"id": "instance"}],
                nsc_run_fn=nsc_run,
            ),
            [{"id": "instance"}],
        )

        self.assertEqual(
            [call[0] for call in calls],
            ["available", "version", "logged-in", "workspace", "history"],
        )
        for _name, kwargs in calls:
            self.assertIs(kwargs["nsc_run_fn"], nsc_run)
        self.assertEqual(calls[-1][1]["max_entries"], 7)


if __name__ == "__main__":
    unittest.main()
