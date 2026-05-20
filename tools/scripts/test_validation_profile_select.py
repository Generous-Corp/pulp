#!/usr/bin/env python3
"""Unit tests for validation_profile_select.

Run with:
    python3 tools/scripts/test_validation_profile_select.py
"""

from __future__ import annotations

import io
import json
import runpy
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))

import validation_profile_select as vps  # noqa: E402


class PathMatchingTests(unittest.TestCase):
    def test_import_design_tool_paths_match(self):
        self.assertTrue(vps.is_parser_only_path("tools/import-design/pulp_import_design.cpp"))
        self.assertTrue(vps.is_parser_only_path("tools/import-design/catalogs/yoga.tsv"))

    def test_import_validation_scripts_match(self):
        self.assertTrue(vps.is_parser_only_path("tools/import-validation/spectr-roundtrip.sh"))
        self.assertTrue(vps.is_parser_only_path("tools/import-validation/check_label_coverage.sh"))

    def test_import_ir_package_matches(self):
        self.assertTrue(vps.is_parser_only_path("packages/pulp-import-ir/src/diff.ts"))
        self.assertTrue(vps.is_parser_only_path("packages/pulp-import-ir/test/anchors.test.ts"))

    def test_parser_test_files_match(self):
        self.assertTrue(vps.is_parser_only_path("test/test_design_import.cpp"))
        self.assertTrue(vps.is_parser_only_path("test/test_design_import_claude_bundle.cpp"))
        self.assertTrue(vps.is_parser_only_path("test/test_cli_import_design.cpp"))
        self.assertTrue(vps.is_parser_only_path("test/test_widget_promotion.cpp"))

    def test_core_view_design_import_paths_match(self):
        self.assertTrue(vps.is_parser_only_path("core/view/src/design_import.cpp"))
        self.assertTrue(vps.is_parser_only_path("core/view/include/pulp/view/design_import.hpp"))
        self.assertTrue(vps.is_parser_only_path("core/view/js/import-runtime.js"))

    def test_unrelated_paths_do_not_match(self):
        self.assertFalse(vps.is_parser_only_path("core/format/src/vst3_adapter.cpp"))
        self.assertFalse(vps.is_parser_only_path("core/audio/src/buffer_view.cpp"))
        self.assertFalse(vps.is_parser_only_path("examples/pulp-gain/CMakeLists.txt"))
        self.assertFalse(vps.is_parser_only_path(".github/workflows/build.yml"))
        # CMakeLists at repo root is broad — always falls back to default.
        self.assertFalse(vps.is_parser_only_path("CMakeLists.txt"))

    def test_unrelated_core_view_paths_do_not_match(self):
        # The parser-only scope covers `design_import*` and the import
        # runtime JS — generic view code is broader.
        self.assertFalse(vps.is_parser_only_path("core/view/src/widget.cpp"))
        self.assertFalse(vps.is_parser_only_path("core/view/include/pulp/view/widget.hpp"))

    def test_recursive_match_handles_zero_and_multiple_segments(self):
        self.assertTrue(vps._match_recursive("docs/reference/imports/**", "docs/reference/imports"))
        self.assertTrue(vps._match_recursive("docs/reference/imports/**", "docs/reference/imports/designmd/ref.md"))
        self.assertFalse(vps._match_recursive("docs/reference/imports/**", "docs/reference/imports-extra/ref.md"))

    def test_recursive_match_requires_remaining_suffix(self):
        self.assertFalse(vps._match_recursive("tools/**/fixture.json", "tools/import-design"))
        self.assertFalse(vps._match_recursive("tools/import-design/*", "tools/import-design"))
        self.assertFalse(vps._match_recursive("tools/import-design/*.cpp", "tools/import-design/foo.ts"))


class ClassifyTests(unittest.TestCase):
    def test_all_parser_only_returns_parser(self):
        result = vps.classify([
            "tools/import-design/pulp_import_design.cpp",
            "test/test_design_import.cpp",
            "test/fixtures/imports/stitch/example.html",
        ])
        self.assertEqual(result.profile, "parser")
        self.assertEqual(len(result.unmatched), 0)

    def test_any_unrelated_path_returns_default(self):
        result = vps.classify([
            "tools/import-design/pulp_import_design.cpp",
            "core/format/src/vst3_adapter.cpp",
        ])
        self.assertEqual(result.profile, "default")
        self.assertEqual(len(result.unmatched), 1)
        self.assertIn("core/format/src/vst3_adapter.cpp", result.unmatched)

    def test_empty_diff_defaults_to_default(self):
        result = vps.classify([])
        self.assertEqual(result.profile, "default")

    def test_whitespace_lines_are_ignored(self):
        result = vps.classify([
            "  ",
            "tools/import-design/pulp_import_design.cpp",
            "",
        ])
        self.assertEqual(result.profile, "parser")
        self.assertEqual(result.matched, ("tools/import-design/pulp_import_design.cpp",))


