#!/usr/bin/env python3
"""Tests for bounded stale merge-group run cleanup planning."""

from __future__ import annotations

import unittest
from typing import Optional

from shipyard_merge_queue_run_cleanup import plan_cleanup


CURRENT = "a" * 40
STALE = "b" * 40
STALE_2 = "c" * 40


def queue(*heads: Optional[str]) -> dict:
    return {
        "data": {
            "repository": {
                "mergeQueue": {
                    "entries": {
                        "nodes": [
                            {"headCommit": None if head is None else {"oid": head}}
                            for head in heads
                        ]
                    }
                }
            }
        }
    }


def run(run_id: int, head: str, status: str = "queued", **overrides: object) -> dict:
    result = {
        "id": run_id,
        "event": "merge_group",
        "status": status,
        "conclusion": None,
        "head_sha": head,
        "head_branch": f"gh-readonly-queue/main/pr-{run_id}-base",
        "name": "Build and Test",
        "created_at": f"2026-08-14T00:00:{run_id:02d}Z",
    }
    result.update(overrides)
    return result


class MergeQueueRunCleanupTests(unittest.TestCase):
    def test_only_nonterminal_exact_heads_absent_from_current_queue_are_candidates(self) -> None:
        result = plan_cleanup(
            queue(CURRENT, None),
            [
                run(1, CURRENT),
                run(2, STALE, "queued"),
                run(3, STALE_2, "in_progress"),
                run(4, STALE, "completed", conclusion="success"),
                run(5, STALE, event="pull_request"),
            ],
        )
        self.assertEqual([row["run_id"] for row in result["candidates"]], [2, 3])
        self.assertEqual(result["current_head_count"], 1)
        self.assertFalse(result["truncated"])

    def test_empty_queue_makes_every_nonterminal_merge_group_run_stale(self) -> None:
        result = plan_cleanup(queue(), [run(1, STALE)])
        self.assertEqual(result["candidate_count"], 1)

    def test_candidates_are_oldest_first_and_bounded(self) -> None:
        runs = [
            run(3, STALE, created_at="2026-08-14T00:00:03Z"),
            run(1, STALE, created_at="2026-08-14T00:00:01Z"),
            run(2, STALE, created_at="2026-08-14T00:00:02Z"),
        ]
        result = plan_cleanup(queue(CURRENT), runs, limit=2)
        self.assertEqual([row["run_id"] for row in result["candidates"]], [1, 2])
        self.assertEqual(result["candidate_count"], 3)
        self.assertTrue(result["truncated"])

    def test_duplicate_nonterminal_run_id_fails_closed(self) -> None:
        with self.assertRaisesRegex(ValueError, "duplicate workflow run id"):
            plan_cleanup(queue(CURRENT), [run(1, STALE), run(1, STALE_2)])

    def test_invalid_queue_head_fails_closed(self) -> None:
        with self.assertRaisesRegex(ValueError, "full lowercase SHA"):
            plan_cleanup(queue("ABC"), [])

    def test_invalid_nonterminal_run_head_fails_closed(self) -> None:
        with self.assertRaisesRegex(ValueError, "full lowercase head SHA"):
            plan_cleanup(queue(CURRENT), [run(1, "short")])

    def test_unknown_nonterminal_status_fails_closed(self) -> None:
        with self.assertRaisesRegex(ValueError, "unexpected nonterminal"):
            plan_cleanup(queue(CURRENT), [run(1, STALE, "mystery")])

    def test_limit_must_be_positive(self) -> None:
        with self.assertRaisesRegex(ValueError, "positive"):
            plan_cleanup(queue(CURRENT), [], limit=0)


if __name__ == "__main__":
    unittest.main()
