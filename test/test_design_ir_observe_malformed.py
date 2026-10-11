#!/usr/bin/env python3
"""pulp-design-ir-observe refuses malformed DesignIR with an error, never a crash.

A truncated document used to terminate the observer by signal: SIGBUS when the
cut fell inside a string literal (the JSON parser read past the buffer) and
SIGABRT on an uncaught parse error elsewhere. Both must now exit 1 with a
diagnostic that names the problem.

Usage: test_design_ir_observe_malformed.py --observer <pulp-design-ir-observe>
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

FIXTURE = Path(__file__).resolve().parent / "fixtures/design-ir-observe/controls.ir.json"
OBSERVER: Path | None = None


class MalformedDesignIrTests(unittest.TestCase):
    def observe(self, document: bytes) -> subprocess.CompletedProcess:
        with tempfile.TemporaryDirectory() as work:
            source = Path(work) / "design.ir.json"
            source.write_bytes(document)
            return subprocess.run(
                [str(OBSERVER), "--input", str(source), "--render", str(Path(work) / "render.png"),
                 "--layout", str(Path(work) / "layout.json"), "--width", "240", "--height", "160"],
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)

    def assert_refused(self, document: bytes, reason: str) -> None:
        result = self.observe(document)
        self.assertEqual(result.returncode, 1, f"exit {result.returncode}: {result.stderr[-400:]}")
        self.assertIn("invalid DesignIR", result.stderr)
        self.assertIn(reason, result.stderr)

    def test_well_formed_fixture_still_renders(self) -> None:
        self.assertEqual(self.observe(FIXTURE.read_bytes()).returncode, 0)

    def test_cut_inside_a_string_literal(self) -> None:
        data = FIXTURE.read_bytes()
        quote = data.index(b'"', data.index(b'"name"') + len(b'"name"'))
        self.assert_refused(data[:quote + 3], "unterminated string")

    def test_cut_between_members(self) -> None:
        data = FIXTURE.read_bytes()
        self.assert_refused(data[:data.index(b'"root"')], "truncated")


def main() -> int:
    global OBSERVER
    parser = argparse.ArgumentParser()
    parser.add_argument("--observer", required=True, type=Path)
    args, rest = parser.parse_known_args()
    OBSERVER = args.observer
    return 0 if unittest.main(argv=[sys.argv[0], *rest], exit=False).result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
