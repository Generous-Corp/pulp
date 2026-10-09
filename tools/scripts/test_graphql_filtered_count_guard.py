#!/usr/bin/env python3
"""Self-test for graphql_filtered_count_guard.py."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import graphql_filtered_count_guard as guard  # noqa: E402


class FindOffencesTest(unittest.TestCase):
    def test_filtered_total_count_is_flagged(self) -> None:
        text = ('q = "pullRequest{timelineItems(itemTypes:'
                '[REMOVED_FROM_MERGE_QUEUE_EVENT]){totalCount}}"\n')
        self.assertEqual(guard.find_offences(text), (1, [1]))

    def test_split_python_string_is_flagged(self) -> None:
        text = ('Q = (\n    "state "\n'
                '    "timelineItems(first:1,itemTypes:[X_EVENT]){"\n'
                '    "nodes{__typename} totalCount} "\n)\n')
        self.assertEqual(guard.find_offences(text), (1, [3]))

    def test_format_string_braces_are_flagged(self) -> None:
        text = ('let q = format!("{{timelineItems(itemTypes:[X],first:1)'
                '{{totalCount}}}}");')
        self.assertEqual(guard.find_offences(text), (1, [1]))

    def test_filtered_nodes_and_filtered_count_pass(self) -> None:
        text = ('timelineItems(first:1,itemTypes:[REMOVED_FROM_MERGE_QUEUE_EVENT])'
                '{filteredCount nodes{__typename}}')
        self.assertEqual(guard.find_offences(text), (1, []))

    def test_nested_total_count_is_not_the_connection(self) -> None:
        text = ('timelineItems(last:5,itemTypes:[PULL_REQUEST_COMMIT]){nodes{'
                '... on PullRequestCommit{commit{parents{totalCount}}}}} '
                'commits{totalCount}')
        self.assertEqual(guard.find_offences(text), (1, []))

    def test_unfiltered_total_count_is_allowed(self) -> None:
        # Without itemTypes, totalCount means what it says.
        self.assertEqual(guard.find_offences("timelineItems{totalCount}"),
                         (0, []))
        self.assertEqual(
            guard.find_offences("timelineItems(first:1){totalCount}"), (0, []))


class RepoScanTest(unittest.TestCase):
    def test_repo_is_clean_and_the_scan_reaches_known_queries(self) -> None:
        seen, problems = guard.scan(guard.REPO)
        self.assertEqual(problems, [])
        # Control: the bump-PR liveness query and the ejection readers are
        # filtered timeline selections, so a scan that saw fewer reached nothing.
        self.assertGreaterEqual(seen, 5)
        _, hits = guard.scan(guard.REPO, ["tools/scripts/version_at_land.py"])
        self.assertEqual(hits, [])
        n, _ = guard.scan(guard.REPO, ["tools/scripts/version_at_land.py"])
        self.assertEqual(n, 1)


if __name__ == "__main__":
    unittest.main()
