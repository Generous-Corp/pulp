#!/usr/bin/env python3
"""Pin the properties that make the API-contract check promotable to required.

The check that enforces public API contracts used to live inside
`docs-material.yml`, a preview-site build that is not a required context. It
detected an undocumented public symbol on a PR, reported FAILURE before the
merge, and the PR merged anyway — because an advisory check cannot block. Main
stayed red for hours and four unrelated PRs inherited a red `build` none of them
caused.

Splitting the fast half into its own workflow is what makes it promotable. These
tests pin the three properties that promotion depends on, each of which fails
silently if someone "tidies" the workflow later:

* it reports on `merge_group`, or a queued group waits forever on a check that
  never arrives;
* it carries no `paths` filter on `pull_request`, or a PR that misses the filter
  leaves the required context permanently pending;
* it runs the contract pass only — dragging the site render back onto it would
  put ~50s of preview-artifact work on the merge critical path, which is the
  mistake that put example validators on the required gate.
"""

from __future__ import annotations

import pathlib
import unittest

try:
    import yaml
except ImportError:  # Required macOS CTest runners do not install PyYAML.
    yaml = None


ROOT = pathlib.Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "api-contracts.yml"
SCRIPT = ROOT / "tools" / "build-api-docs.sh"


def load_workflow() -> dict:
    if yaml is None:
        raise unittest.SkipTest("PyYAML unavailable; workflow-lint enforces YAML contracts")
    parsed = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
    if not isinstance(parsed, dict):
        raise TypeError(f"{WORKFLOW} did not parse as a mapping")
    return parsed


def triggers(workflow: dict) -> dict:
    # PyYAML 1.1 treats the unquoted key `on` as boolean true.
    return workflow.get("on", workflow.get(True, {}))


class ApiContractsWorkflowTests(unittest.TestCase):
    def setUp(self) -> None:
        self.workflow = load_workflow()
        self.jobs = self.workflow["jobs"]

    def test_reports_on_merge_group(self) -> None:
        """A required context must produce a result for a queued merge group."""
        self.assertIn("merge_group", triggers(self.workflow))

    def test_reports_on_every_pull_request(self) -> None:
        """A `paths` filter would leave the required context permanently pending."""
        pull_request = triggers(self.workflow)["pull_request"]
        if pull_request is not None:
            self.assertNotIn("paths", pull_request)
            self.assertNotIn("paths-ignore", pull_request)

    def test_single_job_so_the_context_name_is_stable(self) -> None:
        """Branch protection names a check; more than one job makes that ambiguous."""
        self.assertEqual(list(self.jobs), ["api-contracts"])
        self.assertEqual(self.jobs["api-contracts"]["name"], "api-contracts")

    def test_runs_the_contract_pass_only(self) -> None:
        """The published HTML render must stay off the merge critical path."""
        script = "\n".join(
            str(step.get("run", "")) for step in self.jobs["api-contracts"]["steps"]
        )
        self.assertIn("build-api-docs.sh --contract-only", script)
        self.assertNotIn("mkdocs", script)

    def test_job_is_bounded(self) -> None:
        self.assertIn("timeout-minutes", self.jobs["api-contracts"])

    def test_contract_only_mode_exists(self) -> None:
        """The workflow's one command must be a mode the script actually has."""
        self.assertIn("--contract-only", SCRIPT.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
