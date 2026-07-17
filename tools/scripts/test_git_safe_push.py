#!/usr/bin/env python3
"""Regression tests for git_safe_push.sh."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("git_safe_push.sh")
GIT_ENV = os.environ.copy()
for name in (
    "GIT_DIR",
    "GIT_WORK_TREE",
    "GIT_INDEX_FILE",
    "GIT_OBJECT_DIRECTORY",
    "GIT_ALTERNATE_OBJECT_DIRECTORIES",
):
    GIT_ENV.pop(name, None)


class GitSafePushTest(unittest.TestCase):
    def run_git(self, repo: Path, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["git", *args],
            cwd=repo,
            env=GIT_ENV,
            check=True,
            capture_output=True,
            text=True,
        )

    def test_empty_and_nonempty_optional_argument_vectors(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            remote = root / "remote.git"
            repo = root / "repo"
            remote.mkdir()
            repo.mkdir()
            self.run_git(remote, "init", "--bare")
            self.run_git(repo, "init", "-b", "safe-push-test")
            self.run_git(repo, "config", "user.name", "Pulp Test")
            self.run_git(repo, "config", "user.email", "test@example.invalid")
            self.run_git(repo, "remote", "add", "origin", str(remote))

            tracked = repo / "tracked.txt"
            tracked.write_text("first\n", encoding="utf-8")
            self.run_git(repo, "add", "tracked.txt")
            self.run_git(repo, "commit", "-m", "first")

            without_extra = subprocess.run(
                ["/bin/bash", str(SCRIPT), "safe-push-test"],
                cwd=repo,
                env=GIT_ENV,
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertIn("verified", without_extra.stdout)

            tracked.write_text("second\n", encoding="utf-8")
            self.run_git(repo, "commit", "-am", "second")
            with_extra = subprocess.run(
                [
                    "/bin/bash",
                    str(SCRIPT),
                    "safe-push-test",
                    "--",
                    "--force-with-lease",
                ],
                cwd=repo,
                env=GIT_ENV,
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertIn("verified", with_extra.stdout)


if __name__ == "__main__":
    unittest.main()
