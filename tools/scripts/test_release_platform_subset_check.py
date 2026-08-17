#!/usr/bin/env python3
"""Tests for release_platform_subset_check.py."""

from __future__ import annotations

import unittest
from datetime import datetime, timezone

from release_platform_subset_check import (
    MatrixRevision,
    active_of,
    subset_since,
)

FULL = frozenset({"darwin-arm64", "darwin-x64", "linux-x64"})
ARM = frozenset({"darwin-arm64"})


def rev(sha: str, day: int, active: frozenset[str]) -> MatrixRevision:
    return MatrixRevision(
        sha=sha,
        committed_at=datetime(2026, 8, day, tzinfo=timezone.utc),
        active=active,
        inventory=FULL,
    )


class SubsetSince(unittest.TestCase):
    def test_full_set_now_means_no_streak(self) -> None:
        self.assertIsNone(subset_since([rev("a", 10, FULL), rev("b", 5, ARM)]))

    def test_no_history_means_no_streak(self) -> None:
        self.assertIsNone(subset_since([]))

    def test_streak_dates_from_first_subset_commit(self) -> None:
        revisions = [
            rev("newest", 12, ARM),
            rev("mid", 10, ARM),
            rev("start", 8, ARM),
            rev("full", 5, FULL),
        ]
        self.assertEqual(
            subset_since(revisions),
            datetime(2026, 8, 8, tzinfo=timezone.utc),
        )

    def test_flip_back_to_full_resets_the_clock(self) -> None:
        # Subset, then full, then subset again: only the newest streak counts.
        revisions = [
            rev("newest", 12, ARM),
            rev("full-again", 11, FULL),
            rev("old-subset", 2, ARM),
        ]
        self.assertEqual(
            subset_since(revisions),
            datetime(2026, 8, 12, tzinfo=timezone.utc),
        )

    def test_reshaping_the_subset_does_not_reset_the_clock(self) -> None:
        # darwin-arm64 -> {darwin-arm64, darwin-x64} is still a subset; the
        # streak keeps its original start date.
        revisions = [
            rev("wider", 12, frozenset({"darwin-arm64", "darwin-x64"})),
            rev("narrow", 8, ARM),
            rev("full", 5, FULL),
        ]
        self.assertEqual(
            subset_since(revisions),
            datetime(2026, 8, 8, tzinfo=timezone.utc),
        )

    def test_subset_from_the_start_of_history(self) -> None:
        revisions = [rev("newest", 12, ARM), rev("oldest", 3, ARM)]
        self.assertEqual(
            subset_since(revisions),
            datetime(2026, 8, 3, tzinfo=timezone.utc),
        )


class ActiveOf(unittest.TestCase):
    def test_absent_field_means_every_platform(self) -> None:
        doc = {"platforms": ["a", "b"]}
        self.assertEqual(active_of(doc), frozenset({"a", "b"}))

    def test_declared_field_wins(self) -> None:
        doc = {"platforms": ["a", "b"], "active_platforms": ["a"]}
        self.assertEqual(active_of(doc), frozenset({"a"}))


if __name__ == "__main__":
    unittest.main(verbosity=2)
