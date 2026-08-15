#!/usr/bin/env python3
"""Tests for truthful Shipyard provenance-sentinel label planning."""

from __future__ import annotations

import unittest

from shipyard_provenance_label_plan import SENTINEL, plan_labels


HEAD = "a" * 40


def pull(number: int, labels: list[str], **overrides: object) -> dict:
    result = {
        "number": number,
        "state": "open",
        "head": {"sha": HEAD},
        "labels": [{"name": label} for label in labels],
    }
    result.update(overrides)
    return result


class ProvenanceLabelPlanTests(unittest.TestCase):
    def test_unlabeled_pr_gets_truthful_sentinel_without_claiming_provenance(self) -> None:
        result = plan_labels([pull(7, [])])
        self.assertEqual(
            result["candidates"],
            [{
                "number": 7,
                "expected_head": HEAD,
                "mutation": "add",
                "label": SENTINEL,
            }],
        )

    def test_sentinel_only_is_stable(self) -> None:
        self.assertEqual(plan_labels([pull(7, [SENTINEL])])["candidates"], [])

    def test_sentinel_is_removed_after_real_labels_arrive(self) -> None:
        result = plan_labels([pull(7, [SENTINEL, "1·codex"])])
        self.assertEqual(result["candidates"][0]["mutation"], "remove")

    def test_existing_labels_are_never_rewritten(self) -> None:
        self.assertEqual(
            plan_labels([pull(7, ["1·unresolved", "5·unresolved"])])["candidates"],
            [],
        )

    def test_candidates_are_deterministically_sorted(self) -> None:
        result = plan_labels([pull(9, []), pull(2, [])])
        self.assertEqual([row["number"] for row in result["candidates"]], [2, 9])

    def test_invalid_or_duplicate_census_fails_closed(self) -> None:
        with self.assertRaisesRegex(ValueError, "duplicate open pull request"):
            plan_labels([pull(7, []), pull(7, [])])
        with self.assertRaisesRegex(ValueError, "only open"):
            plan_labels([pull(7, [], state="closed")])
        with self.assertRaisesRegex(ValueError, "full lowercase SHA"):
            plan_labels([pull(7, [], head={"sha": "ABC"})])
        with self.assertRaisesRegex(ValueError, "labels must be an array"):
            invalid_labels = pull(7, [])
            invalid_labels["labels"] = None
            plan_labels([invalid_labels])


if __name__ == "__main__":
    unittest.main()
