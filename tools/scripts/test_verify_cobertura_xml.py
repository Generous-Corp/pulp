#!/usr/bin/env python3
"""Tests for tools/scripts/verify_cobertura_xml.py (A2 first cut)."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SCRIPT = REPO_ROOT / "tools" / "scripts" / "verify_cobertura_xml.py"


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        capture_output=True, text=True, check=False,
    )


class VerifyCoberturaXmlTests(unittest.TestCase):
    def _write(self, content: str) -> pathlib.Path:
        f = tempfile.NamedTemporaryFile(
            mode="w", suffix=".xml", delete=False, encoding="utf-8",
        )
        f.write(content)
        f.close()
        return pathlib.Path(f.name)

    def test_missing_file_is_failure(self) -> None:
        r = run("/nonexistent/coverage.xml")
        self.assertEqual(r.returncode, 1)
        self.assertIn("missing or empty", r.stderr)

    def test_empty_file_is_failure(self) -> None:
        with tempfile.NamedTemporaryFile(suffix=".xml") as f:
            r = run(f.name)
        self.assertEqual(r.returncode, 1)
        self.assertIn("missing or empty", r.stderr)

    def test_unparseable_is_failure(self) -> None:
        path = self._write("not-xml at all")
        try:
            r = run(str(path))
        finally:
            path.unlink()
        self.assertEqual(r.returncode, 1)
        self.assertIn("failed to parse", r.stderr)

    def test_lines_valid_zero_is_failure_with_hint(self) -> None:
        path = self._write('<coverage lines-valid="0"></coverage>')
        try:
            r = run(str(path), "--label", "test.xml", "--hint", "check upstream")
        finally:
            path.unlink()
        self.assertEqual(r.returncode, 1)
        self.assertIn("structurally empty", r.stderr)
        self.assertIn("check upstream", r.stderr)

    def test_lines_valid_nonzero_passes(self) -> None:
        path = self._write('<coverage lines-valid="42"></coverage>')
        try:
            r = run(str(path))
        finally:
            path.unlink()
        self.assertEqual(r.returncode, 0, msg=r.stderr)
        self.assertIn("lines-valid=42", r.stdout)

    def test_label_appears_in_messages(self) -> None:
        path = self._write('<coverage lines-valid="0"></coverage>')
        try:
            r = run(str(path), "--label", "py-lane.xml")
        finally:
            path.unlink()
        self.assertEqual(r.returncode, 1)
        self.assertIn("py-lane.xml", r.stderr)


if __name__ == "__main__":
    unittest.main()
