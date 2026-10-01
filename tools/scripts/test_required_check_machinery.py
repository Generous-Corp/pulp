#!/usr/bin/env python3
"""Tests for the advisory required-check machinery report."""

from __future__ import annotations

import json
import os
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


class CommittedRulesetTest(unittest.TestCase):
    """A missing or malformed committed ruleset fails closed to every workflow."""

    FILES = [".github/workflows/release-cli.yml", ".github/workflows/build.yml", "docs/x.md"]

    def repo_with(self, ruleset: str | None) -> Path:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        repo = Path(temp.name)
        if ruleset is not None:
            (repo / ".github" / "rulesets").mkdir(parents=True)
            (repo / rcm.RULESET).write_text(ruleset, encoding="utf-8")
        return repo

    def test_unusable_ruleset_counts_every_workflow(self) -> None:
        no_required = json.dumps({"rules": [{"type": "merge_queue", "parameters": {}}]})
        wrong_shape = json.dumps({"rules": [{"type": "required_status_checks",
                                             "parameters": {"required_status_checks": [{}]}}]})
        for name, text in (("missing", None), ("not json", "<html>"), ("not an object", "[]"),
                           ("no required checks", no_required), ("wrong shape", wrong_shape)):
            with self.subTest(name):
                repo = self.repo_with(text)
                self.assertEqual(rcm.required_contexts(repo), [])
                flagged = rcm.classify(self.FILES, repo)
                self.assertEqual(sorted(flagged), self.FILES[:2][::-1])
                self.assertIn("workflow (required checks unreadable)",
                              flagged[".github/workflows/release-cli.yml"])

    def test_committed_ruleset_selects_the_mapped_producers(self) -> None:
        repo = self.repo_with((ROOT / rcm.RULESET).read_text(encoding="utf-8"))
        self.assertEqual(rcm.required_contexts(repo), rcm.required_contexts(ROOT))
        self.assertEqual(list(rcm.classify(self.FILES, repo)), [".github/workflows/build.yml"])


