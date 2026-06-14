#!/usr/bin/env python3
"""Tests for cloud refresh facade dependency wiring helpers."""

from pathlib import Path
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("cloud_refresh_facade_helpers.py")


class CloudRefreshFacadeHelpersTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_resolve_github_repository_prefers_config_then_discovery(self):
        self.assertEqual(
            self.mod.resolve_github_repository_with_deps(
                {"repository": "configured/repo"},
                gh_repo_name_fn=lambda: "discovered/repo",
            ),
            "configured/repo",
        )
        self.assertEqual(
            self.mod.resolve_github_repository_with_deps(
                {"repository": "  "},
                gh_repo_name_fn=lambda: "discovered/repo",
            ),
            "discovered/repo",
        )
        with self.assertRaises(ValueError):
            self.mod.resolve_github_repository_with_deps({}, gh_repo_name_fn=lambda: None)

    def test_update_and_refresh_cloud_record_forward_dependencies(self):
        calls = []

        def update_fn(record, snapshot, **kwargs):
            calls.append(("update-with-deps", record, snapshot, kwargs))
            return {"updated": True}

        self.assertEqual(
            self.mod.update_cloud_record_from_run_with_deps(
                {"dispatch_id": "abc"},
                {"databaseId": 7},
                provider_resolved="namespace",
                update_cloud_record_from_run_fn=update_fn,
                now_iso_fn=lambda: "now",
            ),
            {"updated": True},
        )
        self.assertEqual(calls[0][3]["provider_resolved"], "namespace")

        self.assertEqual(
            self.mod.refresh_cloud_record_with_deps(
                {"dispatch_id": "no-run"},
                "danielraffel/pulp",
                require_snapshot=True,
                normalize_cloud_record_fn=lambda record: {"normalized": record["dispatch_id"]},
                gh_run_view_fn=lambda repository, run_id: None,
                update_cloud_record_from_run_fn=lambda record, snapshot: {},
                enrich_cloud_record_provider_metadata_fn=lambda record: record,
                save_cloud_record_fn=lambda record: Path("unused"),
            ),
            {"normalized": "no-run"},
        )
        self.assertEqual(
            self.mod.refresh_cloud_record_with_deps(
                {"dispatch_id": "missing", "run_id": 42},
                "danielraffel/pulp",
                require_snapshot=False,
                normalize_cloud_record_fn=lambda record: {"normalized": record["dispatch_id"]},
                gh_run_view_fn=lambda repository, run_id: None,
                update_cloud_record_from_run_fn=lambda record, snapshot: {},
                enrich_cloud_record_provider_metadata_fn=lambda record: record,
                save_cloud_record_fn=lambda record: Path("unused"),
            ),
            {"normalized": "missing"},
        )
        with self.assertRaises(RuntimeError):
            self.mod.refresh_cloud_record_with_deps(
                {"dispatch_id": "missing", "run_id": 42},
                "danielraffel/pulp",
                require_snapshot=True,
                normalize_cloud_record_fn=lambda record: record,
                gh_run_view_fn=lambda repository, run_id: None,
                update_cloud_record_from_run_fn=lambda record, snapshot: {},
                enrich_cloud_record_provider_metadata_fn=lambda record: record,
                save_cloud_record_fn=lambda record: Path("unused"),
            )

        def update(record, snapshot):
            calls.append(("update", record, snapshot))
            return {"updated": record["dispatch_id"], "snapshot": snapshot["databaseId"]}

        def enrich(record):
            calls.append(("enrich", record))
            return {"enriched": record["updated"]}

        def save(record):
            calls.append(("save", record))
            return Path("stored.json")

        refreshed = self.mod.refresh_cloud_record_with_deps(
            {"dispatch_id": "abc", "run_id": 42},
            "danielraffel/pulp",
            require_snapshot=True,
            normalize_cloud_record_fn=lambda record: record,
            gh_run_view_fn=lambda repository, run_id: {"databaseId": run_id, "repo": repository},
            update_cloud_record_from_run_fn=update,
            enrich_cloud_record_provider_metadata_fn=enrich,
            save_cloud_record_fn=save,
        )

        self.assertEqual(refreshed, {"enriched": "abc"})
        self.assertEqual([call[0] for call in calls], ["update-with-deps", "update", "enrich", "save"])


if __name__ == "__main__":
    unittest.main()
