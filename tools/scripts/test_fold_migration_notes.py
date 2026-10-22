#!/usr/bin/env python3
"""Tests for unreleased migration notes and fold_migration_notes.py.

An unreleased note (docs/migrations/unreleased/<topic>.md) carries no version;
the bump commit folds it into v<assigned>.md. These cover the note's own
validation, the fold's output (which must pass the same checks
build_migration_index.py applies to every released note), and that a failing
fold leaves the tree untouched.

Run:
    python3 tools/scripts/test_fold_migration_notes.py
"""

from __future__ import annotations

import contextlib
import io
import pathlib
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import build_migration_index as bmi  # noqa: E402
import fold_migration_notes as fmn  # noqa: E402

REPO_ROOT = HERE.parent.parent


def _write(path: pathlib.Path, frontmatter: str, body: str = "## Migration steps\n- do it\n") -> pathlib.Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f"---\n{frontmatter}\n---\n\n{body}", encoding="utf-8")
    return path


class _RepoCase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = pathlib.Path(self._tmp.name)
        self.unreleased = self.repo / fmn.UNRELEASED_DIR
        self.unreleased.mkdir(parents=True)
        (self.unreleased / "README.md").write_text("how to write a note\n", encoding="utf-8")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def note(self, name: str, frontmatter: str, body: str = "## Migration steps\n- do it\n") -> pathlib.Path:
        return _write(self.unreleased / name, frontmatter, body)

    def fails(self, fn, *args) -> str:
        err = io.StringIO()
        with contextlib.redirect_stderr(err), self.assertRaises(SystemExit):
            fn(*args)
        return err.getvalue()


class UnreleasedNoteTests(_RepoCase):
    def test_a_note_with_breaking_and_summary_loads(self) -> None:
        self.note("a.md", 'breaking = true\nsummary = "Knob API renamed"')
        notes = bmi.load_unreleased_dir(self.unreleased)
        self.assertEqual([(n.breaking, n.summary) for n in notes], [(True, "Knob API renamed")])

    def test_version_and_applies_if_are_refused(self) -> None:
        for field in ('version = "0.1.0"',
                      'applies_if = "cli_version_from < 0.1.0 && cli_version_to >= 0.1.0"'):
            with self.subTest(field=field):
                path = self.note("a.md", f'summary = "s"\n{field}')
                self.assertIn("must not set", self.fails(bmi.load_unreleased, path))

    def test_summary_body_and_field_shape_are_required(self) -> None:
        cases = {
            "no summary": ("breaking = true", "## Steps\n- x\n", "`summary` is required"),
            "empty body": ('summary = "s"', "\n", "no body"),
            "string breaking": ('summary = "s"\nbreaking = "false"', "x\n", "must be a boolean"),
            "unknown field": ('summary = "s"\nowner = "me"', "x\n", "unsupported frontmatter"),
        }
        for label, (frontmatter, body, message) in cases.items():
            with self.subTest(label):
                path = self.note("a.md", frontmatter, body)
                self.assertIn(message, self.fails(bmi.load_unreleased, path))

    def test_the_index_generator_fails_on_a_bad_unreleased_note(self) -> None:
        docs = self.repo / fmn.MIGRATIONS_DIR
        self.note("bad.md", 'version = "0.1.0"\nsummary = "s"')
        out = self.repo / "migration_index.cpp"
        self.fails(bmi.main, ["--docs-dir", str(docs), "--out", str(out)])

    def test_the_index_generator_does_not_emit_unreleased_notes(self) -> None:
        docs = self.repo / fmn.MIGRATIONS_DIR
        self.note("a.md", 'summary = "UNRELEASED-MARKER"')
        out = self.repo / "migration_index.cpp"
        self.assertEqual(bmi.main(["--docs-dir", str(docs), "--out", str(out)]), 0)
        self.assertNotIn("UNRELEASED-MARKER", out.read_text(encoding="utf-8"))

    def test_the_repository_unreleased_notes_are_valid(self) -> None:
        directory = REPO_ROOT / fmn.UNRELEASED_DIR
        self.assertTrue((directory / "README.md").is_file(),
                        "the unreleased directory and its README must exist")
        bmi.load_unreleased_dir(directory)


