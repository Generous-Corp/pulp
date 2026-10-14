#!/usr/bin/env python3
"""Tests for tools/toolchain/update.py, run against a copy of the checkout's
toolchain files. The pinned mise is replaced by a stand-in lock writer, so
nothing here touches the network."""
from __future__ import annotations

import hashlib
import re
import tempfile
import unittest
from pathlib import Path

import generate
import update

REPO = generate.ROOT


class UpdateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.td = tempfile.TemporaryDirectory()
        self.root = Path(self.td.name)
        for path in update.owned_files(REPO):
            target = self.root / path.relative_to(REPO)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(path.read_bytes())
        self.addCleanup(self.td.cleanup)
        self.original_writer = generate.write_mise_lock
        self.addCleanup(setattr, generate, "write_mise_lock", self.original_writer)
        self.original_fetch = generate.fetch_shipyard_assets
        self.addCleanup(setattr, generate, "fetch_shipyard_assets", self.original_fetch)
        self.addCleanup(setattr, generate, "require_pinned_mise", generate.require_pinned_mise)
        generate.require_pinned_mise = lambda mise, data: None

    def snapshot(self) -> dict[Path, bytes]:
        return {p: p.read_bytes() for p in update.owned_files(self.root)}

    def fake_lock_writer(self, old: str, new: str):
        """Stand-in for `mise lock`: the cmake rows move from old to new."""

        def write(pulp_root: Path, data: dict, mise: Path) -> str:
            lock = pulp_root / generate.MISE_LOCK
            text = lock.read_text(encoding="utf-8")
            start = text.index("[[tools.cmake]]")
            end = text.index("[[tools.", start + 1)
            section = text[start:end].replace(old, new)
            section = re.sub(r'checksum = "sha256:([0-9a-f]{64})"',
                             lambda m: f'checksum = "sha256:{hashlib.sha256(m.group(1).encode()).hexdigest()}"', section)
            lock.write_text(text[:start] + section + text[end:], encoding="utf-8")
            return hashlib.sha256(lock.read_bytes()).hexdigest()

        return write

    def test_rust_bump_needs_no_mise_and_reports_the_pin(self) -> None:
        table = update.update(self.root, "rust", "1.98.0", None, "2026-10-14")
        self.assertIn("| rust pin | all | 1.97.1 | 1.98.0 |", table)
        data, _ = generate.load(self.root / "tools/toolchain/manifest.toml")
        self.assertEqual("target", data["evidence"]["rust"]["status"])
        self.assertIn("2026-10-14", data["evidence"]["rust"]["source"])
        self.assertIn('rust = "1.98.0"', (self.root / ".shipyard/vm-image.toml").read_text(encoding="utf-8"))
        self.assertIn('"rust": "1.98.0"', (self.root / generate.EXPECTED).read_text(encoding="utf-8"))
        generate.generate(self.root, None, True)

    def test_locked_tool_bump_lists_every_lock_row_that_moved(self) -> None:
        self.assertIn("[[tools.cmake]]", (self.root / generate.MISE_LOCK).read_text(encoding="utf-8"))
        generate.write_mise_lock = self.fake_lock_writer("4.3.3", "4.3.4")
        table = update.update(self.root, "cmake", "4.3.4", Path("/pinned/mise"), "2026-10-14")
        self.assertIn("| cmake pin | all | 4.3.3 | 4.3.4 |", table)
        lock_rows = [row for row in table if row.startswith("| cmake lock |")]
        self.assertEqual(len(generate.load()[0]["mise"]["lock_platforms"]), len(lock_rows))
        self.assertTrue(all(" 4.3.3 sha256:" in row and " 4.3.4 sha256:" in row for row in lock_rows))
        self.assertTrue(any(row.startswith("| mise.toolchain.lock sha256 |") for row in table))
        self.assertFalse(any(row.startswith("| ninja") for row in table))

    def test_locked_tool_without_the_pinned_mise_changes_nothing(self) -> None:
        before = self.snapshot()
        with self.assertRaises(ValueError) as ctx:
            update.update(self.root, "cmake", "4.3.4", None, "2026-10-14")
        self.assertIn("pass --mise", str(ctx.exception))
        self.assertEqual(before, self.snapshot())

    def test_a_failed_lock_restores_every_file(self) -> None:
        before = self.snapshot()

        def broken(pulp_root: Path, data: dict, mise: Path) -> str:
            (pulp_root / generate.MISE_LOCK).write_text("half written", encoding="utf-8")
            raise ValueError("mise lock cmake failed")

        generate.write_mise_lock = broken
        with self.assertRaises(ValueError):
            update.update(self.root, "cmake", "4.3.4", Path("/pinned/mise"), "2026-10-14")
        self.assertEqual(before, self.snapshot())

    def test_shipyard_bump_lists_assets(self) -> None:
        generate.fetch_shipyard_assets = lambda repo, tag: {"shipyard-linux-x64": "c" * 64}
        table = update.update(self.root, "shipyard", "v0.300.0", None, "2026-10-14")
        self.assertIn("| shipyard pin | all | v0.269.1 | v0.300.0 |", table)
        self.assertIn("| shipyard asset shipyard-linux-x64 | shipyard-linux-x64 | 327dd4b5f72a | cccccccccccc |", table)
        self.assertTrue(any("shipyard-macos-arm64.dmg | 73c426a8a6f9 | absent" in row for row in table))

    def test_refusals(self) -> None:
        before = self.snapshot()
        for tool, version, needle in (
            ("cmake", "4.3.3", "already pinned"),
            ("gcc", "15.1.0", "unknown tool"),
            ("mise", "2026.11.0", "reviewed source build"),
            ("rust", "stable", "non-exact"),
        ):
            with self.subTest(tool=tool), self.assertRaises(ValueError) as ctx:
                update.update(self.root, tool, version, Path("/pinned/mise"), "2026-10-14")
            self.assertIn(needle, str(ctx.exception))
        self.assertEqual(before, self.snapshot())

    def test_no_change_is_an_empty_table(self) -> None:
        data, _ = generate.load()
        lock = (REPO / generate.MISE_LOCK).read_text(encoding="utf-8")
        self.assertEqual([], update.diff_table(data, data, lock, lock))


if __name__ == "__main__":
    unittest.main()
