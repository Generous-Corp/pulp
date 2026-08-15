#!/usr/bin/env python3
"""Tests for bounded superseded pull-request run cleanup planning."""

from __future__ import annotations

import unittest

from shipyard_pull_request_run_cleanup import plan_cleanup


CURRENT = "a" * 40
CURRENT_2 = "d" * 40
STALE = "b" * 40
STALE_2 = "c" * 40


def pull(number: int, head: str) -> dict:
    return {"number": number, "head": {"sha": head}}


def run(run_id: int, head: str, status: str = "queued", **overrides: object) -> dict:
    result = {
        "id": run_id,
        "event": "pull_request",
        "status": status,
        "conclusion": None,
        "head_sha": head,
        "head_branch": f"feature/pr-{run_id}",
        "name": "Build and Test",
        "created_at": f"2026-08-14T00:00:{run_id:02d}Z",
    }
    result.update(overrides)
    return result


class PullRequestRunCleanupTests(unittest.TestCase):
    def test_only_nonterminal_heads_absent_from_all_open_prs_are_candidates(self) -> None:
        result = plan_cleanup(
            [pull(7, CURRENT), pull(8, CURRENT_2)],
            [
                run(1, CURRENT),
                run(2, CURRENT_2, "in_progress"),
                run(3, STALE),
                run(4, STALE_2, "waiting"),
                run(5, STALE, "completed", conclusion="success"),
                run(6, STALE, event="pull_request_target"),
                run(7, STALE, event="merge_group"),
            ],
        )
        self.assertEqual([row["run_id"] for row in result["candidates"]], [3, 4])
        self.assertEqual(result["current_head_count"], 2)
        self.assertFalse(result["truncated"])

    def test_no_open_prs_makes_every_nonterminal_pull_request_run_stale(self) -> None:
        result = plan_cleanup([], [run(1, STALE)])
        self.assertEqual(result["candidate_count"], 1)

    def test_shared_current_head_is_preserved(self) -> None:
        result = plan_cleanup([pull(7, CURRENT), pull(8, CURRENT)], [run(1, CURRENT)])
        self.assertEqual(result["candidate_count"], 0)

    def test_candidates_are_oldest_first_and_bounded(self) -> None:
        runs = [
            run(3, STALE, created_at="2026-08-14T00:00:03Z"),
            run(1, STALE, created_at="2026-08-14T00:00:01Z"),
            run(2, STALE, created_at="2026-08-14T00:00:02Z"),
        ]
        result = plan_cleanup([pull(7, CURRENT)], runs, limit=2)
        self.assertEqual([row["run_id"] for row in result["candidates"]], [1, 2])
        self.assertEqual(result["candidate_count"], 3)
        self.assertTrue(result["truncated"])

    def test_duplicate_nonterminal_run_id_fails_closed(self) -> None:
        with self.assertRaisesRegex(ValueError, "duplicate workflow run id"):
            plan_cleanup([pull(7, CURRENT)], [run(1, STALE), run(1, STALE_2)])

    def test_invalid_open_pull_head_fails_closed(self) -> None:
        with self.assertRaisesRegex(ValueError, "full lowercase SHA"):
            plan_cleanup([pull(7, "ABC")], [])

    def test_invalid_nonterminal_run_head_fails_closed(self) -> None:
        with self.assertRaisesRegex(ValueError, "full lowercase head SHA"):
            plan_cleanup([pull(7, CURRENT)], [run(1, "short")])

    def test_unknown_nonterminal_status_fails_closed(self) -> None:
        with self.assertRaisesRegex(ValueError, "unexpected nonterminal"):
            plan_cleanup([pull(7, CURRENT)], [run(1, STALE, "mystery")])

    def test_limit_must_be_positive(self) -> None:
        with self.assertRaisesRegex(ValueError, "positive"):
            plan_cleanup([pull(7, CURRENT)], [], limit=0)


if __name__ == "__main__":
    unittest.main()
