#!/usr/bin/env python3
"""Static regression tests for .github/workflows/workflow-lint.yml.

The workflow lint gate is the first release-watchdog layer: it must run
on CI-definition changes and keep the three local checks that catch
workflow-file failures before merge.

Run:
    python3 tools/scripts/test_workflow_lint.py
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "workflow-lint.yml"


def _workflow_text() -> str:
    return WORKFLOW.read_text(encoding="utf-8")


def _find_step(text: str, name: str) -> str:
    pattern = re.compile(
        rf"^\s{{6}}-\s+name:\s+{re.escape(name)}\s*\n"
        r"([\s\S]*?)(?=^\s{6}-\s+(?:name:|uses:)|\Z)",
        re.MULTILINE,
    )
    match = pattern.search(text)
    if not match:
        raise AssertionError(f"could not find workflow step named {name!r}")
    return match.group(0)


def _find_uses_step(text: str, uses: str) -> str:
    pattern = re.compile(
        rf"^\s{{6}}-\s+uses:\s+{re.escape(uses)}\s*\n"
        r"([\s\S]*?)(?=^\s{6}-\s+(?:name:|uses:)|\Z)",
        re.MULTILINE,
    )
    match = pattern.search(text)
    if not match:
        raise AssertionError(f"could not find workflow uses step {uses!r}")
    return match.group(0)


class WorkflowLintWorkflowTests(unittest.TestCase):
    def setUp(self) -> None:
        self.assertTrue(WORKFLOW.exists(), f"missing workflow: {WORKFLOW}")
        self.text = _workflow_text()

    def test_trigger_scope_covers_workflow_and_action_changes(self) -> None:
        self.assertRegex(self.text, r"(?m)^on:\s*$")
        self.assertRegex(self.text, r"(?m)^\s{2}pull_request:\s*$")
        self.assertRegex(self.text, r"(?m)^\s{2}push:\s*$")
        self.assertRegex(self.text, r"(?m)^\s{4}branches:\s*\[main\]\s*$")

        path_patterns = re.findall(r"(?m)^\s{6}-\s+'([^']+)'\s*$", self.text)
        self.assertGreaterEqual(path_patterns.count(".github/workflows/**"), 2)
        self.assertGreaterEqual(path_patterns.count(".github/actions/**"), 2)

    def test_workflow_has_minimal_permissions_and_concurrency(self) -> None:
        self.assertRegex(
            self.text,
            r"(?m)^permissions:\s*\n\s{2}contents:\s*read\s*$",
        )
        self.assertRegex(
            self.text,
            r"(?m)^concurrency:\s*\n"
            r"\s{2}group:\s*workflow-lint-\$\{\{\s*github\.ref\s*\}\}\s*\n"
            r"\s{2}cancel-in-progress:\s*true\s*$",
        )

    def test_lint_job_runs_on_github_ubuntu_with_checkout_and_python(self) -> None:
        self.assertRegex(self.text, r"(?m)^\s{2}lint:\s*$")
        self.assertRegex(self.text, r"(?m)^\s{4}runs-on:\s*ubuntu-latest\s*$")
        self.assertIn("yamllint + actionlint + structural parse", self.text)

        checkout = _find_uses_step(self.text, "actions/checkout@v5")
        self.assertRegex(checkout, r"(?m)^\s{10}fetch-depth:\s*1\s*$")

        setup_python = _find_step(self.text, "Set up Python")
        self.assertIn("uses: actions/setup-python@v5", setup_python)
        self.assertRegex(setup_python, r"(?m)^\s{10}python-version:\s*'3\.12'\s*$")

    def test_yamllint_step_uses_pinned_local_workflow_lint(self) -> None:
        step = _find_step(self.text, "yamllint")
        self.assertIn("set -euo pipefail", step)
        self.assertIn("yamllint==1.35.1", step)
        self.assertIn("yamllint --no-warnings -d 'relaxed' .github/workflows/", step)

    def test_structural_parse_checks_all_workflow_yaml_files(self) -> None:
        step = _find_step(self.text, "Structural YAML parse")
        self.assertIn("set -euo pipefail", step)
        self.assertIn("pyyaml>=6", step)
        self.assertIn("yaml.safe_load", step)
        self.assertIn("pathlib.Path('.github/workflows').rglob('*.yml')", step)
        self.assertIn("pathlib.Path('.github/workflows').rglob('*.yaml')", step)
        self.assertIn("except yaml.YAMLError", step)
        self.assertIn("sys.exit(1)", step)
        self.assertIn("structural parse OK", step)

    def test_release_regression_tests_remain_in_lint_gate(self) -> None:
        step = _find_step(self.text, "Release-pipeline regression tests (#720)")
        self.assertIn("set -euo pipefail", step)
        self.assertIn(
            "python3 tools/scripts/test_release_workflow_test_step.py",
            step,
        )
        self.assertIn(
            "python3 tools/scripts/test_workflow_build_dirs.py",
            step,
        )

    def test_actionlint_step_keeps_core_actionlint_enabled(self) -> None:
        step = _find_step(self.text, "actionlint")
        self.assertIn("uses: raven-actions/actionlint@v2", step)
        self.assertRegex(step, r"(?m)^\s{10}matcher:\s*true\s*$")
        self.assertRegex(step, r"(?m)^\s{10}shellcheck:\s*false\s*$")
        self.assertRegex(step, r"(?m)^\s{10}pyflakes:\s*false\s*$")
        self.assertRegex(step, r"(?m)^\s{10}flags:\s*''\s*$")


if __name__ == "__main__":
    unittest.main(verbosity=2)
