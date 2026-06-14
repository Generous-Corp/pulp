#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("reporting_publish_branch.py")


class ReportingPublishBranchTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_branch_publish_metadata_without_remote_keeps_paths_only(self):
        published = self.mod.desktop_branch_publish_metadata(
            {"runs": [{"label": "ignored"}]},
            branch="desktop-artifacts",
            report_name="20260612-gallery",
            remote_base=None,
        )

        self.assertEqual(
            published,
            {
                "mode": "branch",
                "branch": "desktop-artifacts",
                "report_path": "desktop-automation/reports/20260612-gallery",
                "latest_path": "desktop-automation/latest",
            },
        )

    def test_branch_publish_metadata_adds_remote_urls_and_run_artifacts(self):
        published = self.mod.desktop_branch_publish_metadata(
            {
                "runs": [
                    {
                        "label": "ui-preview",
                        "target": "mac",
                        "action": "click",
                        "artifacts": {
                            "screenshot": "assets/run-01/window.png",
                            "image_change": {"changed": True},
                            "missing": None,
                        },
                    }
                ]
            },
            branch="desktop-artifacts",
            report_name="20260612-gallery",
            remote_base="https://github.com/danielraffel/pulp",
        )

        self.assertEqual(published["branch_url"], "https://github.com/danielraffel/pulp/tree/desktop-artifacts")
        self.assertEqual(
            published["report_url"],
            "https://github.com/danielraffel/pulp/tree/desktop-artifacts/desktop-automation/reports/20260612-gallery",
        )
        self.assertEqual(
            published["latest_index_json_url"],
            "https://github.com/danielraffel/pulp/blob/desktop-artifacts/desktop-automation/latest/index.json",
        )
        self.assertEqual(published["runs"][0]["label"], "ui-preview")
        self.assertEqual(
            published["runs"][0]["artifact_urls"],
            {
                "screenshot": "https://github.com/danielraffel/pulp/blob/desktop-artifacts/desktop-automation/latest/assets/run-01/window.png"
            },
        )

    def test_published_run_payload_handles_missing_artifacts(self):
        payload = self.mod.desktop_published_run_payload(
            {"label": "run", "target": "windows", "action": "smoke"},
            remote_base="https://github.com/danielraffel/pulp",
            branch="desktop-artifacts",
        )

        self.assertEqual(payload["artifact_urls"], {})
        self.assertEqual(payload["target"], "windows")


if __name__ == "__main__":
    unittest.main()
