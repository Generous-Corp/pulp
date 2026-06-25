#!/usr/bin/env python3
"""Tests for the provenance normalize/summary helpers."""

import unittest

import provenance



class ProvenanceTests(unittest.TestCase):
    def setUp(self):
        self.mod = provenance

    def test_extracted_provenance_helpers_normalize_result_and_summarize(self):
        defaults = self.mod.normalize_provenance()
        self.assertEqual(defaults["execution_kind"], "direct")
        self.assertEqual(defaults["control_plane"], "pulp-ci-local")
        self.assertEqual(defaults["direct_backend"], "local-ci")
        self.assertEqual(defaults["hosted_orchestrator"], "")

        direct = self.mod.normalize_provenance({"direct_backend": ""})
        self.assertEqual(direct["execution_kind"], "direct")
        self.assertEqual(direct["direct_backend"], "")
        self.assertEqual(self.mod.provenance_summary(direct), "direct via local-ci")

        hosted = self.mod.normalize_provenance(
            {
                "execution_kind": "hosted",
                "hosted_orchestrator": "github-actions",
                "runner_provider": "github-hosted",
                "runner_selector": "macos-15",
                "run_id": "12345",
            }
        )
        self.assertEqual(
            self.mod.provenance_summary(hosted),
            "hosted via github-actions/github-hosted selector=macos-15 run=12345",
        )

        hosted_without_provider = self.mod.normalize_provenance(
            {
                "execution_kind": "hosted",
                "hosted_orchestrator": "manual-dispatch",
            }
        )
        self.assertEqual(
            self.mod.provenance_summary(hosted_without_provider),
            "hosted via manual-dispatch",
        )

        result = self.mod.normalize_result({"submission": {"provenance": hosted}})
        self.assertEqual(result["provenance"]["run_id"], "12345")



if __name__ == "__main__":
    unittest.main()
