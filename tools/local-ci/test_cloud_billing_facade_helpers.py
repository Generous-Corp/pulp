#!/usr/bin/env python3
"""Tests for cloud billing facade dependency wiring helpers."""

import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("cloud_billing_facade_helpers.py")


class CloudBillingFacadeHelpersTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_billing_and_metadata_helpers_forward_dependencies(self):
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


if __name__ == "__main__":
    unittest.main()