class WorkflowStepTest(unittest.TestCase):
    """Runs the workflow's own step against a fixture PR with a stub `gh`.

    The stub records the check run the step posts, so the rendering is read from
    what the step sent rather than from its text.
    """

    STUB_GH = """#!/usr/bin/env python3
import json, os, sys
args = sys.argv[1:]
with open(os.environ["GH_LOG"], "a", encoding="utf-8") as handle:
    payload = json.load(sys.stdin) if "check-runs" in " ".join(args) else None
    handle.write(json.dumps({"args": args, "payload": payload}) + "\\n")
sys.exit(0 if payload is not None else 2)
"""

    @staticmethod
    def step_script() -> str:
        lines = WORKFLOW.read_text(encoding="utf-8").splitlines()
        start = lines.index("      - name: Report files that decide required checks")
        run = next(i for i in range(start, len(lines)) if lines[i] == "        run: |")
        body = []
        for line in lines[run + 1:]:
            if line.strip() and not line.startswith(" " * 10):
                break
            body.append(line[10:])
        return "\n".join(body) + "\n"

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        (bin_dir / "gh").write_text(self.STUB_GH, encoding="utf-8")
        (bin_dir / "gh").chmod(0o755)
        self.path = f"{bin_dir}:{os.environ['PATH']}"
        origin = self.root / "origin.git"
        git(self.root, "init", "-q", "--bare", str(origin))
        work = self.root / "work"
        work.mkdir()
        for rel in (rcm.RULESET, ".github/workflows/build.yml", "docs/guides/local-ci.md",
                    ".github/workflows/release-cli.yml",
                    "tools/scripts/required_check_machinery.py"):
            (work / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / rel, work / rel)
        git(work, "init", "-q", "-b", "main")
        git(work, "config", "user.email", "ci@example.invalid")
        git(work, "config", "user.name", "CI")
        git(work, "add", ".")
        git(work, "commit", "-qm", "base")
        git(work, "remote", "add", "origin", str(origin))
        git(work, "push", "-q", "origin", "main")
        self.work = work
        self.heads: dict[str, str] = {}
        for number, rel in ((1, ".github/workflows/build.yml"), (2, "docs/guides/local-ci.md"),
                            (3, ".github/workflows/release-cli.yml")):
            git(work, "checkout", "-q", "-b", f"pr{number}", "main")
            with (work / rel).open("a", encoding="utf-8") as handle:
                handle.write("\n# edit\n")
            git(work, "commit", "-qam", f"pr {number}")
            git(work, "push", "-q", "origin", f"HEAD:refs/pull/{number}/head")
            self.heads[rel] = git(work, "rev-parse", "HEAD")
            git(work, "checkout", "-q", "main")
        (self.root / "step.sh").write_text(self.step_script(), encoding="utf-8")

    def run_step(self, number: int, head: str, ruleset: str | None = None) -> dict:
        runner_temp = self.root / f"rt-{number}-{ruleset is None}"
        runner_temp.mkdir()
        log = runner_temp / "gh.log"
        committed = self.work / rcm.RULESET
        if ruleset is not None:
            committed.write_text(ruleset, encoding="utf-8")
            self.addCleanup(git, self.work, "checkout", "-q", "--", rcm.RULESET)
        env = dict(os.environ, PATH=self.path, GH_TOKEN="unused", GH_LOG=str(log),
                   PR_NUMBER=str(number), PR_HEAD=head, REPOSITORY="example/repo",
                   RUN_URL="https://example.invalid/run", RUNNER_TEMP=str(runner_temp),
                   GITHUB_STEP_SUMMARY=str(runner_temp / "summary.md"))
        done = subprocess.run(["bash", "-e", str(self.root / "step.sh")], cwd=self.work,
                              env=env, capture_output=True, text=True, timeout=60)
        if ruleset is not None:
            git(self.work, "checkout", "-q", "--", rcm.RULESET)
        self.assertEqual(done.returncode, 0, done.stderr)
        calls = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]
        # The only API call is the check-run post: no branch-protection read.
        self.assertEqual([c["args"][-3:] for c in calls],
                         [["repos/example/repo/check-runs", "--input", "-"]])
        posted = calls[0]["payload"]
        self.assertEqual(posted["name"], CONTEXT)
        self.assertEqual(posted["head_sha"], head)
        posted["stdout"] = done.stdout
        return posted

    def test_flagged_pull_request_renders_neutral(self) -> None:
        check = self.run_step(1, self.heads[".github/workflows/build.yml"])
        self.assertEqual(check["conclusion"], "neutral")
        self.assertEqual(check["output"]["title"], "touches required-check machinery: build.yml")
        self.assertIn("visibility, not enforcement", check["output"]["summary"])
        self.assertIn("::warning title=Required-check machinery", check["stdout"])

    def test_unflagged_pull_request_renders_success(self) -> None:
        check = self.run_step(2, self.heads["docs/guides/local-ci.md"])
        self.assertEqual(check["conclusion"], "success")
        self.assertEqual(check["output"]["title"], "touches no required-check machinery")
        self.assertNotIn("could not", check["output"]["summary"])
        self.assertNotIn("::warning", check["stdout"])

    def test_committed_ruleset_decides_whether_another_workflow_is_flagged(self) -> None:
        # A workflow that posts no required check is flagged only when main's
        # committed ruleset cannot be read.
        head = self.heads[".github/workflows/release-cli.yml"]
        self.assertEqual(self.run_step(3, head)["conclusion"], "success")
        broken = self.run_step(3, head, ruleset="{")
        self.assertEqual(broken["conclusion"], "neutral")
        self.assertEqual(broken["output"]["title"],
                         "touches required-check machinery: release-cli.yml")
        self.assertIn("lists no readable required checks", broken["output"]["summary"])


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
        self.assertIn("  contents: read\n  checks: write\n", self.text)

    def test_check_is_advisory(self) -> None:
        self.assertIn(f'name: "{CONTEXT}"', self.text)
        self.assertNotIn(CONTEXT, rcm.required_contexts(ROOT))


if __name__ == "__main__":
    unittest.main()
