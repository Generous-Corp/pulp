#!/usr/bin/env python3
"""Contract tests for trusted scheduled-control runner routing."""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
WORKFLOWS = {
    "required-gate-liveness.yml": {
        "name": "Required gate liveness",
        "job": "audit",
        "permissions": ("contents: read", "checks: read", "issues: write"),
        "concurrency": "group: required-gate-liveness",
        "push": True,
    },
    "stale-run-reaper.yml": {
        "name": "Stale run reaper",
        "job": "reap",
        "permissions": ("actions: write", "contents: read"),
        "concurrency": "group: stale-run-reaper",
        "push": False,
    },
    "pending-intent-liveness.yml": {
        "name": "Pending-intent liveness",
        "job": "check",
        "permissions": ("contents: read", "issues: write"),
        "concurrency": "group: pending-intent-liveness",
        "push": False,
    },
}
LOCAL_SELECTOR = (
    '["self-hosted","Linux","X64","pulp-build-linux-x64",'
    '"pulp-host-macpro"]'
)
ROUTE = (
    "runs-on: ${{ fromJSON(vars.PULP_SCHEDULED_CONTROLS_USE_LOCAL == '1' && '"
    + LOCAL_SELECTOR
    + "' || '\"ubuntu-latest\"') }}"
)


def workflow_text(name: str) -> str:
    return (ROOT / ".github" / "workflows" / name).read_text()


class ScheduledControlRunnerRoutingTests(unittest.TestCase):
    def test_each_trusted_control_uses_one_operator_route_with_hosted_fallback(self) -> None:
        for name in WORKFLOWS:
            with self.subTest(workflow=name):
                text = workflow_text(name)
                self.assertEqual(text.count(ROUTE), 1)
                self.assertNotIn("runs-on: ubuntu-latest", text)

    def test_selector_is_not_reused_by_any_other_workflow(self) -> None:
        users = []
        for path in sorted((ROOT / ".github" / "workflows").glob("*.yml")):
            if "PULP_SCHEDULED_CONTROLS_USE_LOCAL" in path.read_text():
                users.append(path.name)
        self.assertEqual(users, sorted(WORKFLOWS))

    def test_boolean_switch_selects_only_the_hard_coded_approved_labels(self) -> None:
        for name in WORKFLOWS:
            with self.subTest(workflow=name):
                text = workflow_text(name)
                self.assertIn(
                    "PULP_SCHEDULED_CONTROLS_USE_LOCAL == '1' && '"
                    + LOCAL_SELECTOR
                    + "'",
                    text,
                )
                self.assertNotIn(
                    "PULP_SCHEDULED_CONTROL_RUNS_ON_JSON", text
                )

    def test_public_workflow_contracts_remain_stable(self) -> None:
        for name, contract in WORKFLOWS.items():
            with self.subTest(workflow=name):
                text = workflow_text(name)
                self.assertRegex(text, rf"(?m)^name: {re.escape(contract['name'])}$")
                self.assertRegex(text, r"(?m)^  schedule:$")
                self.assertRegex(text, r"(?m)^  workflow_dispatch:$")
                self.assertNotIn("pull_request_target", text)
                self.assertNotRegex(text, r"(?m)^  (pull_request|merge_group):$")
                self.assertEqual(bool(re.search(r"(?m)^  push:$", text)), contract["push"])
                self.assertRegex(text, rf"(?m)^  {contract['job']}:$")
                self.assertIn(contract["concurrency"], text)
                for permission in contract["permissions"]:
                    self.assertIn(permission, text)

    def test_schedule_entry_jobs_keep_primary_repository_guards(self) -> None:
        required = workflow_text("required-gate-liveness.yml")
        self.assertIn("if: github.repository == vars.PULP_PRIMARY_REPO", required)
        for name in ("stale-run-reaper.yml", "pending-intent-liveness.yml"):
            with self.subTest(workflow=name):
                self.assertIn(
                    "if: github.event_name != 'schedule' || "
                    "github.repository == vars.PULP_PRIMARY_REPO",
                    workflow_text(name),
                )

    def test_skill_documents_exact_scope_selector_and_assignment_boundary(self) -> None:
        skill = (ROOT / ".agents" / "skills" / "ci" / "SKILL.md").read_text()
        self.assertIn("PULP_SCHEDULED_CONTROLS_USE_LOCAL", skill)
        self.assertIn(LOCAL_SELECTOR, skill)
        for control in (
            "Required gate liveness",
            "Stale run reaper",
            "Pending-intent liveness",
        ):
            self.assertIn(control, skill)
        for excluded in ("pull_request_target", "secret-bearing", "Vellum trusted"):
            self.assertIn(excluded, skill)
        self.assertIn("cannot migrate an already queued hosted job", skill)

    def test_workflow_lint_executes_this_contract(self) -> None:
        lint = workflow_text("workflow-lint.yml")
        script = "tools/scripts/test_scheduled_control_runner_routing.py"
        self.assertGreaterEqual(lint.count(script), 3)


if __name__ == "__main__":
    unittest.main()