class CliTests(unittest.TestCase):
    def test_paths_from_stdin_plain_output(self):
        stdin = io.StringIO("tools/import-design/foo.cpp\ntest/test_design_import.cpp\n")
        buf = io.StringIO()
        old_stdin, sys.stdin = sys.stdin, stdin
        try:
            with redirect_stdout(buf):
                rc = vps.main(["--paths-from", "-"])
        finally:
            sys.stdin = old_stdin
        self.assertEqual(rc, 0)
        self.assertEqual(buf.getvalue().strip(), "parser")

    def test_paths_from_stdin_json_output(self):
        stdin = io.StringIO("core/format/src/vst3_adapter.cpp\n")
        buf = io.StringIO()
        old_stdin, sys.stdin = sys.stdin, stdin
        try:
            with redirect_stdout(buf):
                rc = vps.main(["--paths-from", "-", "--json"])
        finally:
            sys.stdin = old_stdin
        self.assertEqual(rc, 0)
        payload = json.loads(buf.getvalue())
        self.assertEqual(payload["profile"], "default")
        self.assertEqual(payload["unmatched"], ["core/format/src/vst3_adapter.cpp"])
        self.assertEqual(payload["matched"], [])

    def test_paths_from_file_ignores_blank_lines(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "paths.txt"
            path.write_text(
                "\n tools/import-design/foo.cpp \n\n"
                "test/test_design_import.cpp\n",
                encoding="utf-8",
            )

            self.assertEqual(
                vps.paths_from_file(str(path)),
                [" tools/import-design/foo.cpp ", "test/test_design_import.cpp"],
            )

    def test_mixed_paths_json_reports_both_sides(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "paths.txt"
            path.write_text(
                "tools/import-design/foo.cpp\n"
                "core/audio/src/buffer.cpp\n",
                encoding="utf-8",
            )
            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = vps.main(["--paths-from", str(path), "--json"])

        self.assertEqual(rc, 0)
        payload = json.loads(buf.getvalue())
        self.assertEqual(payload["profile"], "default")
        self.assertEqual(payload["matched"], ["tools/import-design/foo.cpp"])
        self.assertEqual(payload["unmatched"], ["core/audio/src/buffer.cpp"])

    def test_git_query_failure_returns_two(self):
        with mock.patch.object(
            vps,
            "changed_paths_from_git",
            side_effect=subprocess.CalledProcessError(128, ["git"], stderr="bad base"),
        ):
            rc = vps.main(["--base", "missing"])
        self.assertEqual(rc, 2)

    def test_changed_paths_from_git_filters_blank_lines(self):
        with mock.patch.object(
            vps,
            "_run_git",
            return_value="tools/import-design/foo.cpp\n\ncore/audio/src/buffer.cpp\n",
        ) as run:
            self.assertEqual(
                vps.changed_paths_from_git("origin/main"),
                ["tools/import-design/foo.cpp", "core/audio/src/buffer.cpp"],
            )
        run.assert_called_once_with(["diff", "--name-only", "origin/main...HEAD"])

    def test_run_git_delegates_to_subprocess(self):
        completed = subprocess.CompletedProcess(
            ["git"], 0, stdout="a\nb\n", stderr="",
        )
        with mock.patch.object(vps.subprocess, "run", return_value=completed) as run:
            self.assertEqual(vps._run_git(["status", "--short"]), "a\nb\n")
        run.assert_called_once_with(
            ["git", "status", "--short"],
            check=True,
            capture_output=True,
            text=True,
        )

    def test_default_git_path_plain_output(self):
        with mock.patch.object(
            vps,
            "changed_paths_from_git",
            return_value=["tools/import-design/foo.cpp"],
        ) as changed:
            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = vps.main(["--base", "upstream/main"])
        self.assertEqual(rc, 0)
        self.assertEqual(buf.getvalue().strip(), "parser")
        changed.assert_called_once_with("upstream/main")

    def test_paths_from_missing_file_returns_two(self):
        stderr = io.StringIO()
        with redirect_stdout(io.StringIO()):
            with mock.patch.object(sys, "stderr", stderr):
                rc = vps.main(["--paths-from", "/definitely/missing/paths.txt"])
        self.assertEqual(rc, 2)
        self.assertIn("validation_profile_select:", stderr.getvalue())

    def test_script_entrypoint_exits_with_main_status(self):
        stdout = io.StringIO()
        with mock.patch.object(sys, "argv", [str(Path(__file__).parent / "validation_profile_select.py"),
                                             "--paths-from", "-", "--json"]), \
             mock.patch.object(sys, "stdin", io.StringIO("tools/import-design/foo.cpp\n")), \
             redirect_stdout(stdout):
            with self.assertRaises(SystemExit) as cm:
                runpy.run_path(str(Path(__file__).parent / "validation_profile_select.py"),
                               run_name="__main__")
        self.assertEqual(cm.exception.code, 0)
        self.assertEqual(json.loads(stdout.getvalue())["profile"], "parser")


if __name__ == "__main__":
    unittest.main()
