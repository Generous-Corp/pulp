#!/usr/bin/env python3
"""Tests for cloud compatibility facade dependency wiring helpers."""

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("cloud_facade_helpers.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CloudFacadeHelpersTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_record_storage_helpers_forward_facade_dependencies(self):
        calls = []

        def save_cloud_record_fn(record, **kwargs):
            calls.append(("save", record, kwargs))
            return Path("stored.json")

        def list_cloud_records_fn(**kwargs):
            calls.append(("list", kwargs))
            return [{"dispatch_id": "abc"}]

        self.assertEqual(
            self.mod.save_cloud_record_with_deps(
                {"dispatch_id": "abc"},
                save_cloud_record_fn=save_cloud_record_fn,
                ensure_state_dirs_fn=lambda: None,
                cloud_run_path_fn=lambda dispatch_id: Path(f"{dispatch_id}.json"),
                atomic_write_text_fn=lambda path, text: None,
            ),
            Path("stored.json"),
        )
        self.assertEqual(
            self.mod.list_cloud_records_with_deps(
                limit=5,
                list_cloud_records_fn=list_cloud_records_fn,
                ensure_state_dirs_fn=lambda: None,
                cloud_runs_dir_fn=lambda: Path("."),
                load_cloud_record_fn=lambda path: {"path": str(path)},
            ),
            [{"dispatch_id": "abc"}],
        )

        self.assertEqual(calls[0][0], "save")
        self.assertEqual(calls[0][2]["cloud_run_path_fn"]("abc"), Path("abc.json"))
        self.assertEqual(calls[1][1]["limit"], 5)

    def test_billing_and_metadata_helpers_forward_facade_dependencies(self):
        calls = []

        def billing_totals_fn(records, config, **kwargs):
            calls.append(("totals", records, config, kwargs))
            return {"provider": kwargs["provider"]}

        def github_billing_fn(repository, config, **kwargs):
            calls.append(("github", repository, config, kwargs))
            return {"repository": repository}

        def metadata_fn(record, **kwargs):
            calls.append(("metadata", record, kwargs))
            return {"metadata": True}

        self.assertEqual(
            self.mod.estimate_billing_period_totals_with_deps(
                [{"dispatch_id": "abc"}],
                {"cfg": True},
                provider="namespace",
                estimate_billing_period_totals_fn=billing_totals_fn,
                billing_period_window_fn=lambda start_day: None,
            ),
            {"provider": "namespace"},
        )
        self.assertEqual(
            self.mod.fetch_github_repo_actions_billing_summary_with_deps(
                "danielraffel/pulp",
                {"cfg": True},
                fetch_github_repo_actions_billing_summary_fn=github_billing_fn,
                resolve_billing_settings_fn=lambda config: {},
                gh_available_fn=lambda: True,
                gh_api_json_fn=lambda path, **kwargs: ({}, ""),
                billing_period_window_fn=lambda start_day: None,
                iter_year_months_fn=lambda start, end: [],
                gh_token_scopes_fn=lambda: set(),
                parse_iso_date_fn=lambda value: None,
                provider_billing_note_text_fn=lambda: "note",
            ),
            {"repository": "danielraffel/pulp"},
        )
        self.assertEqual(
            self.mod.enrich_cloud_record_provider_metadata_with_deps(
                {"dispatch_id": "abc"},
                enrich_cloud_record_provider_metadata_fn=metadata_fn,
                normalize_cloud_record_fn=lambda record: record,
                nsc_logged_in_fn=lambda: False,
                namespace_instances_for_run_fn=lambda repository, run_id: [],
                summarize_namespace_usage_fn=lambda instances: {},
            ),
            {"metadata": True},
        )

        self.assertEqual([call[0] for call in calls], ["totals", "github", "metadata"])
        self.assertIn("period_window_func", calls[0][3])
        self.assertIn("gh_api_json_fn", calls[1][3])
        self.assertIn("nsc_logged_in_fn", calls[2][2])

    def test_summary_and_run_update_helpers_forward_facade_dependencies(self):
        calls = []

        def summary_fn(record, config, **kwargs):
            calls.append(("summary", record, config, kwargs))
            return "summary"

        def update_fn(record, snapshot, **kwargs):
            calls.append(("update", record, snapshot, kwargs))
            return {"updated": True}

        self.assertEqual(
            self.mod.cloud_record_summary_with_deps(
                {"dispatch_id": "abc"},
                {"cfg": True},
                cloud_record_summary_fn=summary_fn,
                estimate_cloud_record_cost_fn=lambda record, config: {},
                format_currency_amount_fn=lambda amount, currency: "",
            ),
            "summary",
        )
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

        self.assertIn("estimate_cloud_record_cost_fn", calls[0][3])
        self.assertEqual(calls[1][3]["provider_resolved"], "namespace")


if __name__ == "__main__":
    unittest.main()
