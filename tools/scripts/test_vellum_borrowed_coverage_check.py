#!/usr/bin/env python3
"""Tests for the pull_request-only borrowed watch-coverage check.

Each case builds the comparison `Vellum freeze` builds: live main, which has
moved on since the branch forked, merged with the PR head into a detached
candidate no ref contains. The remote holds the PR's own pushed branch, so the
own-branch ignore is exercised exactly as in CI.
"""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_vellum_watch_preflight import (  # noqa: E402
    EVENT_DIR_REL,
    UNWATCHED_PATH,
    WATCHED_FAMILIES,
    WATCHED_PATH,
    _git,
    _PreflightRepo,
)

ROOT = Path(__file__).resolve().parents[2]
CHECK = ROOT / "tools" / "scripts" / "vellum_borrowed_coverage_check.py"
WORKFLOW = ROOT / ".github" / "workflows" / "vellum-freeze-check.yml"
STEP_NAME = "Refuse watch coverage borrowed from a stacked parent"


class BorrowedCoverageCheckTest(_PreflightRepo, unittest.TestCase):
    def setUp(self) -> None:
        super().setUp()
        # actions/checkout configures origin; the ignore list is per remote.
        _git(self.repo, "remote", "add", "origin", "https://example.invalid/repo.git")

    def _parent_with_event(self) -> str:
        _git(self.repo, "checkout", "-q", "-b", "parent", "main")
        self._write(WATCHED_PATH, "# the parent PR's watched change\n")
        self._write(f"{EVENT_DIR_REL}/20260920-parent.json",
                    self._event("20260920-parent", WATCHED_FAMILIES))
        parent = self._commit("feat: the parent PR, with its event")
        _git(self.repo, "update-ref", "refs/remotes/origin/parent", parent)
        return parent

    def _push_topic(self) -> str:
        head = _git(self.repo, "rev-parse", "topic")
        _git(self.repo, "update-ref", "refs/remotes/origin/topic", head)
        return head

    def _advance_main(self) -> None:
        _git(self.repo, "checkout", "-q", "main")
        self._write(UNWATCHED_PATH, "main moved on after the branch forked\n")
        self._commit("docs: unrelated work on main")
        _git(self.repo, "update-ref", "refs/remotes/origin/main",
             _git(self.repo, "rev-parse", "main"))

    def _check(self, source_head: str) -> tuple[int, str]:
        """Run the check on live main merged with ``source_head``, as CI does."""
        live = _git(self.repo, "rev-parse", "main")
        _git(self.repo, "checkout", "-q", "--detach", live)
        _git(self.repo, "merge", "-q", "--no-ff", "--no-edit", source_head)
        candidate = _git(self.repo, "rev-parse", "HEAD")
        self.assertEqual(_git(self.repo, "rev-list", "--parents", "-n", "1", candidate),
                         f"{candidate} {live} {source_head}")
        completed = subprocess.run(
            ["python3", str(CHECK), "--root", str(self.repo), "--base", live,
             "--head", candidate, "--source-head", source_head, "--own-branch", "topic"],
            cwd=self.repo, check=False, capture_output=True, text=True, encoding="utf-8")
        return completed.returncode, completed.stdout + completed.stderr

    def test_a_pr_stacked_on_an_unmerged_parent_fails_naming_the_parent(self) -> None:
        self._parent_with_event()
        _git(self.repo, "checkout", "-q", "-b", "topic")
        self._write(WATCHED_PATH, "# the parent PR's watched change\n# and topic's\n")
        self._commit("feat: topic, stacked on parent")
        head = self._push_topic()
        self._advance_main()
        code, output = self._check(head)
        self.assertEqual(code, 1, output)
        self.assertIn("FAIL", output)
        self.assertIn("the pull request from parent (origin/parent)", output)
        self.assertIn("Re-run this check after the parent merges", output)
        self.assertNotIn("origin/topic", output)

    def test_the_same_pr_passes_once_its_parent_has_merged(self) -> None:
        parent = self._parent_with_event()
        _git(self.repo, "checkout", "-q", "-b", "topic")
        self._write(WATCHED_PATH, "# the parent PR's watched change\n# and topic's\n")
        self._commit("feat: topic, stacked on parent")
        head = self._push_topic()
        self._advance_main()
        _git(self.repo, "merge", "-q", "--no-ff", "--no-edit", parent)
        _git(self.repo, "update-ref", "refs/remotes/origin/main",
             _git(self.repo, "rev-parse", "main"))
        # The parent's branch is still on the remote after it merged; its
        # event commit is now in live main, so it is outside the range.
        code, output = self._check(head)
        self.assertEqual(code, 0, output)
        self.assertIn("OK", output)

    def test_a_pr_with_its_own_event_passes(self) -> None:
        _git(self.repo, "checkout", "-q", "-b", "topic")
        self._write(WATCHED_PATH, "# a one-line edit under a watched tree\n")
        self._write(f"{EVENT_DIR_REL}/20260920-fixture.json",
                    self._event("20260920-fixture", WATCHED_FAMILIES))
        self._commit("feat: touch a watched tree, with its event")
        # An older pushed copy would hold the event commit without this head;
        # only the own-branch ignore keeps the PR's own remote branch out.
        older = _git(self.repo, "rev-parse", "topic")
        self._write(WATCHED_PATH, "# a one-line edit under a watched tree\n# more\n")
        head = self._commit("feat: a follow-up")
        _git(self.repo, "update-ref", "refs/remotes/origin/topic", older)
        # A child PR stacked on this one also carries the event commit, but it
        # holds this head too: this PR is its parent, not a borrower. The
        # candidate is on no ref, so only --source-head can tell.
        _git(self.repo, "checkout", "-q", "-b", "child")
        self._write(UNWATCHED_PATH, "a child PR stacked on topic\n")
        _git(self.repo, "update-ref", "refs/remotes/origin/child",
             self._commit("docs: a child PR stacked on topic"))
        self._advance_main()
        code, output = self._check(head)
        self.assertEqual(code, 0, output)
        self.assertNotIn("FAIL", output)

    def _topic_with_event(self) -> str:
        _git(self.repo, "checkout", "-q", "-b", "topic")
        self._write(WATCHED_PATH, "# a one-line edit under a watched tree\n")
        self._write(f"{EVENT_DIR_REL}/20260920-fixture.json",
                    self._event("20260920-fixture", WATCHED_FAMILIES))
        return self._commit("feat: touch a watched tree, with its event")

    def test_a_parent_that_advanced_after_a_child_stacked_on_it_passes(self) -> None:
        event = self._topic_with_event()
        # The child holds topic's event commit but not topic's later review
        # fix, so --no-contains alone does not drop it.
        _git(self.repo, "checkout", "-q", "-b", "child", event)
        self._write(UNWATCHED_PATH, "a child PR stacked on topic\n")
        _git(self.repo, "update-ref", "refs/remotes/origin/child",
             self._commit("docs: a child PR stacked on topic"))
        _git(self.repo, "checkout", "-q", "topic")
        self._write("docs/topic-review-fix.md", "topic's review fix\n")
        self._commit("docs: topic's review fix")
        head = self._push_topic()
        self._advance_main()
        code, output = self._check(head)
        self.assertEqual(code, 0, output)
        self.assertNotIn("origin/child", output)

    def test_a_stale_copy_of_the_pr_under_another_name_does_not_fail_it(self) -> None:
        event = self._topic_with_event()
        _git(self.repo, "update-ref", "refs/remotes/origin/takeover/topic", event)
        self._write("docs/topic-follow-up.md", "a follow-up\n")
        self._commit("docs: a follow-up")
        head = self._push_topic()
        self._advance_main()
        code, output = self._check(head)
        self.assertEqual(code, 0, output)
        self.assertNotIn("takeover", output)


