#!/usr/bin/env python3
"""Tests for tools/deps/audit.py.

These tests exercise the markdown/JSON parsers directly and run the
strict audit over the real repo inventory. Run with:

    python3 -m pytest tools/deps/test_audit.py -v

or as a bare script:

    python3 tools/deps/test_audit.py
"""

from __future__ import annotations

import json
import subprocess
import sys
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
AUDIT = ROOT / "tools" / "deps" / "audit.py"

sys.path.insert(0, str(ROOT / "tools" / "deps"))
import audit  # noqa: E402  (path-injected import)


class ParserTests(unittest.TestCase):
    """The three markdown parsers extract the right names from table rows
    and ## headers. These are regression tests for the attribution audit
    added on 2026-04-21 after the docs/reference/licensing.md drift (7
    bundled deps were silently missing from the public licensing table)."""

    def test_licensing_md_extracts_bolded_first_column(self) -> None:
        sample = textwrap.dedent(
            """\
            | Name | License | Purpose |
            |------|---------|---------|
            | **Highway** | Apache-2.0 | SIMD |
            | **pugixml** | MIT | XML |
            | non-bold | ??? | should be skipped |
            """
        )
        tmp = ROOT / "tools" / "deps" / "_test_licensing.md"
        tmp.write_text(sample)
        try:
            original = audit.LICENSING_MD
            audit.LICENSING_MD = tmp
            names = audit.parse_licensing_md()
        finally:
            audit.LICENSING_MD = original
            tmp.unlink()
        self.assertIn("Highway", names)
        self.assertIn("pugixml", names)
        self.assertNotIn("non-bold", names)

    def test_notice_md_extracts_h2_headings(self) -> None:
        sample = "## foo\n\nbody\n\n## bar baz\n\nbody\n"
        tmp = ROOT / "tools" / "deps" / "_test_notice.md"
        tmp.write_text(sample)
        try:
            original = audit.NOTICE_MD
            audit.NOTICE_MD = tmp
            names = audit.parse_notice_md()
        finally:
            audit.NOTICE_MD = original
            tmp.unlink()
        self.assertEqual(names, {"foo", "bar baz"})

    def test_manifest_json_is_valid(self) -> None:
        manifest = json.loads((ROOT / "tools" / "deps" / "manifest.json").read_text())
        names = [d["name"] for d in manifest["dependencies"]]
        self.assertGreater(len(names), 0)
        # Every manifest entry must declare doc coverage flags + source_files.
        for dep in manifest["dependencies"]:
            self.assertIn("documented_in_dependencies_md", dep, dep["name"])
            self.assertIn("documented_in_notice_md", dep, dep["name"])
            self.assertIn("source_files", dep, dep["name"])

    def test_manifest_is_alphabetical(self) -> None:
        manifest = json.loads((ROOT / "tools" / "deps" / "manifest.json").read_text())
        names = [d["name"] for d in manifest["dependencies"]]
        self.assertEqual(
            names,
            sorted(names, key=str.casefold),
            "manifest.json entries must be alphabetical (case-insensitive)",
        )


