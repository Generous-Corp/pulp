#!/usr/bin/env python3
"""Pin cross-platform-check.yml's concurrency and tracking-issue scope.

Dispatches from different branches must run in parallel (one group per ref),
two runs of one ref must still queue rather than cancel, only main's runs may
open or close the per-platform tracking issues, and the issue's bisect anchor
(the last green run) must come from main, never from a proof branch.

Run:  python3 tools/scripts/test_cross_platform_check_concurrency.py
"""

from __future__ import annotations

import unittest
from pathlib import Path

import yaml

WORKFLOW = (
    Path(__file__).resolve().parent.parent.parent
    / ".github" / "workflows" / "cross-platform-check.yml"
)


def load() -> dict:
    return yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))


class CrossPlatformCheckConcurrencyTests(unittest.TestCase):
    def test_concurrency_is_per_ref_and_queues(self) -> None:
        concurrency = load()["concurrency"]
        self.assertIn("${{ github.ref }}", concurrency["group"])
        self.assertIs(concurrency["cancel-in-progress"], False)

    def test_only_main_maintains_tracking_issues(self) -> None:
        condition = load()["jobs"]["tracking-issues"]["if"]
        self.assertIn("always()", condition)
        self.assertIn("github.ref == 'refs/heads/main'", condition)

    def test_last_green_anchor_is_read_from_main_only(self) -> None:
        steps = load()["jobs"]["tracking-issues"]["steps"]
        script = next(s["run"] for s in steps if s.get("id") == "range")
        self.assertIn("runs?status=success&branch=main", script)


if __name__ == "__main__":
    unittest.main()
