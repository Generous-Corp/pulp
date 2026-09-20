#!/usr/bin/env python3
"""A PR must not re-pin the generated ledger; the version bot does that.

Every assertion here is paired with a control on the same instrument, because
the failures this guard exists to catch and the state where it is simply inert
produce the same exit code.
"""
from __future__ import annotations

import json
import re
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

    def test_the_real_ledger_inventory_carries_repo_and_state(self) -> None:
        """The inventory key is (repo, path, state); if the shipped ledger does
        not actually carry `repo`/`state`, an inventory change that only moves
        one of those fields would read as identity-only and be rejected."""
        rows = guard._inventory_from_text(
            (REPO / guard.HANDOFF).read_text(encoding="utf-8"))
        self.assertIsNotNone(rows)
        self.assertGreater(len(rows), 50)
        self.assertTrue(any(repo for repo, _, _ in rows),
                        "no row carries a repo; the inventory key is degenerate")
        self.assertTrue(any(state for _, _, state in rows),
                        "no row carries a state; the inventory key is degenerate")

    # ── fixture ────────────────────────────────────────────────────────────

    LEDGER_ROWS = [
        {"repo": "pulp", "path": "core/view/src/view.cpp", "state": "shared"},
        {"repo": "pulp", "path": "tools/scripts/pinned_tool.py", "state": "shared"},
    ]

    def _write_ledger(self, root: Path, rows: list[dict], identity: str) -> None:
        doc = root / guard.HANDOFF
        doc.parent.mkdir(parents=True, exist_ok=True)
        doc.write_text(json.dumps({
            "entries": [{"pulp_paths": [dict(r, revision=identity) for r in rows]}]
        }, indent=2) + "\n")
        receipt = root / guard.RECEIPT
        receipt.parent.mkdir(parents=True, exist_ok=True)
        receipt.write_text(json.dumps({"source_commit": identity}) + "\n")

    def _repo(self, td: str) -> Path:
        root = Path(td)
        git("init", "-q", cwd=root)
        git("config", "user.email", "t@e.st", cwd=root)
        git("config", "user.name", "t", cwd=root)
        self._write_ledger(root, self.LEDGER_ROWS, "aaaaaaa")
        for rel in ("core/view/src/view.cpp", "tools/scripts/pinned_tool.py"):
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text("a\n")
        (root / "unpinned.txt").write_text("a\n")
        git("add", "-A", cwd=root)
        git("commit", "-qm", "base", cwd=root)
        git("branch", "-f", "base", cwd=root)
        return root

    def run_guard(self, root: Path, mode: str = "report") -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable,
             str(Path(__file__).parent / "gpu_handoff_pin_freshness.py"),
             "--base", "base", "--root", str(root), "--mode", mode],
            cwd=root, capture_output=True, text=True,
        )

    # ── (i) identity-only re-pin ───────────────────────────────────────────

    def test_identity_only_repin_fails(self) -> None:
        """The work the bump commit now carries must not also happen in a PR."""
        with tempfile.TemporaryDirectory() as td:
            root = self._repo(td)
            (root / "core/view/src/view.cpp").write_text("b\n")
            self._write_ledger(root, self.LEDGER_ROWS, "bbbbbbb")
            git("add", "-A", cwd=root); git("commit", "-qm", "edit + repin", cwd=root)
            proc = self.run_guard(root)
            self.assertEqual(proc.returncode, 1, proc.stderr)
            self.assertIn(str(guard.HANDOFF), proc.stderr)
            self.assertIn("identity-only", proc.stderr)

    def test_receipt_only_repin_fails(self) -> None:
        """The receipt binds to the ledger's bytes, so moving it alone is the
        same in-PR re-pin wearing one file instead of two."""
        with tempfile.TemporaryDirectory() as td:
            root = self._repo(td)
            (root / guard.RECEIPT).write_text(json.dumps({"source_commit": "ccc"}) + "\n")
            git("add", "-A", cwd=root); git("commit", "-qm", "receipt only", cwd=root)
            proc = self.run_guard(root)
            self.assertEqual(proc.returncode, 1, proc.stderr)
            self.assertIn(str(guard.RECEIPT), proc.stderr)

    def test_inventory_change_with_a_repin_passes(self) -> None:
        """POSITIVE CONTROL for the case above: same two files, same identity
        move, but the row set changed — which is the one ledger edit a PR still
        owns. If this failed, the guard would reject the legitimate shape."""
        with tempfile.TemporaryDirectory() as td:
            root = self._repo(td)
            added = root / "core/view/src/added.cpp"
            added.write_text("a\n")
            rows = self.LEDGER_ROWS + [
                {"repo": "pulp", "path": "core/view/src/added.cpp", "state": "shared"},
            ]
            self._write_ledger(root, rows, "bbbbbbb")
            git("add", "-A", cwd=root); git("commit", "-qm", "add pinned path", cwd=root)
            proc = self.run_guard(root)
            self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_a_state_only_inventory_change_is_not_an_identity_repin(self) -> None:
        """`state` is editorial, not derived, so moving it is an inventory
        change even though no path was added or removed."""
        with tempfile.TemporaryDirectory() as td:
            root = self._repo(td)
            rows = [dict(r) for r in self.LEDGER_ROWS]
            rows[0]["state"] = "pulp-owned"
            self._write_ledger(root, rows, "bbbbbbb")
            git("add", "-A", cwd=root); git("commit", "-qm", "restate row", cwd=root)
            self.assertEqual(self.run_guard(root).returncode, 0)

    # ── (ii) a pinned path that stops existing ─────────────────────────────

    def test_deleting_a_pinned_path_without_updating_the_inventory_fails(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = self._repo(td)
            (root / "core/view/src/view.cpp").unlink()
            git("add", "-A", cwd=root); git("commit", "-qm", "delete pinned", cwd=root)
            proc = self.run_guard(root)
            self.assertEqual(proc.returncode, 1, proc.stderr)
            self.assertIn("core/view/src/view.cpp", proc.stderr)
            self.assertIn("deleted or renamed", proc.stderr)

    def test_renaming_a_pinned_path_without_updating_the_inventory_fails(self) -> None:
        """A rename removes the pinned path exactly as a delete does, and the
        ledger keeps naming the source side."""
        with tempfile.TemporaryDirectory() as td:
            root = self._repo(td)
            git("mv", "core/view/src/view.cpp", "core/view/src/renamed.cpp", cwd=root)
            git("commit", "-qm", "rename pinned", cwd=root)
            proc = self.run_guard(root)
            self.assertEqual(proc.returncode, 1, proc.stderr)
            self.assertIn("core/view/src/view.cpp", proc.stderr)

    def test_deleting_a_pinned_path_and_dropping_its_row_passes(self) -> None:
        """POSITIVE CONTROL for the case above: the same deletion, correctly
        accompanied by the inventory decision the bot cannot make."""
        with tempfile.TemporaryDirectory() as td:
            root = self._repo(td)
            (root / "core/view/src/view.cpp").unlink()
            self._write_ledger(root, self.LEDGER_ROWS[1:], "bbbbbbb")
            git("add", "-A", cwd=root); git("commit", "-qm", "drop pinned row", cwd=root)
            proc = self.run_guard(root)
            self.assertEqual(proc.returncode, 0, proc.stderr)

    # ── everything else ────────────────────────────────────────────────────

    def test_an_ordinary_edit_to_a_pinned_path_passes(self) -> None:
        """The inversion itself: editing a pinned path and NOT re-pinning is
        now the correct shape. Under the previous rule this exact case failed."""
        with tempfile.TemporaryDirectory() as td:
            root = self._repo(td)
            (root / "core/view/src/view.cpp").write_text("b\n")
            git("add", "-A", cwd=root); git("commit", "-qm", "edit pinned", cwd=root)
            proc = self.run_guard(root)
            self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_unrelated_diff_passes(self) -> None:
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
            git("add", "-A", cwd=root); git("commit", "-qm", "broken", cwd=root)
            proc = self.run_guard(root)
            self.assertEqual(proc.returncode, 0)
            self.assertIn("nothing checked", proc.stderr)

    def test_hint_mode_never_fails(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = self._repo(td)
            (root / "core/view/src/view.cpp").write_text("b\n")
            self._write_ledger(root, self.LEDGER_ROWS, "bbbbbbb")
            git("add", "-A", cwd=root); git("commit", "-qm", "edit + repin", cwd=root)
            proc = self.run_guard(root, mode="hint")
            self.assertEqual(proc.returncode, 0)
            self.assertIn("identity-only", proc.stderr)

    def test_both_push_paths_run_the_freshness_check(self) -> None:
        """`gates.sh` is run by convention; the hook is run by git.

        Wiring only `gates.sh` would leave the rule holding for whoever
        remembered to run it -- which is exactly the state this guard was in
        before the inversion. Asserting the script *name* appears cannot prove
        it, because both surfaces hold the path in a shell variable and the
        name survives the line that runs it being deleted. Find the
        assignment, then require a live command that executes that variable.
        """
        script = "gpu_handoff_pin_freshness.py"
        for surface in ("tools/scripts/gates.sh", ".githooks/pre-push"):
            text = (REPO / surface).read_text(encoding="utf-8")
            assigned = re.findall(
                rf'^\s*(\w+)="[^"]*{re.escape(script)}"', text, re.MULTILINE)
            self.assertEqual(
                len(assigned), 1,
                f"{surface} does not name {script} exactly once")
            variable = assigned[0]
            invocations = [
                line for line in text.splitlines()
                if not line.lstrip().startswith("#")
                and f'"${variable}"' in line
                and "--mode" in line
            ]
            self.assertTrue(
                invocations,
                f"{surface} never runs ${variable}: the gate is registered but dead")


if __name__ == "__main__":
    unittest.main()
