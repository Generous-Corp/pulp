#!/usr/bin/env python3
"""Tests for tools/scripts/verify_cobertura_xml.py (A2 first cut)."""

from __future__ import annotations

import pathlib
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

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

    def test_missing_file_failure_includes_hint_when_provided(self) -> None:
        r = run(
            "/nonexistent/coverage.xml",
            "--label",
            "native.xml",
            "--hint",
            "check coverage build",
        )
        self.assertEqual(r.returncode, 1)
        self.assertIn("native.xml is missing or empty", r.stderr)
        self.assertIn("check coverage build", r.stderr)

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

    def test_unparseable_failure_uses_custom_label(self) -> None:
        path = self._write("not-xml at all")
        try:
            r = run(str(path), "--label", "merged.xml")
        finally:
            path.unlink()
        self.assertEqual(r.returncode, 1)
        self.assertIn("merged.xml failed to parse", r.stderr)

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

    def test_lines_valid_with_whitespace_passes(self) -> None:
        path = self._write('<coverage lines-valid=" 8 "></coverage>')
        try:
            r = run(str(path), "--label", "coverage.xml")
        finally:
            path.unlink()
        self.assertEqual(r.returncode, 0, msg=r.stderr)
        self.assertIn("coverage.xml has lines-valid=8", r.stdout)

    def test_negative_lines_valid_is_rejected(self) -> None:
        path = self._write('<coverage lines-valid="-1"></coverage>')
        try:
            r = run(str(path), "--label", "coverage.xml")
        finally:
            path.unlink()
        self.assertEqual(r.returncode, 1)
        self.assertIn("lines-valid=-1", r.stderr)

    def test_label_appears_in_messages(self) -> None:
        path = self._write('<coverage lines-valid="0"></coverage>')
        try:
            r = run(str(path), "--label", "py-lane.xml")
        finally:
            path.unlink()
        self.assertEqual(r.returncode, 1)
        self.assertIn("py-lane.xml", r.stderr)

    def test_non_numeric_lines_valid_is_structurally_empty(self) -> None:
        path = self._write('<coverage lines-valid="many"></coverage>')
        try:
            r = run(str(path), "--label", "native.xml", "--hint", "rerun coverage")
        finally:
            path.unlink()

        self.assertEqual(r.returncode, 1)
        self.assertIn("native.xml is structurally empty", r.stderr)
        self.assertIn("rerun coverage", r.stderr)

    def test_missing_lines_valid_defaults_to_structurally_empty(self) -> None:
        path = self._write("<coverage></coverage>")
        try:
            r = run(str(path), "--label", "python.xml")
        finally:
            path.unlink()

        self.assertEqual(r.returncode, 1)
        self.assertIn("python.xml is structurally empty", r.stderr)

    def test_success_uses_custom_label_for_size_and_lines(self) -> None:
        path = self._write('<coverage lines-valid="7"></coverage>')
        try:
            r = run(str(path), "--label", "python.xml")
        finally:
            path.unlink()

        self.assertEqual(r.returncode, 0, msg=r.stderr)
        self.assertIn("python.xml size:", r.stdout)
        self.assertIn("python.xml has lines-valid=7", r.stdout)

    def test_success_default_label_uses_basename_for_size_and_lines(self) -> None:
        path = self._write('<coverage lines-valid="5"></coverage>')
        try:
            r = run(str(path))
        finally:
            path.unlink()

        self.assertEqual(r.returncode, 0, msg=r.stderr)
        self.assertIn(f"{path.name} size:", r.stdout)
        self.assertIn(f"{path.name} has lines-valid=5", r.stdout)

    def test_script_entrypoint_exits_zero(self) -> None:
        path = self._write('<coverage lines-valid="3"></coverage>')
        try:
            with mock.patch.object(sys, "argv", [str(SCRIPT), str(path)]):
                with self.assertRaises(SystemExit) as cm:
                    runpy.run_path(str(SCRIPT), run_name="__main__")
        finally:
            path.unlink()
        self.assertEqual(cm.exception.code, 0)

    def test_importing_script_does_not_execute_main(self) -> None:
        module_globals = runpy.run_path(
            str(SCRIPT),
            run_name="verify_cobertura_xml_imported",
        )

        self.assertIn("main", module_globals)


if __name__ == "__main__":
    unittest.main()