class WorkflowWiringTest(unittest.TestCase):
    def setUp(self) -> None:
        try:
            import yaml
        except ImportError:  # pragma: no cover - environment-dependent
            self.skipTest("PyYAML not installed")
        self.steps = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))[
            "jobs"]["freeze-check"]["steps"]

    def test_the_check_runs_on_pull_request_only_after_the_watch_check(self) -> None:
        names = [step.get("name") for step in self.steps]
        step = self.steps[names.index(STEP_NAME)]
        self.assertEqual(step["if"], "github.event_name == 'pull_request'")
        self.assertIn("vellum_borrowed_coverage_check.py", step["run"])
        self.assertIn("--source-head '${{ steps.comparison.outputs.source_head }}'",
                      step["run"])
        # The branch name reaches the shell through env, never interpolated.
        self.assertEqual(step["env"]["PR_HEAD_REF"], "${{ github.head_ref }}")
        self.assertIn('--own-branch "$PR_HEAD_REF"', step["run"])
        self.assertLess(names.index("Validate durable source-authority events"),
                        names.index(STEP_NAME))

    def test_the_merge_group_path_is_unchanged(self) -> None:
        # No other step names the check, and every other step runs on both
        # events, so merge_group sees exactly the steps it saw before.
        for step in self.steps:
            if step.get("name") == STEP_NAME:
                continue
            self.assertNotIn("tools/scripts/vellum_borrowed_coverage_check.py",
                             str(step.get("run", "")))
            self.assertNotIn("if", step, step.get("name"))
        comparison = next(s for s in self.steps if s.get("id") == "comparison")["run"]
        self.assertIn('echo "base=$MERGE_BASE"', comparison)
        self.assertIn('echo "head=$MERGE_HEAD"', comparison)
        self.assertIn('echo "source_head=$MERGE_HEAD"', comparison)


if __name__ == "__main__":
    unittest.main()
