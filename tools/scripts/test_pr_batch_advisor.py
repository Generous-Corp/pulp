#!/usr/bin/env python3
"""The pre-push batching advisor logs every firing, and only firings.

Builds a throwaway repository with a fake ``origin/main``, runs the advisor as
the hook does, and reads the JSONL log it writes.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ADVISOR = Path(__file__).resolve().parent / "pr_batch_advisor.py"


def git(repo: Path, *args: str) -> str:
    return subprocess.run(
        ["git", *args], cwd=repo, check=True, capture_output=True, text=True
    ).stdout.strip()


def commit(repo: Path, path: str) -> None:
    f = repo / path
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_text(path + "\n")
    git(repo, "add", path)
    git(repo, "commit", "-q", "-m", f"touch {path}")


class AdvisorLog(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name) / "repo"
        self.repo.mkdir()
        self.log = Path(self.tmp.name) / "state" / "advice.jsonl"
        git(self.repo, "init", "-q", "-b", "main")
        git(self.repo, "config", "user.email", "t@example.com")
        git(self.repo, "config", "user.name", "t")
        commit(self.repo, "README.md")
        git(self.repo, "update-ref", "refs/remotes/origin/main", "HEAD")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_advisor(self) -> subprocess.CompletedProcess:
        env = dict(os.environ, PULP_PR_BATCH_ADVICE_LOG=str(self.log))
        env.pop("PULP_SKIP_PR_BATCH_ADVICE", None)
        return subprocess.run(
            [sys.executable, str(ADVISOR)],
            cwd=self.repo,
            env=env,
            capture_output=True,
            text=True,
        )

    def entries(self) -> list[dict]:
        if not self.log.exists():
            return []
        return [json.loads(l) for l in self.log.read_text().splitlines() if l]

    def test_related_branch_is_logged(self) -> None:
        git(self.repo, "checkout", "-q", "-b", "feature/a")
        commit(self.repo, "core/widget/a.cpp")
        git(self.repo, "checkout", "-q", "-b", "feature/b")
        commit(self.repo, "core/widget/b.cpp")
        result = self.run_advisor()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("feature/a", result.stderr)
        entries = self.entries()
        self.assertEqual(len(entries), 1, entries)
        e = entries[0]
        self.assertEqual(e["schema"], "pulp-pr-batch-advice/v1")
        self.assertEqual(e["branch"], "feature/b")
        self.assertEqual(e["head"], git(self.repo, "rev-parse", "HEAD"))
        self.assertEqual([r["branch"] for r in e["related"]], ["feature/a"])
        self.assertEqual(e["related"][0]["reason"], "contained")
        self.assertEqual(e["related"][0]["subsystems"], ["core/widget"])

    def test_unrelated_branch_writes_nothing(self) -> None:
        git(self.repo, "checkout", "-q", "-b", "feature/docs")
        commit(self.repo, "docs/guide/x.md")
        git(self.repo, "checkout", "-q", "main")
        git(self.repo, "checkout", "-q", "-b", "feature/core")
        commit(self.repo, "core/widget/a.cpp")
        result = self.run_advisor()
        self.assertEqual(result.returncode, 0, result.stderr)
        # Control: the advisor did examine the other live branch; it simply
        # judged it unrelated, so there is nothing to log.
        self.assertIn("docs/guide", git(self.repo, "diff", "--name-only", "origin/main...feature/docs"))
        self.assertNotIn("feature/docs", result.stderr)
        self.assertEqual(self.entries(), [])

    def test_unwritable_log_never_fails_the_push(self) -> None:
        blocker = Path(self.tmp.name) / "file-not-dir"
        blocker.write_text("x")
        self.log = blocker / "advice.jsonl"
        git(self.repo, "checkout", "-q", "-b", "feature/a")
        commit(self.repo, "core/widget/a.cpp")
        git(self.repo, "checkout", "-q", "-b", "feature/b")
        commit(self.repo, "core/widget/b.cpp")
        result = self.run_advisor()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("feature/a", result.stderr)


if __name__ == "__main__":
    unittest.main()
