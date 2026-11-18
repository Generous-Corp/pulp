#!/usr/bin/env python3
"""Pin codeql-advanced.yml's concurrency and its sharded C++ job.

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


class CodeqlCppShardTests(unittest.TestCase):
    """C++ is scanned in shards, not by one repository-wide extraction."""

    def cpp_job(self) -> dict:
        return load()["jobs"]["analyze-cpp"]

    def test_cpp_is_not_in_the_per_push_matrix(self) -> None:
        languages = [m["language"] for m in load()["jobs"]["analyze"]["strategy"]["matrix"]["include"]]
        self.assertNotIn("c-cpp", languages)

    def test_cpp_runs_nightly_and_on_demand_only(self) -> None:
        condition = self.cpp_job()["if"]
        self.assertIn("workflow_dispatch", condition)
        self.assertIn("schedule", condition)
        self.assertNotIn("push", condition)
        self.assertNotIn("pull_request", condition)
        self.assertIn("* * *", load()[True]["schedule"][0]["cron"].replace("23 3 ", ""))

    def test_shards_are_bounded_and_have_distinct_names_and_categories(self) -> None:
        job = self.cpp_job()
        shards = [m["shard"] for m in job["strategy"]["matrix"]["include"]]
        self.assertEqual(len(shards), len(set(shards)))
        self.assertFalse(job["strategy"]["fail-fast"])
        self.assertLessEqual(job["timeout-minutes"], 30)
        analyze = [s for s in job["steps"] if s.get("name") == "Analyze"][0]
        self.assertIn("matrix.shard", analyze["with"]["category"])

    def test_examples_shards_partition_the_examples_tree(self) -> None:
        by_name = {m["shard"]: m for m in self.cpp_job()["strategy"]["matrix"]["include"]}
        self.assertEqual(
            by_name["examples-a"]["ignore"].split(), by_name["examples-b"]["paths"].split()
        )

    def test_tools_b_takes_everything_tools_a_does_not(self) -> None:
        by_name = {m["shard"]: m for m in self.cpp_job()["strategy"]["matrix"]["include"]}
        self.assertEqual(by_name["tools-b"]["paths"], "tools")
        self.assertEqual(by_name["tools-b"]["ignore"].split(), by_name["tools-a"]["paths"].split())

    def test_core_rest_ignores_exactly_the_named_core_shards(self) -> None:
        by_name = {m["shard"]: m for m in self.cpp_job()["strategy"]["matrix"]["include"]}
        named = []
        for key in ("core-view", "core-audio-host-format", "core-timeline-canvas-platform"):
            named += by_name[key]["paths"].split()
        self.assertEqual(sorted(by_name["core-rest"]["ignore"].split()), sorted(named))

    def test_rest_shard_ignores_the_directories_that_have_their_own(self) -> None:
        by_name = {m["shard"]: m for m in self.cpp_job()["strategy"]["matrix"]["include"]}
        self.assertEqual(by_name["rest"]["ignore"].split(), ["core", "tools", "examples", "test"])

    def test_test_letter_shards_and_test_rest_agree(self) -> None:
        by_name = {m["shard"]: m for m in self.cpp_job()["strategy"]["matrix"]["include"]}
        lettered = []
        for key in ("test-a-d", "test-f-l", "test-m-r", "test-s", "test-t-z"):
            lettered += by_name[key]["paths"].split()
        self.assertEqual(
            sorted(lettered), sorted(by_name["test-rest"]["ignore"].split())
        )
        self.assertEqual(len(lettered), 26)

    def test_the_two_non_terminating_files_are_excluded_from_every_shard(self) -> None:
        step = [s for s in self.cpp_job()["steps"] if s.get("name") == "Select this shard"][0]
        for path in (
            "tools/audio/host-probe/au_effect_ab.cpp",
            "examples/pulp-delay/test_pulp_delay_ui.cpp",
        ):
            self.assertIn(path, step["run"])


if __name__ == "__main__":
    unittest.main()
