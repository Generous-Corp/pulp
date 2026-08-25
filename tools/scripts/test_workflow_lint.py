#!/usr/bin/env python3
"""Static regression tests for .github/workflows/workflow-lint.yml.

The workflow lint gate is the first release-watchdog layer: it must run
on CI-definition changes and keep the three local checks that catch
workflow-file failures before merge.

It also owns the fleet-wide action-pin invariant below: GitHub retires a
runtime under an action's old major, and the resulting deprecation
annotation is only visible on a real run, so a drifting pin is otherwise
found by reading warnings rather than by a gate.

Run:
    python3 tools/scripts/test_workflow_lint.py
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

import yaml


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "workflow-lint.yml"
ACTIONLINT_CONFIG = REPO_ROOT / ".github" / "actionlint.yaml"
POST_TAG_SYNC_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "post-tag-sync.yml"


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
        self.assertGreaterEqual(path_patterns.count(".github/actionlint.yaml"), 2)
        self.assertGreaterEqual(path_patterns.count(".github/workflows/**"), 2)
        self.assertGreaterEqual(path_patterns.count(".github/actions/**"), 2)
        self.assertGreaterEqual(path_patterns.count("tools/shipyard.toml"), 2)

    def test_actionlint_knows_the_authority_runner_label(self) -> None:
        self.assertTrue(
            ACTIONLINT_CONFIG.exists(),
            f"missing actionlint config: {ACTIONLINT_CONFIG}",
        )
        config = ACTIONLINT_CONFIG.read_text(encoding="utf-8")
        self.assertRegex(config, r"(?m)^self-hosted-runner:\s*$")
        self.assertRegex(config, r"(?m)^\s{2}labels:\s*$")
        self.assertRegex(
            config,
            r"(?m)^\s{4}-\s+pulp-queue-authority-studio\s*$",
        )

    def test_post_tag_sync_runs_on_the_authority_runner(self) -> None:
        self.assertTrue(
            POST_TAG_SYNC_WORKFLOW.exists(),
            f"missing workflow: {POST_TAG_SYNC_WORKFLOW}",
        )
        post_tag_sync = POST_TAG_SYNC_WORKFLOW.read_text(encoding="utf-8")
        self.assertRegex(
            post_tag_sync,
            r"(?m)^\s{4}runs-on:\s*\[self-hosted, pulp-queue-authority-studio\]\s*$",
        )

    def test_workflow_lint_gate_runs_this_regression_suite(self) -> None:
        self.assertIn(
            "python3 tools/scripts/test_workflow_lint.py",
            self.text,
        )

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

    def test_every_job_inherits_or_declares_token_permissions(self) -> None:
        missing: list[str] = []
        workflows = sorted(
            (REPO_ROOT / ".github" / "workflows").glob("*.y*ml")
        )
        for path in workflows:
            value = yaml.safe_load(path.read_text(encoding="utf-8"))
            if value.get("permissions") is not None:
                continue
            for job_name, job in value.get("jobs", {}).items():
                if job.get("permissions") is None:
                    missing.append(f"{path.name}:{job_name}")
        self.assertEqual(
            missing,
            [],
            "jobs inherit repository-default token permissions: "
            + ", ".join(missing),
        )

    def test_lint_job_runs_on_github_ubuntu_with_checkout_and_python(self) -> None:
        self.assertRegex(self.text, r"(?m)^\s{2}lint:\s*$")
        self.assertRegex(self.text, r"(?m)^\s{4}runs-on:\s*ubuntu-latest\s*$")
        self.assertIn("yamllint + actionlint + structural parse", self.text)

        checkout = _find_uses_step(self.text, "actions/checkout@v5")
        self.assertRegex(checkout, r"(?m)^\s{10}fetch-depth:\s*1\s*$")

        setup_python = _find_step(self.text, "Set up Python")
        # The major lives in ActionMajorPinTests so a routine action bump
        # touches one invariant instead of every workflow assertion.
        self.assertRegex(setup_python, r"uses: actions/setup-python@v\d+")
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
        step = _find_step(self.text, "Release-pipeline regression tests (#720, #1962, #2467)")
        self.assertIn("set -euo pipefail", step)
        self.assertIn(
            "python3 tools/scripts/test_release_workflow_test_step.py",
            step,
        )
        self.assertIn(
            "python3 tools/scripts/test_workflow_build_dirs.py",
            step,
        )
        self.assertIn(
            "python3 tools/scripts/test_build_macos_workflow_dispatch.py",
            step,
        )
        self.assertIn(
            "python3 tools/scripts/test_fetch_skia_for_release.py",
            step,
        )
        self.assertIn(
            "python3 tools/scripts/test_preamble_python_stable_cwd.py",
            step,
        )

    def test_actionlint_step_keeps_core_actionlint_enabled(self) -> None:
        step = _find_step(self.text, "actionlint")
        self.assertIn("uses: raven-actions/actionlint@v2", step)
        self.assertRegex(step, r"(?m)^\s{10}matcher:\s*true\s*$")
        self.assertRegex(step, r"(?m)^\s{10}shellcheck:\s*false\s*$")
        self.assertRegex(step, r"(?m)^\s{10}pyflakes:\s*false\s*$")
        self.assertRegex(step, r"(?m)^\s{10}flags:\s*''\s*$")


class ActionMajorPinTests(unittest.TestCase):
    """Every workflow must agree on one non-retired major per action.

    A split pin is the failure mode that actually happens: a new workflow
    copies a current pin while the older ones keep an obsolete major, and
    the deprecation surfaces only as a warning annotation on a live run.
    Comparing the pins against each other catches the drift without this
    test needing to know GitHub's current runtime deprecation schedule.
    """

    #: Minimum major per action, raised as GitHub retires the runtime under
    #: the previous one. `setup-python` moved to 6 when v5 was forced onto
    #: Node 24 and started emitting a Node 20 deprecation annotation.
    MINIMUM_MAJOR = {"setup-python": 6}

    def _pins(self) -> dict[str, dict[str, set[int]]]:
        found: dict[str, dict[str, set[int]]] = {}
        pattern = re.compile(r"uses:\s*actions/([a-z0-9-]+)@v(\d+)")
        for workflow in sorted((REPO_ROOT / ".github" / "workflows").rglob("*.yml")):
            for action, major in pattern.findall(workflow.read_text(encoding="utf-8")):
                found.setdefault(action, {}).setdefault(workflow.name, set()).add(int(major))
        return found

    def test_setup_python_is_pinned_above_the_retired_major(self) -> None:
        pins = self._pins()
        for action, minimum in self.MINIMUM_MAJOR.items():
            with self.subTest(action=action):
                usage = pins.get(action)
                self.assertIsNotNone(usage, f"no workflow uses actions/{action}")
                stale = {
                    name: sorted(majors)
                    for name, majors in usage.items()
                    if any(major < minimum for major in majors)
                }
                self.assertEqual(
                    stale,
                    {},
                    f"actions/{action} must be pinned to v{minimum} or newer; "
                    f"stale pins: {stale}",
                )

    def test_setup_python_major_does_not_split_across_workflows(self) -> None:
        usage = self._pins().get("setup-python") or {}
        majors = sorted({major for majors in usage.values() for major in majors})
        self.assertEqual(
            len(majors),
            1,
            f"actions/setup-python is pinned to more than one major: {majors}",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
