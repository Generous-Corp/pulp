#!/usr/bin/env python3
"""Tests for tools/ci/binary_identity_shadow.py (cross-VM test-binary identity, shadow).

What must hold:
- `compare` counts identical and differing binaries over the paths both
  identities carry, lists differing examples, and reports paths only one side
  has separately (a missing binary is not a mismatch);
- the merge-parent and tree-identity helpers read a real two-parent merge and
  answer false for a moved base and true for an unmoved one;
- `measure` on a commit that is not a two-parent merge annotates that verdict
  and exits 0; an unreadable build directory exits 2 with no annotation;
- the annotation carries the schema and mode.

Run:
    python3 tools/ci/test_binary_identity_shadow.py
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import binary_identity_shadow as bis  # noqa: E402

ENV = {"PATH": "/usr/bin:/bin:/opt/homebrew/bin", "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
       "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}


def git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, check=True, env=ENV).stdout.strip()


def ident(**files: str) -> dict:
    return {"files": [{"path": p, "sha256": h} for p, h in sorted(files.items())]}


class CompareTests(unittest.TestCase):
    def test_identical_differing_and_one_sided_paths(self) -> None:
        r = bis.compare(ident(a="1", b="2", c="3", only_here="9"), ident(a="1", b="X", c="3", only_there="8"))
        self.assertEqual((r["compared"], r["identical"], r["differing"]), (3, 2, 1))
        self.assertEqual(r["differing_examples"], ["b"])
        self.assertEqual((r["only_here"], r["only_there"]), (1, 1))
        self.assertAlmostEqual(r["identical_share"], 2 / 3)

    def test_nothing_in_common_has_no_share(self) -> None:
        r = bis.compare(ident(a="1"), ident(b="1"))
        self.assertEqual(r["compared"], 0)
        self.assertIsNone(r["identical_share"])


class GitHelperTests(unittest.TestCase):
    def make_repo(self, tmp: Path, move_base: bool) -> tuple[Path, str, str]:
        repo = tmp / "r"
        repo.mkdir()
        git(repo, "init", "-q", "-b", "main")
        (repo / "a.txt").write_text("a\n"); git(repo, "add", "a.txt"); git(repo, "commit", "-q", "-m", "base")
        git(repo, "checkout", "-q", "-b", "pr")
        (repo / "b.txt").write_text("b\n"); git(repo, "add", "b.txt"); git(repo, "commit", "-q", "-m", "pr")
        head = git(repo, "rev-parse", "HEAD")
        git(repo, "checkout", "-q", "main")
        if move_base:
            (repo / "c.txt").write_text("c\n"); git(repo, "add", "c.txt"); git(repo, "commit", "-q", "-m", "main moved")
        git(repo, "merge", "-q", "--no-ff", "--no-edit", "pr")
        return repo, git(repo, "rev-parse", "HEAD"), head

    def test_unmoved_base_is_tree_identical_and_moved_base_is_not(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo, merge, head = self.make_repo(Path(tmp), move_base=False)
            parents = bis.merge_parents(repo, merge)
            self.assertEqual(parents[1], head)
            self.assertTrue(bis.tree_identical(repo, merge, head))
        with tempfile.TemporaryDirectory() as tmp:
            repo, merge, head = self.make_repo(Path(tmp), move_base=True)
            self.assertFalse(bis.tree_identical(repo, merge, head))

    def test_a_plain_commit_has_no_merge_parents(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo, merge, head = self.make_repo(Path(tmp), move_base=False)
            self.assertIsNone(bis.merge_parents(repo, head))


class CliTests(unittest.TestCase):
    def test_non_merge_commit_annotates_and_exits_0(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo, merge, head = GitHelperTests().make_repo(Path(tmp), move_base=False)
            proc = subprocess.run([sys.executable, str(HERE / "binary_identity_shadow.py"), "measure",
                                   "--build-dir", tmp, "--source-root", str(repo), "--merge-sha", head,
                                   "--repository", "O/R", "--token", "t", "--work-dir", tmp],
                                  capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        line = [ln for ln in proc.stdout.splitlines() if ln.startswith(f"::notice title={bis.TITLE}::")]
        rec = json.loads(line[0].split("::", 2)[2])
        self.assertEqual((rec["schema"], rec["mode"], rec["verdict"]), (bis.SCHEMA, "shadow", "not_a_two_parent_merge"))

    def test_unreadable_build_dir_exits_2_without_annotation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo, merge, head = GitHelperTests().make_repo(Path(tmp), move_base=False)
            proc = subprocess.run([sys.executable, str(HERE / "binary_identity_shadow.py"), "measure",
                                   "--build-dir", str(Path(tmp) / "no-such-build"), "--source-root", str(repo),
                                   "--merge-sha", merge, "--repository", "O/R", "--token", "t", "--work-dir", tmp],
                                  capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 2, proc.stdout)
        self.assertNotIn("::notice", proc.stdout)


if __name__ == "__main__":
    unittest.main()
