#!/usr/bin/env python3
"""Unit tests for docs_sync_check.py (#566 Phase 4 / #567).

Pure-Python; no subprocess, no git. Exercises matches_any, trailer
parse, evaluate, and render.
"""

from __future__ import annotations

import unittest

import docs_sync_check as dsc


DOCS = [
    dsc.DocEntry(
        name="coverage.md",
        path="docs/guides/coverage.md",
        paths=("codecov.yml", "scripts/run_coverage.sh"),
    ),
    dsc.DocEntry(
        name="cli.md",
        path="docs/reference/cli.md",
        paths=("tools/cli/cmd_*.cpp",),
    ),
]


class MatchTests(unittest.TestCase):

    def test_literal_match(self) -> None:
        self.assertTrue(dsc.matches_any("codecov.yml", ("codecov.yml",)))

    def test_glob_match(self) -> None:
        self.assertTrue(
            dsc.matches_any("tools/cli/cmd_pr.cpp", ("tools/cli/cmd_*.cpp",))
        )

    def test_prefix_star_star(self) -> None:
        # Accepts `dir/**` as prefix-style.
        self.assertTrue(
            dsc.matches_any("core/audio/src/foo.cpp", ("core/audio/**",))
        )

    def test_no_match(self) -> None:
        self.assertFalse(
            dsc.matches_any("README.md", ("codecov.yml", "scripts/run_coverage.sh"))
        )


class TrailerTests(unittest.TestCase):

    def test_parses_skip_trailer(self) -> None:
        msg = 'fix: something\n\nDocs-Update: skip doc=coverage.md reason="test"\n'
        self.assertEqual(
            dsc.parse_bypass_trailers(msg),
            {"coverage.md": "test"},
        )

    def test_multiple_trailers(self) -> None:
        msg = (
            'fix: two docs\n\n'
            'Docs-Update: skip doc=coverage.md reason="a"\n'
            'Docs-Update: skip doc=cli.md reason="b"\n'
        )
        self.assertEqual(
            dsc.parse_bypass_trailers(msg),
            {"coverage.md": "a", "cli.md": "b"},
        )

    def test_no_trailer(self) -> None:
        self.assertEqual(dsc.parse_bypass_trailers("fix: nothing\n"), {})


class EvaluateTests(unittest.TestCase):

    def test_touched_unupdated_fails(self) -> None:
        diff = ["codecov.yml"]
        findings = dsc.evaluate(DOCS, diff, message="fix: change codecov")
        self.assertEqual(len(findings), 1)
        self.assertFalse(findings[0].doc_modified)
        self.assertIsNone(findings[0].bypass_reason)

    def test_touched_and_updated_passes(self) -> None:
        diff = ["codecov.yml", "docs/guides/coverage.md"]
        findings = dsc.evaluate(DOCS, diff, message="fix: +doc")
        self.assertEqual(len(findings), 1)
        self.assertTrue(findings[0].doc_modified)

    def test_bypass_trailer_passes(self) -> None:
        diff = ["codecov.yml"]
        msg = 'fix\n\nDocs-Update: skip doc=coverage.md reason="x"\n'
        findings = dsc.evaluate(DOCS, diff, message=msg)
        self.assertEqual(findings[0].bypass_reason, "x")

    def test_untouched_doc_is_absent(self) -> None:
        # Touching only non-mapped path: no findings at all.
        diff = ["README.md"]
        findings = dsc.evaluate(DOCS, diff, message="fix: readme")
        self.assertEqual(findings, [])


class RenderTests(unittest.TestCase):

    def test_render_pass_when_updated(self) -> None:
        findings = [dsc.Finding(
            doc=DOCS[0],
            touched_paths=["codecov.yml"],
            doc_modified=True,
            bypass_reason=None,
        )]
        report, ok = dsc.render(findings)
        self.assertTrue(ok)
        self.assertIn("✓ updated", report)

    def test_render_fail_when_missing(self) -> None:
        findings = [dsc.Finding(
            doc=DOCS[0],
            touched_paths=["codecov.yml"],
            doc_modified=False,
            bypass_reason=None,
        )]
        report, ok = dsc.render(findings)
        self.assertFalse(ok)
        self.assertIn("NOT updated", report)

    def test_render_pass_when_bypassed(self) -> None:
        findings = [dsc.Finding(
            doc=DOCS[0],
            touched_paths=["codecov.yml"],
            doc_modified=False,
            bypass_reason="tier tweak only",
        )]
        report, ok = dsc.render(findings)
        self.assertTrue(ok)
        self.assertIn("bypassed", report)


if __name__ == "__main__":
    unittest.main()
