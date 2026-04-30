#!/usr/bin/env python3
"""Extra focused tests for tools/scripts/build_migration_index.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import runpy
import sys
import tempfile
import unittest
from unittest import mock

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SCRIPT = REPO_ROOT / "tools" / "scripts" / "build_migration_index.py"

_SPEC = importlib.util.spec_from_file_location("bmi_extra", SCRIPT)
assert _SPEC and _SPEC.loader
_bmi = importlib.util.module_from_spec(_SPEC)
sys.modules["bmi_extra"] = _bmi
_SPEC.loader.exec_module(_bmi)


def _write_doc(
    docs_dir: pathlib.Path,
    name: str,
    frontmatter: str,
    body: str = "body.\n",
) -> pathlib.Path:
    path = docs_dir / name
    path.write_text(f"---\n{frontmatter}\n---\n{body}", encoding="utf-8")
    return path


class FrontmatterParsingExtra(unittest.TestCase):
    def _capture_system_exit(self, fn, *args):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            with self.assertRaises(SystemExit) as ctx:
                fn(*args)
        return ctx.exception, stderr.getvalue()

    def test_comments_trailing_comments_false_and_string_unescapes(self):
        with tempfile.TemporaryDirectory() as td:
            docs = pathlib.Path(td)
            doc = _write_doc(
                docs,
                "v0.31.0.md",
                '# ignored comment\n'
                '\n'
                'version = "0.31.0" # release line\n'
                'breaking = false # must stay boolean false\n'
                'applies_if = "line\\ncol\\ttab\\rcarriage\\\\slash\\"quote\\q"\n'
                'summary = "hash # remains inside quotes" # trailing comment\n'
                'extra_key = "extra value"\n',
                "\n## Details\nBody.\n",
            )

            entry = _bmi.load_entry(doc)

        self.assertEqual(entry.version, "0.31.0")
        self.assertIs(entry.breaking, False)
        self.assertEqual(
            entry.applies_if,
            'line\ncol\ttab\rcarriage\\slash"quote\\q',
        )
        self.assertEqual(entry.summary, "hash # remains inside quotes")
        self.assertEqual(entry.extra, {"extra_key": "extra value"})
        self.assertEqual(entry.body, "## Details\nBody.\n")

    def test_frontmatter_parser_rejects_bad_line_shapes(self):
        source = pathlib.Path("bad.md")
        cases = [
            ("version \"0.31.0\"", "expected `key = value`"),
            ('summary = "unterminated', "unterminated string"),
            ('summary = "ok" trailing', "trailing content after string"),
            ("version = 0.31.0", "unsupported value shape"),
        ]

        for text, expected in cases:
            with self.subTest(text=text):
                exc, stderr = self._capture_system_exit(
                    _bmi._parse_toml_frontmatter,
                    text,
                    source,
                )
                self.assertEqual(exc.code, 2)
                self.assertIn(expected, stderr)

    def test_missing_and_invalid_frontmatter_edges_fail_fast(self):
        cases = [
            ("plain.md", "not frontmatter\n", "missing or malformed TOML frontmatter"),
            ("missing-version.md", "---\nsummary = \"s\"\n---\nbody\n", "missing required `version`"),
            ("bool-version.md", "---\nversion = true\n---\nbody\n", "`version` must be a string"),
            (
                "bad-applies.md",
                "---\nversion = \"0.31.0\"\napplies_if = true\n---\nbody\n",
                "`applies_if` must be a double-quoted string",
            ),
            (
                "bad-summary.md",
                "---\nversion = \"0.31.0\"\nsummary = false\n---\nbody\n",
                "`summary` must be a double-quoted string",
            ),
        ]

        for name, contents, expected in cases:
            with self.subTest(name=name):
                with tempfile.TemporaryDirectory() as td:
                    path = pathlib.Path(td) / name
                    path.write_text(contents, encoding="utf-8")
                    exc, stderr = self._capture_system_exit(_bmi.load_entry, path)
                self.assertEqual(exc.code, 2)
                self.assertIn(expected, stderr)


class CodegenAndMainExtra(unittest.TestCase):
    def _capture_stderr(self, fn, *args):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            result = fn(*args)
        return result, stderr.getvalue()

    def _capture_system_exit(self, fn, *args):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            with self.assertRaises(SystemExit) as ctx:
                fn(*args)
        return ctx.exception, stderr.getvalue()

    def test_cpp_escape_covers_special_and_control_characters(self):
        raw = 'slash\\quote"\n\r\t' + chr(1) + "done"
        self.assertEqual(
            _bmi._cpp_escape(raw),
            'slash\\\\quote\\"\\n\\r\\t\\x01done',
        )

    def test_emit_cpp_empty_entries_uses_null_index(self):
        generated = _bmi.emit_cpp([])
        self.assertIn("// No migration docs found under docs/migrations/.", generated)
        self.assertIn('{"", false, "", "", ""}', generated)
        self.assertIn("const MigrationEntry* const kMigrationIndex = nullptr;", generated)
        self.assertIn("const std::size_t kMigrationIndexSize = 0;", generated)

    def test_main_success_sorts_skips_readme_escapes_and_is_idempotent(self):
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            docs = root / "docs"
            docs.mkdir()
            out = root / "generated" / "migration_index.cpp"

            _write_doc(
                docs,
                "z-newer.md",
                'version = "0.32.0"\n'
                'breaking = true\n'
                'summary = "newer summary"',
                'newer body\n',
            )
            _write_doc(
                docs,
                "a-older.md",
                'version = "0.31.0"\n'
                'breaking = false\n'
                'applies_if = "from < 0.31.0"\n'
                'summary = "older \\"summary\\""\n'
                'extra_key = "ignored"',
                'older body with slash \\\\ and tab\\ttext\n',
            )
            (docs / "README.md").write_text(
                "---\nversion = true\n---\nshould be ignored\n",
                encoding="utf-8",
            )

            result = _bmi.main(["--docs-dir", str(docs), "--out", str(out)])
            self.assertEqual(result, 0)
            first = out.read_text(encoding="utf-8")
            self.assertLess(first.index('"0.31.0"'), first.index('"0.32.0"'))
            self.assertIn("extra frontmatter keys ignored: ['extra_key']", first)
            self.assertIn('\\"summary\\"', first)
            self.assertIn("older body with slash \\\\\\\\ and tab\\\\ttext", first)
            self.assertNotIn("should be ignored", first)

            result = _bmi.main(["--docs-dir", str(docs), "--out", str(out)])
            second = out.read_text(encoding="utf-8")

        self.assertEqual(result, 0)
        self.assertEqual(first, second)

    def test_main_missing_docs_dir_reports_error(self):
        with tempfile.TemporaryDirectory() as td:
            missing = pathlib.Path(td) / "missing"
            out = pathlib.Path(td) / "out.cpp"
            result, stderr = self._capture_stderr(
                _bmi.main,
                ["--docs-dir", str(missing), "--out", str(out)],
            )

        self.assertEqual(result, 2)
        self.assertIn("docs dir not found", stderr)

    def test_main_duplicate_versions_fail_before_writing(self):
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            docs = root / "docs"
            docs.mkdir()
            out = root / "out.cpp"
            _write_doc(docs, "one.md", 'version = "0.31.0"')
            _write_doc(docs, "two.md", 'version = "0.31.0"')

            exc, stderr = self._capture_system_exit(
                _bmi.main,
                ["--docs-dir", str(docs), "--out", str(out)],
            )

        self.assertEqual(exc.code, 2)
        self.assertIn("duplicate `version = '0.31.0'`", stderr)
        self.assertFalse(out.exists())

    def test_semver_key_invalid_versions_sort_to_zero_tuple(self):
        self.assertEqual(_bmi.semver_key("not-semver"), (0, 0, 0))

    def test_script_entrypoint_exits_with_main_result(self):
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            docs = root / "docs"
            docs.mkdir()
            out = root / "out.cpp"
            _write_doc(docs, "v0.31.0.md", 'version = "0.31.0"')

            with mock.patch.object(
                sys,
                "argv",
                [str(SCRIPT), "--docs-dir", str(docs), "--out", str(out)],
            ):
                with self.assertRaises(SystemExit) as ctx:
                    runpy.run_path(str(SCRIPT), run_name="__main__")

            self.assertEqual(ctx.exception.code, 0)
            self.assertTrue(out.exists())


if __name__ == "__main__":
    unittest.main()
