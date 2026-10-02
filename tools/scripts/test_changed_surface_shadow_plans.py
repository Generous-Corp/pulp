#!/usr/bin/env python3
"""Tests for the changed-surface shadow-plan ranking and its workflow."""
from __future__ import annotations

import io
import json
import re
import sys
import unittest
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import changed_surface_shadow_plans as shadow  # noqa: E402

REPO_ROOT = HERE.parents[1]
WORKFLOW = REPO_ROOT / ".github/workflows" / shadow.WORKFLOW
RULESET = REPO_ROOT / ".github/rulesets/main-protection.json"


def record(pr: int, head: str | None, reason: str | None = None, *, suite: str = "full",
           outcome: str = "planned", origin: str = shadow.ORIGIN) -> dict:
    return {"origin": origin, "shadow_only": True, "pull_request": pr, "head_sha": head,
            "outcome": outcome, "planned_suite": None if outcome == "planner_error" else suite,
            "planner_reason": "planner_error" if outcome == "planner_error" else reason}


class RankTest(unittest.TestCase):
    def test_reasons_are_ranked_with_an_example_each(self) -> None:
        result = shadow.rank(
            [record(1, "a", "stale_base"), record(2, "b", "stale_base"),
             record(3, "c", suite="bounded"), record(4, "d", "policy_path_changed")],
            [(1, "a"), (2, "b"), (3, "c"), (4, "d")])
        self.assertEqual(result["reasons"][0], {"reason": "stale_base", "count": 2, "example_pr": 1})
        self.assertIn({"reason": "bounded", "count": 1, "example_pr": 3}, result["reasons"])
        self.assertEqual((result["plans"], result["heads"], result["plans_per_head"]), (4, 4, 1.0))

    def test_a_head_without_a_record_is_a_missing_plan_not_a_dropped_one(self) -> None:
        result = shadow.rank([record(1, "a", "stale_base")], [(1, "a"), (2, "b")])
        self.assertEqual(result["plans_per_head"], 0.5)
        self.assertEqual(result["missing"], [{"pull_request": 2, "head_sha": "b"}])
        self.assertIn({"reason": "missing_record", "count": 1, "example_pr": 2}, result["reasons"])

    def test_a_record_for_an_older_head_does_not_cover_the_new_one(self) -> None:
        result = shadow.rank([record(1, "old", "stale_base")], [(1, "new")])
        self.assertEqual(result["plans"], 0)

    def test_a_lane_plan_never_enters_the_shadow_ranking(self) -> None:
        lane = record(1, "a", "base_policy_mismatch", origin="lane")
        unstamped = {**record(2, "b", "stale_base"), "shadow_only": False}
        result = shadow.rank([lane, unstamped], [(1, "a"), (2, "b")])
        self.assertEqual(result["plans"], 0)

    def test_a_planner_error_is_a_labelled_plan(self) -> None:
        result = shadow.rank([record(5, None, outcome="planner_error")], [(5, "e")])
        self.assertEqual(result["plans"], 1)
        self.assertEqual(result["reasons"], [{"reason": "planner_error", "count": 1, "example_pr": 5}])

    def test_a_full_plan_without_a_reason_is_unlabelled(self) -> None:
        self.assertEqual(shadow.reason_of(record(1, "a")), "unlabelled")

    def test_records_are_read_from_the_uploaded_artifact(self) -> None:
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as zf:
            zf.writestr("Generous-Corp%2Fpulp/1/a/mac.json", json.dumps(record(1, "a", "stale_base")))
            zf.writestr("notes.txt", "ignored")
        self.assertEqual(shadow.records_from_zip(buffer.getvalue()), [record(1, "a", "stale_base")])


class WorkflowContractTest(unittest.TestCase):
    """The shadow plan informs; it must never gate, execute, or slow a merge.
    Read as text: the gate lane's Python carries no YAML parser."""

    def setUp(self) -> None:
        self.text = WORKFLOW.read_text(encoding="utf-8")
        blocks = re.split(r"(?m)^      - ", self.text)
        self.steps = {}
        for block in blocks[1:]:
            match = re.match(r"(?:name|uses): (.+)", block)
            if match:
                self.steps[match.group(1).strip()] = block

    def value(self, step: str, key: str) -> str | None:
        match = re.search(rf"(?m)^\s+{re.escape(key)}: (.+)$", self.steps[step])
        return match.group(1).strip() if match else None

    def test_it_is_not_a_required_context_and_not_on_the_merge_queue(self) -> None:
        ruleset = json.loads(RULESET.read_text(encoding="utf-8"))
        required = {check["context"] for rule in ruleset["rules"]
                    if rule["type"] == "required_status_checks"
                    for check in rule["parameters"]["required_status_checks"]}
        self.assertIn("drift-fast", required)  # control: the instrument reads real contexts
        name = re.search(r"(?m)^    name: (.+)$", self.text).group(1).strip()
        self.assertEqual(name, "changed-surface shadow plan (advisory)")
        self.assertNotIn(name, required)
        triggers = re.search(r"(?ms)^on:\n(.*?)^\S", self.text).group(1)
        self.assertEqual(re.findall(r"(?m)^  (\w+):", triggers), ["pull_request"])

    def test_it_plans_the_exact_head_and_never_fails_the_job(self) -> None:
        self.assertEqual(self.value("actions/checkout@v5", "ref"),
                         "${{ github.event.pull_request.head.sha }}")
        self.assertEqual(self.value("actions/checkout@v5", "fetch-depth"), "0")
        for name in ("Install pinned Shipyard", "Record shadow plan"):
            self.assertEqual(self.value(name, "continue-on-error"), "true", name)
        self.assertIn("--record", self.steps["Record shadow plan"])
        self.assertLessEqual(int(self.value("Record shadow plan", "timeout-minutes")), 3)

    def test_the_record_is_uploaded_where_the_ranking_reads_it(self) -> None:
        upload = "Upload shadow plan record"
        self.assertEqual(self.value(upload, "if"), "always()")
        self.assertEqual(self.value(upload, "name"), shadow.ARTIFACT)
        self.assertEqual(self.value(upload, "retention-days"), "90")
        record_dir = self.value("Record shadow plan", "RECORD_DIR")
        self.assertEqual(self.value(upload, "path"), record_dir)
        self.assertTrue(record_dir.startswith("${{ runner.temp }}"))


if __name__ == "__main__":
    unittest.main()
