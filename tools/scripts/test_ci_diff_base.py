#!/usr/bin/env python3
"""Tests for ci_diff_base.py, the diff base version-skill-check.yml compares against.

The central fixture is a two-entry merge queue on main M:

    M ── A_group (merge of entry A: a `fix:` touching no versioned surface)
            └── B_group (merge of entry B: a `test:` commit)

Entry A is a real Z2 failure. Entry B is innocent. With the merge-group base
taken from ``merge_group.base_sha`` B passes and A fails on its own. Under the
merge-base(origin/main, HEAD) rule B's range swallows A's commit and B fails,
which is how an innocent queue entry was ejected while the offender merged.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gate_test_support import GateFixtureTestCase, VBC, _git, _run  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
RESOLVER = REPO_ROOT / "tools" / "scripts" / "ci_diff_base.py"
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "version-skill-check.yml"


def _out(cwd: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(cwd), *args], check=True, capture_output=True, text=True, encoding="utf-8"
    ).stdout.strip()


class MergeQueueBaseTests(GateFixtureTestCase):
    def setUp(self) -> None:
        super().setUp()
        root = self.f.root
        cfgp = self.tmp / "tools/scripts/versioning.json"
        cfg = json.loads(cfgp.read_text(encoding="utf-8"))
        cfg["post_merge_assignment"] = True  # the live model on main
        cfgp.write_text(json.dumps(cfg, indent=2) + "\n", encoding="utf-8")
        _git(root, "add", "--", "tools/scripts/versioning.json")
        _git(root, "commit", "-q", "-m", "chore: enable post-merge assignment")
        _git(root, "update-ref", "refs/remotes/origin/main", "HEAD")
        self.cfg = str(cfgp)
        self.main = _out(root, "rev-parse", "HEAD")

        # Entry A: a fix: commit touching no versioned surface, no skip trailer.
        _git(root, "checkout", "-q", "-b", "entry-a", self.main)
        self._commit("notes/a.txt", "a\n", "fix(notes): adjust a note")
        # Entry B: a test: commit, branched from main like any PR.
        _git(root, "checkout", "-q", "-b", "entry-b", self.main)
        self._commit("notes/b.txt", "b\n", "test: cover the b note")

        # The queue builds each group commit on top of the one ahead of it.
        _git(root, "checkout", "-q", "--detach", self.main)
        _git(root, "merge", "-q", "--no-ff", "-m",
             "Merge pull request #1 from example/entry-a", "entry-a")
        self.a_group = _out(root, "rev-parse", "HEAD")
        _git(root, "merge", "-q", "--no-ff", "-m",
             "Merge pull request #2 from example/entry-b", "entry-b")
        self.b_group = _out(root, "rev-parse", "HEAD")

    def _commit(self, path: str, content: str, subject: str) -> None:
        self.f.write(path, content)
        _git(self.f.root, "add", "--", path)
        _git(self.f.root, "commit", "-q", "-m", subject)

    def _resolve(self, *args: str, head: str) -> tuple[int, str, str]:
        proc = subprocess.run(
            [sys.executable, str(RESOLVER), "--repo", str(self.tmp),
             "--head", head, "--github-output", "", *args],
            capture_output=True, text=True, encoding="utf-8",
        )
        return proc.returncode, proc.stdout.strip(), proc.stderr

    def _vbc(self, base: str, head: str) -> tuple[int, str]:
        return _run(
            [sys.executable, str(VBC), "--base", base, "--head", head,
             "--config", self.cfg, "--mode=report"],
            cwd=self.tmp,
        )

    def test_innocent_entry_behind_an_offender_passes(self) -> None:
        rc, base, err = self._resolve(
            "--event", "merge_group", "--merge-group-base-sha", self.a_group,
            head=self.b_group)
        self.assertEqual(rc, 0, err)
        self.assertEqual(base, self.a_group)
        rc, out = self._vbc(base, self.b_group)
        self.assertEqual(rc, 0, out)
        self.assertNotIn("touches NO versioned surface", out)

    def test_offending_entry_still_fails_on_its_own(self) -> None:
        rc, base, err = self._resolve(
            "--event", "merge_group", "--merge-group-base-sha", self.main,
            head=self.a_group)
        self.assertEqual(rc, 0, err)
        self.assertEqual(base, self.main)
        rc, out = self._vbc(base, self.a_group)
        self.assertEqual(rc, 1, out)
        self.assertIn("touches NO versioned surface", out)

    def test_negative_control_merge_base_rule_fails_the_innocent_entry(self) -> None:
        # The previous rule: merge-base(origin/main, HEAD). With main at M it
        # resolves to M, so B's range carries A's fix: commit and B fails.
        legacy = _out(self.f.root, "merge-base", "origin/main", self.b_group)
        self.assertEqual(legacy, self.main)
        rc, out = self._vbc(legacy, self.b_group)
        self.assertEqual(rc, 1, out)
        self.assertIn("touches NO versioned surface", out)

    def test_merge_group_without_base_sha_fails_closed(self) -> None:
        rc, base, err = self._resolve("--event", "merge_group", head=self.b_group)
        self.assertEqual(rc, 2)
        self.assertEqual(base, "")
        self.assertIn("base_sha", err)

    def test_merge_group_base_not_an_ancestor_fails_closed(self) -> None:
        entry_b_tip = _out(self.f.root, "rev-parse", "entry-b")
        rc, base, err = self._resolve(
            "--event", "merge_group", "--merge-group-base-sha", entry_b_tip,
            head=self.a_group)
        self.assertEqual(rc, 2)
        self.assertIn("not an ancestor", err)

    def test_pull_request_uses_merge_base_with_its_base_branch(self) -> None:
        # Move origin/main ahead of entry-b's fork point; the base must stay M.
        _git(self.f.root, "update-ref", "refs/remotes/origin/main", self.a_group)
        rc, base, err = self._resolve(
            "--event", "pull_request", "--base-ref", "main", head="entry-b")
        self.assertEqual(rc, 0, err)
        self.assertEqual(base, self.main)

    def test_dispatch_uses_merge_base_with_origin_main(self) -> None:
        rc, base, err = self._resolve("--event", "workflow_dispatch", head="entry-a")
        self.assertEqual(rc, 0, err)
        self.assertEqual(base, self.main)

    def test_writes_github_output(self) -> None:
        out_file = self.tmp / "gh_output"
        proc = subprocess.run(
            [sys.executable, str(RESOLVER), "--repo", str(self.tmp),
             "--head", self.b_group, "--event", "merge_group",
             "--merge-group-base-sha", self.a_group,
             "--github-output", str(out_file)],
            capture_output=True, text=True, encoding="utf-8",
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(out_file.read_text(encoding="utf-8"), f"ref={self.a_group}\n")


class WorkflowWiringTests(unittest.TestCase):
    """The required workflow must call the resolver with the queue's base."""

    def _step_code(self, name: str) -> str:
        text = WORKFLOW.read_text(encoding="utf-8")
        m = re.search(
            rf"^      - name: {re.escape(name)}\n(.*?)(?=^      - name: )",
            text, re.S | re.M)
        self.assertIsNotNone(m, f"step {name!r} not found")
        # Code lines only, so a comment naming the script cannot satisfy this.
        return "\n".join(
            ln for ln in m.group(1).splitlines()
            if ln.strip() and not ln.strip().startswith("#"))

    def test_resolve_step_passes_merge_group_base_sha(self) -> None:
        code = self._step_code("Resolve diff base")
        self.assertRegex(
            code, r"MERGE_GROUP_BASE_SHA: \$\{\{ github\.event\.merge_group\.base_sha \}\}")
        self.assertRegex(code, r"EVENT_NAME: \$\{\{ github\.event_name \}\}")
        self.assertIn("python3 tools/scripts/ci_diff_base.py", code)
        self.assertIn('--merge-group-base-sha "$MERGE_GROUP_BASE_SHA"', code)
        self.assertIn('--event "$EVENT_NAME"', code)
        self.assertNotIn("git merge-base", code)

    def test_resolver_tests_run_in_the_required_workflow(self) -> None:
        code = self._step_code("Gate-script fixture tests")
        self.assertIn("python3 tools/scripts/test_ci_diff_base.py", code)

if __name__ == "__main__":
    unittest.main()
