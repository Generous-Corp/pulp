#!/usr/bin/env python3
"""Tests for tools/scripts/skills_doc_check.py.

Locks down the stdlib frontmatter parser (inline, quoted, and block-scalar
descriptions), the one-line summary extraction, the render, and the two quality
gates — plus a smoke test that the real skill catalog is clean.

Run:
    python3 tools/scripts/test_skills_doc_check.py
"""
from __future__ import annotations

import importlib.util
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools/scripts/skills_doc_check.py"


def _load():
    spec = importlib.util.spec_from_file_location("skills_doc_check", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["skills_doc_check"] = mod
    spec.loader.exec_module(mod)
    return mod


MOD = _load()


class Frontmatter(unittest.TestCase):
    def test_inline_field(self):
        fm = MOD._frontmatter_lines("---\nname: ci\ndescription: does CI things\n---\nbody\n")
        self.assertEqual(MOD._field(fm, "name"), "ci")
        self.assertEqual(MOD._field(fm, "description"), "does CI things")

    def test_quoted_inline_field(self):
        fm = MOD._frontmatter_lines('---\ndescription: "quoted value"\n---\n')
        self.assertEqual(MOD._field(fm, "description"), "quoted value")

    def test_block_scalar_field(self):
        text = ("---\n"
                "name: update-demos\n"
                "description: |\n"
                "  First line of the block.\n"
                "  Second line continues it.\n"
                "requires:\n"
                "  tools:\n"
                "    - pulp\n"
                "---\n")
        fm = MOD._frontmatter_lines(text)
        desc = MOD._field(fm, "description")
        self.assertIn("First line of the block.", desc)
        self.assertIn("Second line continues it.", desc)
        # The block must stop at the next top-level key, not swallow `requires:`.
        self.assertNotIn("requires", desc)
        self.assertNotIn("pulp", desc)

    def test_missing_field_is_none(self):
        fm = MOD._frontmatter_lines("---\nname: x\n---\n")
        self.assertIsNone(MOD._field(fm, "description"))

    def test_no_frontmatter(self):
        self.assertEqual(MOD._frontmatter_lines("# just a heading\n"), [])


class Summary(unittest.TestCase):
    def test_first_sentence_only(self):
        s = MOD._summary("Do the thing well. Also handles 'trigger a', 'trigger b'.")
        self.assertEqual(s, "Do the thing well.")

    def test_no_period_returns_whole_collapsed(self):
        s = MOD._summary("CLAP adapter:\n  routing,\n  midi")
        self.assertEqual(s, "CLAP adapter: routing, midi")

    def test_pipe_is_escaped_for_table_cell(self):
        s = MOD._summary("routing A | B | C")
        self.assertNotIn(" | ", s)
        self.assertIn("\\|", s)


class QualityGates(unittest.TestCase):
    def _skill(self, base: pathlib.Path, name: str, frontmatter: str):
        d = base / name
        d.mkdir(parents=True)
        (d / "SKILL.md").write_text(frontmatter)

    def test_short_description_is_flagged(self):
        base = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(base, ignore_errors=True))
        self._skill(base, "thin", "---\nname: thin\ndescription: tiny\n---\n")
        with mock.patch.object(MOD, "SKILLS_DIR", base):
            _, problems = MOD.load_skills()
        self.assertTrue(any("too short" in p for p in problems))

    def test_name_mismatch_is_flagged(self):
        base = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(base, ignore_errors=True))
        self._skill(base, "realdir",
                    "---\nname: wrongname\ndescription: "
                    "a sufficiently long description here\n---\n")
        with mock.patch.object(MOD, "SKILLS_DIR", base):
            _, problems = MOD.load_skills()
        self.assertTrue(any("!= directory" in p for p in problems))

    def test_clean_skill_has_no_problems(self):
        base = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(base, ignore_errors=True))
        self._skill(base, "good",
                    "---\nname: good\ndescription: "
                    "a nicely explained skill that does a clear thing\n---\n")
        with mock.patch.object(MOD, "SKILLS_DIR", base):
            skills, problems = MOD.load_skills()
        self.assertEqual(problems, [])
        self.assertEqual(skills[0]["name"], "good")


class RenderAndRealCatalog(unittest.TestCase):
    def test_render_is_deterministic_and_tabular(self):
        skills = [{"name": "b-skill", "summary": "does B"},
                  {"name": "a-skill", "summary": "does A"}]
        out = MOD.render(sorted(skills, key=lambda s: s["name"]))
        self.assertEqual(out, MOD.render(sorted(skills, key=lambda s: s["name"])))
        # a-skill sorts before b-skill in the rendered table.
        self.assertLess(out.index("`a-skill`"), out.index("`b-skill`"))
        self.assertIn("| Skill | What it does |", out)

    def test_real_catalog_is_clean(self):
        # Every shipped skill must parse with a real name + description.
        skills, problems = MOD.load_skills()
        self.assertEqual(problems, [], f"skill catalog problems: {problems}")
        self.assertGreater(len(skills), 1)


if __name__ == "__main__":
    unittest.main()
