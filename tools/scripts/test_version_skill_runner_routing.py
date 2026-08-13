#!/usr/bin/env python3
"""Structural contract checks for Version/Skill runner routing."""

from __future__ import annotations

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW_PATH = ROOT / ".github" / "workflows" / "version-skill-check.yml"
GUIDE_PATH = ROOT / "docs" / "guides" / "versioning.md"
RUNNER_VARIABLE = "PULP_VERSION_SKILL_RUNS_ON_JSON"
DEFAULT_HOSTED_SELECTOR = (
    "${{ fromJSON(vars.PULP_VERSION_SKILL_RUNS_ON_JSON || '\"ubuntu-latest\"') }}"
)


def workflow_source() -> str:
    return WORKFLOW_PATH.read_text(encoding="utf-8")


def mapping_block(source: str, key: str, indent: int) -> str:
    lines = source.splitlines()
    header = f"{' ' * indent}{key}:"
    try:
        start = lines.index(header)
    except ValueError as exc:
        raise AssertionError(f"missing workflow mapping {key!r}") from exc

    end = len(lines)
    for index in range(start + 1, len(lines)):
        line = lines[index]
        stripped = line.lstrip()
        if not stripped or stripped.startswith("#"):
            continue
        line_indent = len(line) - len(stripped)
        if line_indent <= indent:
            end = index
            break
    return "\n".join(lines[start:end])


def mapping_keys(block: str, indent: int) -> list[str]:
    pattern = re.compile(rf"^ {{{indent}}}([A-Za-z0-9_-]+):", re.MULTILINE)
    return pattern.findall(block)


class VersionSkillRunnerRoutingTests(unittest.TestCase):
    def test_runner_selector_is_opt_in_and_defaults_to_github_hosted(self) -> None:
        source = workflow_source()
        job = mapping_block(source, "version-skill-check", 2)
        match = re.search(r"^    runs-on: (.+)$", job, re.MULTILINE)

        self.assertIsNotNone(match)
        self.assertEqual(match.group(1), DEFAULT_HOSTED_SELECTOR)
        self.assertEqual(source.count(f"vars.{RUNNER_VARIABLE}"), 1)
        self.assertIn(
            "python3 tools/scripts/test_version_skill_runner_routing.py", job
        )
        for activated_label in (
            "self-hosted",
            "pulp-build-linux-x64",
            "pulp-host-macpro",
        ):
            with self.subTest(label=activated_label):
                self.assertNotIn(activated_label, source)
        self.assertNotRegex(source, r"\bsecrets\.")

        guide = " ".join(GUIDE_PATH.read_text(encoding="utf-8").split())
        for policy_text in (
            "healthy approved disposable Mac Pro Shipyard pool",
            "must clear the variable whenever that pool is unhealthy",
            "restoring the hosted fallback",
            "Never apply this selector pattern to secret-bearing or `pull_request_target`",
        ):
            with self.subTest(policy=policy_text):
                self.assertIn(policy_text, guide)

    def test_required_workflow_public_contract_is_unchanged(self) -> None:
        source = workflow_source()
        on = mapping_block(source, "on", 0)
        pull_request = mapping_block(on, "pull_request", 2)
        workflow_dispatch = mapping_block(on, "workflow_dispatch", 2)
        runner_provider = mapping_block(workflow_dispatch, "runner_provider", 6)
        concurrency = mapping_block(source, "concurrency", 0)
        jobs = mapping_block(source, "jobs", 0)
        job = mapping_block(jobs, "version-skill-check", 2)

        self.assertIn("name: Versioning & Skill-Sync", source)
        self.assertEqual(
            mapping_keys(on, 2),
            ["pull_request", "merge_group", "workflow_dispatch"],
        )
        self.assertEqual(mapping_keys(pull_request, 4), ["branches", "types"])
        self.assertIn("branches: [main, develop]", pull_request)
        self.assertIn(
            "types: [opened, synchronize, reopened, edited]", pull_request
        )
        self.assertRegex(on, r"(?m)^  merge_group:\s*$")
        self.assertEqual(mapping_keys(workflow_dispatch, 4), ["inputs"])
        self.assertEqual(
            mapping_keys(runner_provider, 8),
            ["description", "required", "default", "type", "options"],
        )
        self.assertIn("required: false", runner_provider)
        self.assertIn("default: github-hosted", runner_provider)
        self.assertIn("options:\n          - github-hosted\n          - namespace", runner_provider)

        self.assertEqual(
            mapping_keys(concurrency, 2), ["group", "cancel-in-progress"]
        )
        self.assertIn("group: version-skill-check-${{ github.ref }}", concurrency)
        self.assertIn("cancel-in-progress: false", concurrency)
        self.assertEqual(mapping_keys(jobs, 2), ["version-skill-check"])
        self.assertIn("name: Enforce version & skill sync", job)
        self.assertNotRegex(source, r"(?m)^\s*permissions:")


if __name__ == "__main__":
    unittest.main()
