#!/usr/bin/env python3
"""Tests for hotspot_edit_guard.py against real git repositories."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "hotspot_edit_guard.py"
sys.path.insert(0, str(HERE))
import hotspot_edit_guard as guard  # noqa: E402

GIT_ENV = {"PATH": "/usr/bin:/bin:/opt/homebrew/bin:/usr/local/bin", "HOME": "/nonexistent",
           "GIT_CONFIG_NOSYSTEM": "1", "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
           "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}
AGGREGATE = "test/ctest_script_inputs.json"
FRAGMENT = "test/ctest_script_inputs/tests/alpha.json"


class Repo:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.git("init", "-q", "-b", "main")
        self.write(guard.REGISTRY.as_posix(), (HERE / "hotspot_edit_guard.json").read_text(encoding="utf-8"))

    def git(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(["git", "-C", str(self.root), *args], env=GIT_ENV, capture_output=True,
                              text=True, check=True)

    def write(self, rel: str, text: str) -> None:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")

    def commit(self, msg: str, *paths: str) -> None:
        self.git("add", "--all", "--", *paths)
        self.git("commit", "-q", "-m", msg)

    def run(self, *args: str, env: dict | None = None) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.root), *args],
                              capture_output=True, text=True, env={**GIT_ENV, **(env or {})}, timeout=60)


def repo_with_aggregate(tmp: str) -> Repo:
    """main as it was before the split: the single aggregate file."""
    repo = Repo(Path(tmp))
    repo.write(AGGREGATE, json.dumps({"tests": {"alpha": {}}}) + "\n")
    repo.commit("base", ".")
    repo.git("branch", "base-ref")
    return repo


class GuardTests(unittest.TestCase):
    def test_modifying_the_aggregate_is_refused_and_names_the_fragments(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = repo_with_aggregate(tmp)
            repo.write(AGGREGATE, json.dumps({"tests": {"alpha": {}, "bravo": {}}}) + "\n")
            repo.commit("hand edit", AGGREGATE)
            proc = repo.run("--base", "base-ref")
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            self.assertIn(f"file={AGGREGATE}; cause_class=HOTSPOT_AGGREGATE_EDIT", proc.stdout)
            self.assertIn("test/ctest_script_inputs/", proc.stdout)
            self.assertIn("script_test_inputs.py", proc.stdout)

    def test_an_uncommitted_edit_is_refused_too(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = repo_with_aggregate(tmp)
            repo.write(AGGREGATE, "{}\n")
            self.assertEqual(repo.run("--base", "base-ref").returncode, 1)

    def test_deleting_the_aggregate_and_writing_fragments_passes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = repo_with_aggregate(tmp)
            repo.git("rm", "-q", AGGREGATE)
            repo.write(FRAGMENT, "{}\n")
            repo.commit("split", "test")
            proc = repo.run("--base", "base-ref")
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertIn("OK", proc.stdout)

    def test_re_adding_the_aggregate_after_the_split_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = repo_with_aggregate(tmp)
            repo.git("rm", "-q", AGGREGATE)
            repo.write(FRAGMENT, "{}\n")
            repo.commit("split", "test")
            repo.git("branch", "-f", "base-ref", "HEAD")
            repo.write(AGGREGATE, "{}\n")
            repo.commit("tool writes the old single file", AGGREGATE)
            proc = repo.run("--base", "base-ref")
            self.assertEqual(proc.returncode, 1, proc.stdout)
            self.assertIn("re-added", proc.stdout)

    def test_a_fragment_edit_and_unrelated_edits_pass(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = repo_with_aggregate(tmp)
            repo.write(FRAGMENT, "{}\n")
            repo.write("README.md", "x\n")
            repo.commit("normal change", ".")
            self.assertEqual(repo.run("--base", "base-ref").returncode, 0)

    def test_the_base_is_the_merge_base_not_the_moving_tip(self) -> None:
        """main editing the aggregate after the branch point is not this branch's edit."""
        with tempfile.TemporaryDirectory() as tmp:
            repo = repo_with_aggregate(tmp)
            repo.git("checkout", "-q", "-b", "pr")
            repo.write("README.md", "x\n")
            repo.commit("pr", "README.md")
            repo.git("checkout", "-q", "main")
            repo.write(AGGREGATE, "{}\n")
            repo.commit("main moves", AGGREGATE)
            repo.git("checkout", "-q", "pr")
            self.assertEqual(repo.run("--base", "main").returncode, 0)

    def test_a_merge_checkout_resolves_its_first_parent(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = repo_with_aggregate(tmp)
            repo.git("checkout", "-q", "-b", "pr")
            repo.write(AGGREGATE, "{}\n")
            repo.commit("pr edits", AGGREGATE)
            repo.git("checkout", "-q", "main")
            repo.write("README.md", "x\n")
            repo.commit("main", "README.md")
            repo.git("merge", "-q", "--no-ff", "--no-edit", "pr")
            proc = repo.run(env={"GITHUB_EVENT_NAME": "pull_request"})
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            self.assertIn("HEAD^1", proc.stdout)

    def test_no_base_and_a_bad_registry_are_errors_not_passes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = repo_with_aggregate(tmp)
            self.assertEqual(repo.run().returncode, 2)  # no origin/main, no event
            skipped = repo.run("--skip-without-base")
            self.assertEqual(skipped.returncode, 77)
            self.assertIn("SKIPPED", skipped.stdout)
            repo.write(guard.REGISTRY.as_posix(), json.dumps({"schema": "nope"}))
            self.assertEqual(repo.run("--base", "base-ref").returncode, 2)

    def test_the_checked_in_registry_is_valid_and_its_fragments_exist(self) -> None:
        root = HERE.parents[1]
        for spot in guard.load_registry(root):
            self.assertTrue((root / spot["fragments"]).is_dir(),
                            f"{spot['path']} is registered but {spot['fragments']} does not exist: "
                            "register a hotspot only once its fragments exist")
            self.assertFalse((root / spot["path"]).exists(), f"{spot['path']} is back beside its fragments")


if __name__ == "__main__":
    unittest.main()
