#!/usr/bin/env python3
"""Tests for migration_note_guard.py against throwaway git ranges."""

from __future__ import annotations

import contextlib
import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import migration_note_guard as guard  # noqa: E402

NOTE = "---\nversion = \"1.0.0\"\n---\n\nbody\n"


class GuardTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.git("init", "-q", "-b", "main")
        for key, value in (("user.email", "t@e"), ("user.name", "t"),
                           ("commit.gpgsign", "false")):
            self.git("config", key, value)
        self.write("docs/migrations/v0.9.0.md", NOTE)
        self.commit("seed")
        self.git("checkout", "-q", "-b", "feature")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def git(self, *args: str) -> str:
        return subprocess.run(["git", "-C", str(self.root), *args], capture_output=True,
                              text=True, encoding="utf-8", check=True).stdout.strip()

    def write(self, rel: str, text: str) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def commit(self, message: str) -> str:
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)
        return self.git("rev-parse", "HEAD")

    def check(self) -> int:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return guard.main(["--root", str(self.root), "--base", "main"])

    def test_a_hand_written_released_note_is_refused(self) -> None:
        self.write("docs/migrations/v1.0.0.md", NOTE)
        sha = self.commit("feat: knob\n\nadds a migration note")
        self.assertEqual(guard.hand_written_notes(self.root, "main"),
                         [("docs/migrations/v1.0.0.md", sha)])
        self.assertEqual(self.check(), 1)

    def test_an_unreleased_note_passes(self) -> None:
        self.write("docs/migrations/unreleased/knob.md", '---\nsummary = "s"\n---\n\nbody\n')
        self.commit("feat: knob")
        self.assertEqual(self.check(), 0)

    def test_a_bump_commit_may_add_the_folded_note(self) -> None:
        self.write("docs/migrations/v1.0.0.md", NOTE)
        self.commit("chore: bump versions\n\nVersion-Bump-Applied: " + "a" * 40)
        self.assertEqual(self.check(), 0)

    def test_editing_a_released_note_passes(self) -> None:
        self.write("docs/migrations/v0.9.0.md", NOTE + "correction\n")
        self.commit("docs: correct the 0.9.0 note")
        self.assertEqual(self.check(), 0)

    def test_a_note_main_added_after_the_branch_point_is_not_this_ranges(self) -> None:
        self.git("checkout", "-q", "main")
        self.write("docs/migrations/v1.0.0.md", NOTE)
        self.commit("chore: bump versions\n\nVersion-Bump-Applied: " + "b" * 40)
        self.git("checkout", "-q", "feature")
        self.write("docs/other.md", "x\n")
        self.commit("docs: unrelated")
        self.git("merge", "-q", "--no-edit", "main")
        self.assertEqual(self.check(), 0)

    def test_an_unreadable_base_is_not_a_pass(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(guard.main(["--root", str(self.root), "--base", "nope"]), 2)


if __name__ == "__main__":
    unittest.main()