class ManifestSourceScannerTests(unittest.TestCase):
    """Completeness gate (added 2026-04-22 under #582 follow-up).

    The audit now scans real dependency manifests — ``requirements-docs.txt``,
    ``mkdocs.yml``, CMake ``FetchContent_Declare`` blocks, and ``external/``
    subdirectories — and flags anything declared there that isn't
    represented by a manifest.json entry.

    This class of check was missing before, which is how the MkDocs
    Material docs lane (#582) landed without updating any of the four
    attribution files.
    """

    def test_requirements_docs_parser_extracts_packages(self) -> None:
        sample = textwrap.dedent(
            """\
            # a comment
            mkdocs-material>=9.5,<10
            some-pkg==1.0
              spaced-pkg  # trailing comment
            """
        )
        tmp = ROOT / "tools" / "deps" / "_test_requirements.txt"
        tmp.write_text(sample)
        try:
            original = audit.REQUIREMENTS_DOCS
            audit.REQUIREMENTS_DOCS = tmp
            declared = audit.parse_requirements_docs()
        finally:
            audit.REQUIREMENTS_DOCS = original
            tmp.unlink()
        names = {d.name for d in declared}
        self.assertIn("mkdocs-material", names)
        self.assertIn("some-pkg", names)
        self.assertIn("spaced-pkg", names)

    def test_mkdocs_yml_parser_extracts_theme_and_plugins(self) -> None:
        sample = textwrap.dedent(
            """\
            site_name: Demo
            theme:
              name: material
              features:
                - navigation.instant
            plugins:
              - search
              - awesome-pages
              - git-revision-date-localized:
                  type: iso_date
            markdown_extensions:
              - admonition
              - pymdownx.details
              - pymdownx.superfences
            """
        )
        tmp = ROOT / "tools" / "deps" / "_test_mkdocs.yml"
        tmp.write_text(sample)
        try:
            original = audit.MKDOCS_YML
            audit.MKDOCS_YML = tmp
            declared = audit.parse_mkdocs_yml()
        finally:
            audit.MKDOCS_YML = original
            tmp.unlink()
        names = {d.name for d in declared}
        self.assertIn("material", names)
        self.assertIn("awesome-pages", names)
        self.assertIn("git-revision-date-localized", names)
        self.assertIn("pymdown-extensions", names)

    def test_fetchcontent_parser_extracts_target_names(self) -> None:
        sample = textwrap.dedent(
            """\
            include(FetchContent)
            FetchContent_Declare(
                choc
                GIT_REPOSITORY https://example.com/choc.git
            )
            FetchContent_Declare( webgpu
                GIT_REPOSITORY https://example.com/webgpu.git
            )
            """
        )
        tmp = ROOT / "tools" / "deps" / "_test_cmake.txt"
        tmp.write_text(sample)
        try:
            declared = audit.parse_fetchcontent(tmp)
        finally:
            tmp.unlink()
        names = {d.name for d in declared}
        self.assertEqual(names, {"choc", "webgpu"})

    def test_uncovered_detection_catches_missing_pip_dep(self) -> None:
        """The key regression test — reproduces the class of miss that
        #582 shipped. A synthetic ``requirements-docs.txt`` declares a
        package that has no manifest entry; the audit must flag it.
        """
        synthetic_requirements = textwrap.dedent(
            """\
            # Synthetic fixture — stuff-that-does-not-exist is the bug
            # we're testing. If the completeness gate regresses, this
            # test will silently pass and we'll be back to the #582 state.
            stuff-that-does-not-exist>=1.0,<2
            mkdocs-material>=9.5,<10
            """
        )
        synthetic_mkdocs = "site_name: Demo\n"
        synthetic_cmake = "# empty\n"

        tmp_req = ROOT / "tools" / "deps" / "_test_req_missing.txt"
        tmp_mk = ROOT / "tools" / "deps" / "_test_mk_missing.yml"
        tmp_cm = ROOT / "tools" / "deps" / "_test_cm_missing.txt"
        tmp_req.write_text(synthetic_requirements)
        tmp_mk.write_text(synthetic_mkdocs)
        tmp_cm.write_text(synthetic_cmake)

        # Load the REAL manifest to compare against — we want to verify
        # the synthetic bogus pip package isn't accidentally covered by
        # some alias elsewhere.
        manifest = audit.load_manifest()

        try:
            declared = audit.collect_declared(
                extra_requirements=tmp_req,
                extra_mkdocs=tmp_mk,
                extra_cmake=[tmp_cm],
            )
        finally:
            tmp_req.unlink()
            tmp_mk.unlink()
            tmp_cm.unlink()

        uncovered = audit.find_uncovered_declarations(manifest, declared)
        uncovered_names = {d.name for d in uncovered}
        self.assertIn(
            "stuff-that-does-not-exist",
            uncovered_names,
            msg="completeness gate must flag pip packages with no manifest entry",
        )
        # Real package that IS in manifest.json should NOT be flagged.
        self.assertNotIn("mkdocs-material", uncovered_names)

    def test_audit_strict_fails_on_synthetic_missing_dep(self) -> None:
        """End-to-end: the ``--strict`` exit status must be non-zero when
        a manifest source declares a dep that ``manifest.json`` doesn't
        cover. Shells out to the real audit binary so we catch wiring
        regressions between ``collect_declared`` and ``main``."""
        synthetic = ROOT / "tools" / "deps" / "_test_req_e2e.txt"
        synthetic.write_text("completely-bogus-attribution-miss==0.0.1\n")
        harness = ROOT / "tools" / "deps" / "_run_synthetic_audit.py"
        harness.write_text(textwrap.dedent("""\
            import sys
            from pathlib import Path
            sys.path.insert(0, str(Path(__file__).parent))
            import audit
            audit.REQUIREMENTS_DOCS = Path(__file__).parent / "_test_req_e2e.txt"
            sys.exit(audit.main())
        """))
        try:
            result = subprocess.run(
                [sys.executable, str(harness), "--strict"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
        finally:
            synthetic.unlink()
            harness.unlink()
        self.assertNotEqual(
            result.returncode,
            0,
            msg=(
                "audit.py --strict should fail when a manifest source "
                "declares a dep that manifest.json does not cover.\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
            ),
        )
        self.assertIn(
            "completely-bogus-attribution-miss",
            result.stdout,
            msg="uncovered dep should appear in the audit output",
        )


class StrictAuditTests(unittest.TestCase):
    """Running the real audit script with --strict against origin/main
    inventory should succeed. If this fails, something is missing from
    DEPENDENCIES.md, NOTICE.md, or docs/reference/licensing.md."""

    def test_audit_strict_passes(self) -> None:
        result = subprocess.run(
            [sys.executable, str(AUDIT), "--strict"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(
            result.returncode,
            0,
            msg=f"audit.py --strict failed:\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )


if __name__ == "__main__":
    unittest.main()
