#!/usr/bin/env python3
"""Fixture tests for tools/scripts/build_migration_index.py.

Regression coverage for migration-index validation:

- `bool("false")` is True — if a schema-incorrect `breaking = "false"`
  (quoted string) slipped through, the codegen silently emitted a
  `breaking = true` row. The generator must reject non-boolean values
  for a boolean field rather than coercing.
- `version = "abc"` or `version = "0.29"` was silently keyed as
  (0,0,0) in the sort, and the runtime hop-filter — which parses semver
  from the same string at load time — dropped the entry completely.
  Users would never see the note. Reject non-semver versions at
  codegen time instead.

Run:
    python3 tools/scripts/test_build_migration_index.py
"""

from __future__ import annotations

import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SCRIPT = REPO_ROOT / "tools" / "scripts" / "build_migration_index.py"

_SPEC = importlib.util.spec_from_file_location("bmi", SCRIPT)
assert _SPEC and _SPEC.loader
_bmi = importlib.util.module_from_spec(_SPEC)
sys.modules["bmi"] = _bmi
_SPEC.loader.exec_module(_bmi)


def _write_doc(tmp: pathlib.Path, name: str, frontmatter: str, body: str = "body.\n") -> pathlib.Path:
    path = tmp / name
    path.write_text(f"---\n{frontmatter}\n---\n{body}", encoding="utf-8")
    return path


class LoadEntryValidation(unittest.TestCase):
    """Direct unit tests for `load_entry()` type + semver validation."""

    def test_valid_frontmatter_loads(self):
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            doc = _write_doc(
                tmp,
                "v0.27.0.md",
                'version = "0.27.0"\n'
                'breaking = true\n'
                'applies_if = "x"\n'
                'summary = "s"',
            )
            e = _bmi.load_entry(doc)
            self.assertEqual(e.version, "0.27.0")
            self.assertIs(e.breaking, True)
            self.assertEqual(e.applies_if, "x")
            self.assertEqual(e.summary, "s")

    def test_string_value_rejected_for_boolean_field(self):
        # Regression guard: bool("false") is True.
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            doc = _write_doc(
                tmp,
                "v0.27.0.md",
                'version = "0.27.0"\n'
                'breaking = "false"',
            )
            with self.assertRaises(SystemExit) as ctx:
                _bmi.load_entry(doc)
            self.assertEqual(ctx.exception.code, 2)

    def test_non_semver_version_rejected(self):
        # Regression guard: silent (0,0,0) sort + runtime hop-filter drops entry.
        # SEMVER_RE deliberately allows prerelease/build suffixes like
        # "0.27.0-rc.1" or "0.27.0.beta" — those are rejected as
        # "not semver" in the stricter sense, but the generator has
        # historically accepted the (\d+)\.(\d+)\.(\d+)(?:[.\-+].*)? shape.
        # Keep the reject list limited to inputs the major.minor.patch
        # regex truly cannot parse.
        bad_versions = ['"abc"', '"0.29"', '"1"', '""']
        for v in bad_versions:
            with self.subTest(version=v):
                with tempfile.TemporaryDirectory() as td:
                    tmp = pathlib.Path(td)
                    doc = _write_doc(tmp, "bad.md", f"version = {v}")
                    with self.assertRaises(SystemExit) as ctx:
                        _bmi.load_entry(doc)
                    self.assertEqual(ctx.exception.code, 2)

    def test_version_v_prefix_allowed(self):
        # `v0.30.0` is a common convention; SEMVER_RE already allows it
        # and the hop-filter strips the prefix. Keep it working.
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            doc = _write_doc(tmp, "v0.30.0.md", 'version = "v0.30.0"')
            e = _bmi.load_entry(doc)
            self.assertEqual(e.version, "v0.30.0")

    def test_semver_with_prerelease_allowed(self):
        # `0.27.0-rc.1` is valid semver; the parser must accept it so
        # pre-release migration notes can ship (e.g. during a beta hop).
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            doc = _write_doc(tmp, "rc.md", 'version = "0.27.0-rc.1"')
            e = _bmi.load_entry(doc)
            self.assertEqual(e.version, "0.27.0-rc.1")


class EndToEndCodegen(unittest.TestCase):
    """Shell out to the generator and assert exit code + stderr signals."""

    def _run(self, docs_dir: pathlib.Path, out: pathlib.Path) -> subprocess.CompletedProcess:
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--docs-dir",
                str(docs_dir),
                "--out",
                str(out),
            ],
            capture_output=True,
            text=True,
        )

    def test_invalid_breaking_value_fails_codegen(self):
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            _write_doc(
                tmp,
                "v0.27.0.md",
                'version = "0.27.0"\nbreaking = "true"',
            )
            out = tmp / "out.cpp"
            res = self._run(tmp, out)
            self.assertNotEqual(res.returncode, 0)
            self.assertIn("breaking", res.stderr)
            self.assertFalse(out.exists(), "codegen must not emit on schema error")

    def test_invalid_version_fails_codegen(self):
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            _write_doc(tmp, "v.md", 'version = "abc"')
            out = tmp / "out.cpp"
            res = self._run(tmp, out)
            self.assertNotEqual(res.returncode, 0)
            self.assertIn("semver", res.stderr.lower())
            self.assertFalse(out.exists())


