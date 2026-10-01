#!/usr/bin/env python3
"""Tests for the advisory required-check machinery report."""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

import protected_merge_receipt  # noqa: E402
import required_check_machinery as rcm  # noqa: E402

WORKFLOW = ROOT / ".github" / "workflows" / "required-check-machinery.yml"
CONTEXT = "Required-check machinery (advisory)"


def git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(repo), *args], check=True,
                          capture_output=True, text=True).stdout.strip()


class DiffFixtureTest(unittest.TestCase):
    """Runs the CLI on a real two-commit diff, as the workflow does."""

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.repo = Path(self.temp.name)
        for rel in (rcm.RULESET, ".github/workflows/build.yml", "docs/guides/local-ci.md"):
            target = self.repo / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / rel, target)
        git(self.repo, "init", "-q")
        git(self.repo, "config", "user.email", "ci@example.invalid")
        git(self.repo, "config", "user.name", "CI")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "base")
        self.base = git(self.repo, "rev-parse", "HEAD")

    def tearDown(self) -> None:
        self.temp.cleanup()

    def commit_edit(self, rel: str, old: str, new: str) -> str:
        path = self.repo / rel
        text = path.read_text(encoding="utf-8")
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, 1), encoding="utf-8")
        git(self.repo, "commit", "-qam", "edit")
        return git(self.repo, "rev-parse", "HEAD")

    def report(self, head: str) -> dict:
        done = subprocess.run(
            [sys.executable, str(HERE / "required_check_machinery.py"),
             "--repo", str(self.repo), "--diff", self.base, head],
            check=True, capture_output=True, text=True)
        return json.loads(done.stdout)

    def test_editing_the_macos_job_is_flagged(self) -> None:
        # The shape that lets a pull request pass `macos` without building:
        # rewrite the job that posts it.
        head = self.commit_edit(
            ".github/workflows/build.yml",
            "&& matrix.key == 'macos' && 'macos' ||",
            "&& 'macos' ||")
        result = self.report(head)
        self.assertEqual(sorted(result["flagged"]), [".github/workflows/build.yml"])
        self.assertIn("required-check workflow", result["flagged"][".github/workflows/build.yml"])
        self.assertIn("receipt reuse", result["flagged"][".github/workflows/build.yml"])
        self.assertEqual(result["description"], "touches required-check machinery: build.yml")

    def test_docs_only_change_is_not_flagged(self) -> None:
        head = self.commit_edit("docs/guides/local-ci.md", "#", "# ")
        result = self.report(head)
        self.assertEqual(result["flagged"], {})
        self.assertEqual(result["description"], "touches no required-check machinery")


class ClassificationTest(unittest.TestCase):

    def test_each_category_is_named(self) -> None:
        flagged = rcm.classify([
            "tools/scripts/protected_merge_receipt.py",
            ".github/workflows/drift-fast.yml",
            ".github/actions/install-linux-build-deps/action.yml",
            ".github/rulesets/main-protection.json",
            ".github/workflows/release-cli.yml",
            "core/view/src/widgets.cpp",
        ], ROOT)
        self.assertEqual(flagged, {
            "tools/scripts/protected_merge_receipt.py": ["receipt reuse"],
            ".github/workflows/drift-fast.yml": ["required-check workflow"],
            ".github/actions/install-linux-build-deps/action.yml": ["required-check workflow"],
            ".github/rulesets/main-protection.json": ["merge rules"],
        })

    def test_every_required_context_has_an_existing_producer(self) -> None:
        contexts = rcm.required_contexts(ROOT)
        self.assertEqual(set(contexts), set(rcm.CONTEXT_PRODUCERS))
        for context in contexts:
            producers = rcm.CONTEXT_PRODUCERS[context]
            texts = [(ROOT / path).read_text(encoding="utf-8") for path in producers]
            self.assertTrue(any(context in text for text in texts), context)

    def test_an_unmapped_required_context_widens_to_every_workflow(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        repo = Path(temp.name)
        (repo / ".github" / "rulesets").mkdir(parents=True)
        ruleset = json.loads((ROOT / rcm.RULESET).read_text(encoding="utf-8"))
        for rule in ruleset["rules"]:
            if rule["type"] == "required_status_checks":
                rule["parameters"]["required_status_checks"].append({"context": "new-gate"})
        (repo / rcm.RULESET).write_text(json.dumps(ruleset), encoding="utf-8")
        flagged = rcm.classify([".github/workflows/release-cli.yml", "docs/x.md"], repo)
        self.assertEqual(list(flagged), [".github/workflows/release-cli.yml"])
        self.assertIn("new-gate", flagged[".github/workflows/release-cli.yml"][0])

    def test_reuse_machinery_covers_the_receipt_policy_paths(self) -> None:
        self.assertLessEqual(set(protected_merge_receipt.POLICY_PATHS), set(rcm.REUSE_MACHINERY))

    def test_description_fits_a_status_and_counts_what_it_drops(self) -> None:
        flagged = {f".github/workflows/w{i:02d}-long-workflow-name.yml": ["x"] for i in range(12)}
        text = rcm.describe(flagged)
        self.assertLessEqual(len(text), 140)
        shown = text.removeprefix("touches required-check machinery: ").split(" (+")[0]
        dropped = int(text.rsplit("(+", 1)[1].split()[0])
        self.assertEqual(len(shown.split(", ")) + dropped, 12)


class WorkflowContractTest(unittest.TestCase):
    """The report runs from protected main and is never a required check."""

    def setUp(self) -> None:
        self.text = WORKFLOW.read_text(encoding="utf-8")

    def test_runs_the_protected_definition_and_never_checks_out_the_head(self) -> None:
        self.assertIn("pull_request_target:", self.text)
        for event in ("\n  pull_request:", "\n  merge_group:", "\n  push:"):
            self.assertNotIn(event, self.text)
        self.assertIn("ref: main\n", self.text)
        self.assertNotIn("ref: ${{ github.event.pull_request.head", self.text)
        self.assertIn("persist-credentials: false", self.text)

    def test_status_is_advisory(self) -> None:
        self.assertIn(f"context='{CONTEXT}'", self.text)
        self.assertNotIn(CONTEXT, rcm.required_contexts(ROOT))


if __name__ == "__main__":
    unittest.main()
