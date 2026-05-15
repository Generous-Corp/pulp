#!/usr/bin/env python3
"""Structural tests for the ruleset drift workflow.

These tests keep the checked-in branch protection intent aligned with the
workflow that compares it to GitHub's live ruleset. They are intentionally
offline: no GitHub API calls, no workflow dispatch, and no runner access.

Run:
    python3 tools/scripts/test_ruleset_drift_config.py
"""

from __future__ import annotations

import json
import pathlib
import unittest

import yaml


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "ruleset-drift-check.yml"
RULESET = REPO_ROOT / ".github" / "rulesets" / "main-protection.json"

EXPECTED_REQUIRED_CONTEXTS = {
    "macos",
    "linux",
    "windows",
    "Enforce version & skill sync",
}

EXPECTED_ADVISORY_CONTEXTS = {
    "AddressSanitizer (macOS ARM64)",
    "ThreadSanitizer (macOS ARM64)",
    "UndefinedBehaviorSanitizer (macOS ARM64)",
    "RealtimeSanitizer (Linux x86_64, Clang 18)",
}


def workflow_on(doc: dict) -> dict:
    """Return the GitHub Actions `on:` block from a PyYAML-loaded document."""
    # PyYAML's YAML 1.1 resolver treats the bare word "on" as boolean True.
    # GitHub Actions treats it as a literal key.
    return doc.get("on") or doc.get(True) or {}


def required_contexts(ruleset: dict) -> set[str]:
    contexts: set[str] = set()
    for rule in ruleset.get("rules", []):
        if rule.get("type") != "required_status_checks":
            continue
        params = rule.get("parameters", {})
        for check in params.get("required_status_checks", []):
            context = check.get("context")
            if context:
                contexts.add(context)
    return contexts


def advisory_contexts(ruleset: dict) -> set[str]:
    advisory = ruleset.get("advisory_status_checks", {})
    return {
        check["context"]
        for check in advisory.get("checks", [])
        if check.get("context")
    }


class RulesetDriftConfig(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow_text = WORKFLOW.read_text(encoding="utf-8")
        cls.workflow = yaml.safe_load(cls.workflow_text)
        cls.ruleset = json.loads(RULESET.read_text(encoding="utf-8"))

    def test_pull_request_trigger_is_scoped_to_ruleset_files(self) -> None:
        trigger = workflow_on(self.workflow)
        pull_request = trigger.get("pull_request", {})

        self.assertEqual(pull_request.get("branches"), ["main"])
        self.assertEqual(
            set(pull_request.get("paths", [])),
            {
                ".github/rulesets/**",
                ".github/workflows/ruleset-drift-check.yml",
            },
        )

    def test_manual_trigger_can_override_drift_failure(self) -> None:
        trigger = workflow_on(self.workflow)
        dispatch = trigger.get("workflow_dispatch", {})
        fail_on_drift = dispatch.get("inputs", {}).get("fail_on_drift", {})

        self.assertEqual(
            fail_on_drift.get("default"),
            "",
            "manual runs should default to the workflow's event-sensitive "
            "failure policy instead of forcing failures on every dispatch",
        )
        self.assertFalse(fail_on_drift.get("required", True))

    def test_workflow_points_at_checked_in_ruleset_name(self) -> None:
        diff_job = self.workflow["jobs"]["diff"]
        env = diff_job["env"]

        self.assertEqual(
            env["CHECKED_IN_PATH"],
            ".github/rulesets/main-protection.json",
        )
        self.assertEqual(env["RULESET_NAME"], self.ruleset["name"])
        self.assertEqual(self.ruleset["target"], "branch")
        self.assertEqual(self.ruleset["source_type"], "Repository")
        self.assertEqual(self.ruleset["enforcement"], "active")

    def test_required_and_advisory_contexts_stay_separate(self) -> None:
        required = required_contexts(self.ruleset)
        advisory = advisory_contexts(self.ruleset)

        self.assertEqual(required, EXPECTED_REQUIRED_CONTEXTS)
        self.assertEqual(advisory, EXPECTED_ADVISORY_CONTEXTS)
        self.assertTrue(
            required.isdisjoint(advisory),
            "slow sanitizer contexts must remain advisory-only unless branch "
            "protection policy changes explicitly",
        )

    def test_fetch_filters_out_inherited_org_rulesets(self) -> None:
        self.assertIn(
            "includes_parents=false",
            self.workflow_text,
            "ruleset drift fetch must ignore inherited org rulesets so a "
            "same-named org ruleset cannot mask repo-local drift",
        )
        self.assertIn(
            "source_type')=='Repository'",
            self.workflow_text,
            "ruleset id selection must require Repository source_type",
        )

    def test_pr_comment_is_fork_safe_and_scheduled_drift_fails(self) -> None:
        self.assertIn(
            "github.event.pull_request.head.repo.full_name == github.repository",
            self.workflow_text,
            "PR drift comments must be skipped for fork heads because their "
            "GITHUB_TOKEN cannot write issue comments",
        )
        self.assertIn(
            "steps.diff.outputs.drift == 'true' && github.event_name != 'pull_request'",
            self.workflow_text,
            "scheduled/manual drift must surface as a failing check while PR "
            "drift remains advisory",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