class EntryConsistency(unittest.TestCase):
    """A note must describe the release it is filed under: the file name,
    `version` and `applies_if` must agree, or `pulp upgrade --notes` shows
    it for the wrong hop or never (a renamed v0.945.2.md once kept
    `version = "0.943.1"` and passed every other check)."""

    def _codegen(self, name: str, frontmatter: str) -> subprocess.CompletedProcess:
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            _write_doc(tmp, name, frontmatter)
            return subprocess.run(
                [sys.executable, str(SCRIPT), "--docs-dir", str(tmp), "--out", str(tmp / "out.cpp")],
                capture_output=True, text=True, encoding="utf-8")

    def test_consistent_note_passes(self):
        res = self._codegen("v0.945.2.md", 'version = "0.945.2"\n'
                            'applies_if = "cli_version_from < 0.945.2 && cli_version_to >= 0.945.2"')
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_file_name_and_version_disagree(self):
        res = self._codegen("v0.945.2.md", 'version = "0.943.1"\n'
                            'applies_if = "cli_version_from < 0.943.1 && cli_version_to >= 0.943.1"')
        self.assertEqual(res.returncode, 2)
        self.assertIn("file name says 0.945.2", res.stderr)

    def test_applies_if_selects_a_hop_before_the_version(self):
        res = self._codegen("v0.945.2.md", 'version = "0.945.2"\n'
                            'applies_if = "cli_version_from < 0.943.1 && cli_version_to >= 0.943.1"')
        self.assertEqual(res.returncode, 2)
        self.assertIn("must be exactly", res.stderr)

    def test_applies_if_never_crosses_the_version(self):
        res = self._codegen("v0.945.2.md", 'version = "0.945.2"\napplies_if = "cli_version_to < 0.1.0"')
        self.assertEqual(res.returncode, 2)
        self.assertIn("must be exactly", res.stderr)

    def test_applies_if_that_does_not_parse(self):
        res = self._codegen("v0.945.2.md", 'version = "0.945.2"\napplies_if = "from < 0.945.2"')
        self.assertEqual(res.returncode, 2)
        self.assertIn("must be exactly", res.stderr)

    def test_applies_if_that_also_selects_an_unrelated_hop(self):
        # Crosses 0.945.2 and not its predecessor, so a boundary probe passes
        # it, yet the `||` arm also selects every hop inside 1.0.x.
        res = self._codegen("v0.945.2.md", 'version = "0.945.2"\n'
                            'applies_if = "cli_version_from < 0.945.2 && cli_version_to >= 0.945.2'
                            ' || cli_version_from >= 1.0.0 && cli_version_to < 1.1.0"')
        self.assertEqual(res.returncode, 2)
        self.assertIn("must be exactly", res.stderr)
        self.assertIn("nothing else", res.stderr)

    def test_canonical_applies_if_with_other_spacing_passes(self):
        res = self._codegen("v0.945.2.md", 'version = "0.945.2"\n'
                            'applies_if = "  cli_version_from<0.945.2  &&  cli_version_to>=0.945.2 "')
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_canonical_shape_with_mismatched_bounds_is_rejected(self):
        res = self._codegen("v0.945.2.md", 'version = "0.945.2"\n'
                            'applies_if = "cli_version_from < 0.945.2 && cli_version_to >= 0.946.0"')
        self.assertEqual(res.returncode, 2)
        self.assertIn("must be exactly", res.stderr)

    def test_empty_applies_if_and_free_form_names_are_allowed(self):
        res = self._codegen("notes.md", 'version = "0.31.0"')
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_evaluator_matches_the_documented_grammar(self):
        ev = _bmi.evaluate_applies_if
        expr = "cli_version_from < 0.27.0 && cli_version_to >= 0.27.0"
        self.assertTrue(ev(expr, "0.26.0", "0.27.0"))
        self.assertFalse(ev(expr, "0.27.0", "0.28.0"))
        self.assertTrue(ev("(cli_version_to == v1.0.0) || cli_version_from != 0.1.0", "0.1.0", "1.0.0"))
        with self.assertRaises(_bmi.AppliesIfError):
            ev("cli_version_from <", "0.1.0", "0.2.0")


if __name__ == "__main__":
    unittest.main()
