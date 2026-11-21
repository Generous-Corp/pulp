#!/usr/bin/env python3
"""Focused tests for the fail-closed flake-rerun-once policy."""
from __future__ import annotations

import sys
import unittest
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import flake_rerun_once as fr  # noqa: E402


@dataclass
class Main:
    conclusion: str = "success"
    failing: frozenset[str] = frozenset()
    read: bool = True


def run(**extra):
    value = {"event": "pull_request", "workflow_name": fr.SANITIZER_WORKFLOW,
             "run_attempt": 1, "conclusion": "failure"}
    value.update(extra)
    return value


def eligible(**kwargs):
    return fr.decide(run(), ["WakeSignal no-lost-wakeup"], Main(),
                     ["docs/readme.md"], {"WakeSignal no-lost-wakeup": ["core/wake"]}, **kwargs)


class FlakeRerunOnceTests(unittest.TestCase):
    def test_all_conditions_are_true(self):
        self.assertTrue(eligible()["eligible"])

    def test_main_failure_named_test_is_false(self):
        self.assertFalse(fr.decide(run(), ["WakeSignal no-lost-wakeup"],
                                   Main("failure", frozenset({"WakeSignal no-lost-wakeup"})),
                                   ["docs/readme.md"],
                                   {"WakeSignal no-lost-wakeup": ["core/wake"]})["eligible"])

    def test_changed_source_surface_is_false(self):
        self.assertFalse(fr.decide(run(), ["WakeSignal no-lost-wakeup"], Main(),
                                   ["core/wake/wake.cpp"],
                                   {"WakeSignal no-lost-wakeup": ["core/wake"]})["eligible"])

    def test_deterministic_tier_is_false(self):
        self.assertFalse(eligible(deterministic={"WakeSignal no-lost-wakeup"})["eligible"])

    def test_second_attempt_is_never_rerun(self):
        self.assertFalse(fr.decide(run(run_attempt=2), ["WakeSignal no-lost-wakeup"], Main(),
                                   ["docs/readme.md"],
                                   {"WakeSignal no-lost-wakeup": ["core/wake"]})["eligible"])

    def test_merge_group_is_never_rerun(self):
        self.assertFalse(fr.decide(run(event="merge_group"), ["WakeSignal no-lost-wakeup"], Main(),
                                   ["docs/readme.md"],
                                   {"WakeSignal no-lost-wakeup": ["core/wake"]})["eligible"])

    def test_exactly_one_failure_is_required(self):
        self.assertFalse(fr.decide(run(), ["one", "two"], Main(), [], {})["eligible"])

    def test_unknown_mapping_means_no_rerun(self):
        self.assertFalse(fr.decide(run(), ["WakeSignal no-lost-wakeup"], Main(),
                                   ["docs/readme.md"], {})["eligible"])


if __name__ == "__main__":
    unittest.main()
