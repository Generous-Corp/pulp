#!/usr/bin/env python3
"""Fixture tests for tools/scripts/build_migration_index.py.

Regression coverage for the Codex post-merge sweep wave 3 findings on
PR #571 (release-discovery Slice 3):

- P2: `bool("false")` is True — if a schema-incorrect `breaking = "false"`
  (quoted string) slipped through, the codegen silently emitted a
  `breaking = true` row. The generator must reject non-boolean values
  for a boolean field rather than coercing.
- P2: `version = "abc"` or `version = "0.29"` was silently keyed as
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
        # Codex P2: bool("false") is True.
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
        # Codex P2: silent (0,0,0) sort + runtime hop-filter drops entry.
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


if __name__ == "__main__":
    unittest.main()
