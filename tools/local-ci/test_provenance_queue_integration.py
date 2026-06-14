#!/usr/bin/env python3
"""Facade-level provenance and queue supersedence integration tests."""

from __future__ import annotations

import pathlib
import tempfile
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("local_ci.py", module_name="pulp_local_ci_provenance_queue_integration", add_module_dir=True)


class ProvenanceQueueIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_provenance_job_and_supersedence_helpers_cover_edges(self) -> None:
        hosted = {
            "execution_kind": "hosted",
            "hosted_orchestrator": "github-actions",
            "runner_provider": "namespace",
            "runner_selector": "linux-large",
            "run_id": "123",
        }
        self.assertEqual(
            self.mod.provenance_summary(hosted),
            "hosted via github-actions/namespace selector=linux-large run=123",
        )
        self.assertEqual(
            self.mod.provenance_summary({"direct_backend": ""}),
            "direct via local-ci",
        )
        self.assertEqual(
            self.mod.normalize_result({"submission": {"provenance": hosted}})["provenance"]["runner_provider"],
            "namespace",
        )

        legacy = self.mod.normalize_job(
            {
                "branch": "feature/local-ci",
                "sha": "a" * 40,
                "queued_at": "2026-04-30T00:00:00+00:00",
                "targets": ["windows", "mac", "windows"],
                "validation": " SMOKE ",
            }
        )
        self.assertEqual(len(legacy["id"]), 12)
        self.assertEqual(legacy["priority"], "normal")
        self.assertEqual(legacy["targets"], ["mac", "windows"])
        self.assertEqual(legacy["validation"], "smoke")
        self.assertEqual(legacy["submission"]["provenance"]["direct_backend"], "local-ci")
        self.assertIn("validation=smoke", self.mod.summarize_job(legacy))
        self.assertEqual(self.mod.summarize_active_targets(None), "")
        self.assertEqual(
            self.mod.summarize_active_targets(
                {"windows": {"status": "running"}, "mac": {"status": "queued"}},
                preferred_order=["mac"],
            ),
            "mac=queued, windows=running",
        )

        older = {
            "id": "older",
            "branch": "feature/local-ci",
            "sha": "a" * 40,
            "fingerprint": "old",
            "targets": ["mac", "windows"],
            "validation": "full",
            "priority": "normal",
            "queued_at": "2026-04-30T00:00:00+00:00",
        }
        newer_same_scope = dict(older, id="newer", sha="b" * 40, fingerprint="new")
        newer_narrower = dict(older, id="narrow", fingerprint="narrow", targets=["mac"])
        self.assertTrue(self.mod.jobs_share_supersedence_scope(newer_same_scope, older))
        self.assertEqual(self.mod.supersedence_reason(newer_same_scope, older), "newer_sha_queued")
        self.assertEqual(self.mod.supersedence_reason(newer_narrower, older), "narrower_scope_queued")
        self.assertIsNone(self.mod.supersedence_reason(older, older))
        self.assertEqual(
            self.mod.supersedence_result(older, "newer", "newer_sha_queued")["overall"],
            "superseded",
        )
        self.assertEqual(self.mod.cancellation_result(older, "operator")["canceled_reason"], "operator")



if __name__ == "__main__":
    unittest.main()