class FoldTests(_RepoCase):
    def released(self, version: str) -> bmi.Entry:
        entry = bmi.load_entry(self.repo / fmn.MIGRATIONS_DIR / f"v{version}.md")
        bmi.check_entry_consistency(entry)
        return entry

    def test_notes_fold_into_a_new_release_note(self) -> None:
        self.note("b-saturator.md", 'breaking = true\nsummary = "Saturator is minimax"', "B body\n")
        self.note("a-compressor.md", 'summary = "Compressor is opt-in"', "A body\n")
        changed = fmn.fold(self.repo, "0.1006.0")
        self.assertEqual(changed, ["docs/migrations/v0.1006.0.md",
                                   "docs/migrations/unreleased/a-compressor.md",
                                   "docs/migrations/unreleased/b-saturator.md"])
        entry = self.released("0.1006.0")
        self.assertTrue(entry.breaking)
        self.assertEqual(entry.applies_if,
                         "cli_version_from < 0.1006.0 && cli_version_to >= 0.1006.0")
        self.assertEqual(entry.summary, "Compressor is opt-in; Saturator is minimax")
        self.assertLess(entry.body.index("A body"), entry.body.index("B body"))
        self.assertEqual(sorted(p.name for p in self.unreleased.iterdir()), ["README.md"])

    def test_notes_append_to_an_existing_release_note(self) -> None:
        _write(self.repo / fmn.MIGRATIONS_DIR / "v0.1006.0.md",
               'version = "0.1006.0"\nbreaking = false\n'
               'applies_if = "cli_version_from < 0.1006.0 && cli_version_to >= 0.1006.0"\n'
               'summary = "Hand-filed"', "OLD body\n")
        self.note("a.md", 'breaking = true\nsummary = "Folded"', "NEW body\n")
        fmn.fold(self.repo, "0.1006.0")
        entry = self.released("0.1006.0")
        self.assertTrue(entry.breaking)
        self.assertEqual(entry.summary, "Hand-filed; Folded")
        self.assertLess(entry.body.index("OLD body"), entry.body.index("NEW body"))

    def test_quotes_in_a_summary_survive_the_round_trip(self) -> None:
        self.note("a.md", 'summary = "Use \\"strict\\" mode"')
        fmn.fold(self.repo, "1.2.3")
        self.assertEqual(self.released("1.2.3").summary, 'Use "strict" mode')

    def test_nothing_to_fold_writes_nothing(self) -> None:
        self.assertEqual(fmn.fold(self.repo, "1.2.3"), [])
        self.assertFalse((self.repo / fmn.MIGRATIONS_DIR / "v1.2.3.md").exists())

    def test_a_bad_note_leaves_the_tree_untouched(self) -> None:
        self.note("a.md", 'summary = "fine"')
        bad = self.note("b.md", 'version = "9.9.9"\nsummary = "bad"')
        self.fails(fmn.fold, self.repo, "1.2.3")
        self.assertFalse((self.repo / fmn.MIGRATIONS_DIR / "v1.2.3.md").exists())
        self.assertTrue(bad.exists())
        self.assertTrue((self.unreleased / "a.md").exists())

    def test_a_non_release_version_is_refused(self) -> None:
        self.note("a.md", 'summary = "s"')
        with self.assertRaises(ValueError):
            fmn.fold(self.repo, "next")
        self.assertTrue((self.unreleased / "a.md").exists())

    def test_the_folded_note_reaches_the_generated_index(self) -> None:
        self.note("a.md", 'breaking = true\nsummary = "FOLDED-MARKER"')
        fmn.fold(self.repo, "2.0.0")
        out = self.repo / "migration_index.cpp"
        docs = self.repo / fmn.MIGRATIONS_DIR
        self.assertEqual(bmi.main(["--docs-dir", str(docs), "--out", str(out)]), 0)
        generated = out.read_text(encoding="utf-8")
        self.assertIn("FOLDED-MARKER", generated)
        self.assertIn('"2.0.0"', generated)


if __name__ == "__main__":
    unittest.main()
