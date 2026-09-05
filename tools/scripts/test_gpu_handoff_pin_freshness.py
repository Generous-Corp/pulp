#!/usr/bin/env python3
"""Editing a pinned path without refreshing the ledger must fail the gate."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import gpu_handoff_pin_freshness as guard  # noqa: E402

REPO = Path(__file__).parents[2]


def git(*args: str, cwd: Path) -> None:
    subprocess.run(["git", *args], cwd=cwd, check=True,
                   capture_output=True, text=True)


class PinFreshnessTests(unittest.TestCase):
    def test_the_real_ledger_parses_and_is_not_empty(self) -> None:
        """POSITIVE CONTROL: without this, every other assertion is vacuous."""
        pins = guard.pinned_paths(REPO)
        self.assertGreater(len(pins), 50, "parser found no rows; the gate is inert")
        self.assertIn("docs/status/gpu-vellum-handoff.yaml", str(guard.HANDOFF))

    def _repo(self, td: str) -> Path:
        root = Path(td)
        git("init", "-q", cwd=root)
        git("config", "user.email", "t@e.st", cwd=root)
        git("config", "user.name", "t", cwd=root)
        doc = root / guard.HANDOFF
        doc.parent.mkdir(parents=True, exist_ok=True)
        doc.write_text(json.dumps({
            "entries": [{"pulp_paths": [
                {"repo": "x", "path": "core/view/src/view.cpp"},
                {"repo": "x", "path": "tools/scripts/pinned_tool.py"},
            ]}]
        }))
        (root / "core" / "view" / "src").mkdir(parents=True)
        (root / "core" / "view" / "src" / "view.cpp").write_text("a\n")
        (root / "unpinned.txt").write_text("a\n")
        git("add", "-A", cwd=root)
        git("commit", "-qm", "base", cwd=root)
        git("branch", "-f", "base", cwd=root)
        return root

    def run_guard(self, root: Path) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(Path(__file__).parent / "gpu_handoff_pin_freshness.py"),
             "--base", "base", "--root", str(root)],
            cwd=root, capture_output=True, text=True,
        )

    def test_pinned_path_without_ledger_refresh_fails(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = self._repo(td)
            (root / "core" / "view" / "src" / "view.cpp").write_text("b\n")
            git("add", "-A", cwd=root); git("commit", "-qm", "edit", cwd=root)
            proc = self.run_guard(root)
            self.assertEqual(proc.returncode, 1, proc.stderr)
            self.assertIn("core/view/src/view.cpp", proc.stderr)
            self.assertIn("gpu_handoff_provenance.py write", proc.stderr)

    def test_pinned_path_with_ledger_refresh_passes(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = self._repo(td)
            (root / "core" / "view" / "src" / "view.cpp").write_text("b\n")
            doc = root / guard.HANDOFF
            d = json.loads(doc.read_text()); d["refreshed"] = True
            doc.write_text(json.dumps(d))
            git("add", "-A", cwd=root); git("commit", "-qm", "edit+refresh", cwd=root)
            self.assertEqual(self.run_guard(root).returncode, 0)

    def test_unpinned_path_is_ignored(self) -> None:
        """NEGATIVE CONTROL: the gate must not fire on unrelated work."""
        with tempfile.TemporaryDirectory() as td:
            root = self._repo(td)
            (root / "unpinned.txt").write_text("b\n")
            git("add", "-A", cwd=root); git("commit", "-qm", "unrelated", cwd=root)
            self.assertEqual(self.run_guard(root).returncode, 0)

    def test_an_unreadable_ledger_says_so_rather_than_passing_quietly(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = self._repo(td)
            (root / guard.HANDOFF).write_text("{ not json")
            (root / "core" / "view" / "src" / "view.cpp").write_text("b\n")
            git("add", "-A", cwd=root); git("commit", "-qm", "broken", cwd=root)
            proc = self.run_guard(root)
            self.assertIn("nothing checked", proc.stderr)

    def test_hint_mode_never_fails(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = self._repo(td)
            (root / "core" / "view" / "src" / "view.cpp").write_text("b\n")
            git("add", "-A", cwd=root); git("commit", "-qm", "edit", cwd=root)
            proc = subprocess.run(
                [sys.executable,
                 str(Path(__file__).parent / "gpu_handoff_pin_freshness.py"),
                 "--base", "base", "--root", str(root), "--mode", "hint"],
                cwd=root, capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0)
            self.assertIn("core/view/src/view.cpp", proc.stderr)


if __name__ == "__main__":
    unittest.main()
