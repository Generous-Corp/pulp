#!/usr/bin/env python3
"""Extra edge tests for tools/scripts/iwyu_annotate.py."""

from __future__ import annotations

import contextlib
import io
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import iwyu_annotate as ia  # noqa: E402


class ParserEdgeTests(unittest.TestCase):
    def test_terminator_without_blank_line_ends_add_block(self) -> None:
        text = """\
core/foo.hpp should add these lines:
#include <string>  // for string
The full include-list for core/foo.hpp:
#include <vector>  // would be ignored outside add block
---
"""

        self.assertEqual(
            list(ia.iter_add_findings(text.splitlines())),
            [("core/foo.hpp", "<string>", "for string")],
        )

    def test_non_matching_hint_line_inside_add_block_is_ignored(self) -> None:
        text = """\
core/foo.hpp should add these lines:
#include <memory>  // for unique_ptr
namespace std { class allocator; }
#include "local.hpp"
"""

        self.assertEqual(
            list(ia.iter_add_findings(text.splitlines())),
            [
                ("core/foo.hpp", "<memory>", "for unique_ptr"),
                ("core/foo.hpp", '"local.hpp"', ""),
            ],
        )


class RenderingEdgeTests(unittest.TestCase):
    def test_annotation_escapes_percent_cr_and_lf(self) -> None:
        out = ia.render_annotation("core/foo.hpp", "<memory>", "100%\r\nready")

        self.assertIn("100%25%0D%0Aready", out)

    def test_blocking_summary_omits_advisory_paragraph(self) -> None:
        summary = ia.render_summary(
            [("core/foo.hpp", "<memory>", "for unique_ptr")],
            advisory=False,
            flip_date="2026-05-05",
        )

        self.assertIn("Found **1**", summary)
        self.assertNotIn("This check is **advisory**", summary)


class MainTests(unittest.TestCase):
    def test_main_reads_input_filters_changed_and_appends_summary(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            input_path = root / "iwyu.txt"
            changed_path = root / "changed.txt"
            summary_path = root / "summary.md"
            input_path.write_text(
                """\
core/keep.hpp should add these lines:
#include <memory>  // for unique_ptr

core/drop.hpp should add these lines:
#include <vector>  // for vector
""",
                encoding="utf-8",
            )
            changed_path.write_text("core/keep.hpp\n", encoding="utf-8")
            summary_path.write_text("before\n", encoding="utf-8")

            stdout = io.StringIO()
            stderr = io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                rc = ia.main(
                    [
                        "--input",
                        str(input_path),
                        "--changed-files-from",
                        str(changed_path),
                        "--summary-file",
                        str(summary_path),
                        "--blocking",
                    ]
                )

            self.assertEqual(rc, 0)
            self.assertIn("core/keep.hpp", stdout.getvalue())
            self.assertNotIn("core/drop.hpp", stdout.getvalue())
            self.assertIn("Found **1**", stderr.getvalue())
            summary_text = summary_path.read_text(encoding="utf-8")
            self.assertTrue(summary_text.startswith("before\n"))
            self.assertIn("Found **1**", summary_text)
            self.assertNotIn("This check is **advisory**", summary_text)

    def test_main_reads_stdin_when_no_input_path(self) -> None:
        stdout = io.StringIO()
        stderr = io.StringIO()
        stdin = io.StringIO(
            """\
core/stdin.cpp should add these lines:
#include <string>
"""
        )

        with mock.patch.object(sys, "stdin", stdin), \
             contextlib.redirect_stdout(stdout), \
             contextlib.redirect_stderr(stderr):
            rc = ia.main([])

        self.assertEqual(rc, 0)
        self.assertIn("core/stdin.cpp", stdout.getvalue())
        self.assertIn("Found **1**", stderr.getvalue())

    def test_main_warns_when_summary_file_cannot_be_written(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            input_path = root / "iwyu.txt"
            input_path.write_text("", encoding="utf-8")

            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                rc = ia.main(["--input", str(input_path), "--summary-file", str(root)])

            self.assertEqual(rc, 0)
            self.assertIn("warning: could not write summary:", stderr.getvalue())

    def test_script_entrypoint_runs_from_stdin(self) -> None:
        proc = subprocess.run(
            [sys.executable, str(HERE / "iwyu_annotate.py")],
            input=(
                "core/entrypoint.cpp should add these lines:\n"
                "#include <memory>  // for unique_ptr\n"
            ),
            text=True,
            capture_output=True,
            check=False,
        )

        self.assertEqual(proc.returncode, 0)
        self.assertIn("core/entrypoint.cpp", proc.stdout)
        self.assertIn("Found **1**", proc.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
