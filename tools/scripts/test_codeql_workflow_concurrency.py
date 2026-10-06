#!/usr/bin/env python3
"""Pin codeql-advanced.yml's concurrency.

A pull_request run shares one group per PR ref and cancels the superseded
head's analysis. Every other event (push to main, schedule, dispatch) gets a
group per run and never cancels, so no main commit loses its analysis.

Run:  python3 tools/scripts/test_codeql_workflow_concurrency.py
"""

from __future__ import annotations

import unittest
from pathlib import Path

import yaml

WORKFLOW = (
    Path(__file__).resolve().parent.parent.parent
    / ".github" / "workflows" / "codeql-advanced.yml"
)


def load() -> dict:
    return yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))


class CodeqlWorkflowConcurrencyTests(unittest.TestCase):
    def test_pull_requests_group_per_ref_and_cancel(self) -> None:
        concurrency = load()["concurrency"]
        self.assertIn(
            "github.event_name == 'pull_request' && github.ref",
            concurrency["group"],
        )
        self.assertEqual(
            concurrency["cancel-in-progress"],
            "${{ github.event_name == 'pull_request' }}",
        )

    def test_other_events_get_a_group_per_run(self) -> None:
        group = load()["concurrency"]["group"]
        self.assertIn("|| github.run_id", group)

    def test_main_still_triggers_analysis(self) -> None:
        # PyYAML reads the bare `on:` key as boolean True.
        triggers = load()[True]
        self.assertEqual(triggers["push"]["branches"], ["main"])
        self.assertIn("pull_request", triggers)


if __name__ == "__main__":
    unittest.main()
