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
import tempfile
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


class LicenseVerificationTests(unittest.TestCase):
    """Truthfulness gate (``--verify-licenses``).

    The consistency and completeness gates only ask whether a dependency is
    *named* in each attribution file. Both of the bugs below passed a green
    ``--strict`` audit: NOTICE.md reproduced a truncated MIT license for 25 of
    its 26 MIT entries, and the VST3 SDK was labeled MIT on all four surfaces
    while the pinned tree was "Steinberg VST3 License OR GPLv3".
    """

    def _write_notice(self, body: str):
        tmp = ROOT / "tools" / "deps" / "_test_notice_verify.md"
        tmp.write_text(body)
        self.addCleanup(tmp.unlink)
        original = audit.NOTICE_MD
        audit.NOTICE_MD = tmp
        self.addCleanup(lambda: setattr(audit, "NOTICE_MD", original))

    def test_complete_mit_notice_is_accepted(self) -> None:
        self._write_notice(
            "## dep\n\nCopyright (c) 2026 Someone\n\n"
            "Permission is hereby granted, free of charge, to any person "
            "obtaining a copy of this software ..., to deal in the Software "
            "without restriction, ...\n\n"
            "The above copyright notice and this permission notice "
            "shall be included in all copies.\n\n"
            'THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.\n'
        )
        self.assertEqual(audit.find_notice_truncations(), [])

    def test_notice_missing_warranty_disclaimer_is_flagged(self) -> None:
        self._write_notice(
            "## dep\n\nPermission is hereby granted, free of charge, to any person "
            "obtaining a copy of this software ..., to deal in the Software "
            "without restriction, ...\n\n"
            "The above copyright notice and this permission notice "
            "shall be included in all copies.\n"
        )
        self.assertEqual(
            audit.find_notice_truncations(), [("dep", ["warranty disclaimer"])]
        )

    def test_notice_missing_inclusion_condition_is_flagged(self) -> None:
        self._write_notice(
            "## dep\n\nPermission is hereby granted, free of charge, to any person "
            "obtaining a copy of this software ..., to deal in the Software "
            "without restriction, ...\n\n"
            'THE SOFTWARE IS PROVIDED "AS IS".\n'
        )
        self.assertEqual(
            audit.find_notice_truncations(), [("dep", ["inclusion condition"])]
        )

    def test_boost_license_is_not_judged_as_truncated_mit(self) -> None:
        """BSL-1.0 opens with MIT's "Permission is hereby granted" line but
        words its condition "must be included in all copies". Matching on the
        shared opener reported Catch2 (BSL-1.0) as a truncated MIT entry."""
        self._write_notice(
            "## Catch2\n\nCopyright (c) 2022 Two Blue Cubes Ltd.\n\n"
            "Boost Software License - Version 1.0\n\n"
            "Permission is hereby granted, free of charge, to any person or "
            "organization obtaining a copy of the software ...\n\n"
            "The copyright notices in the Software and this entire statement "
            "must be included in all copies.\n\n"
            'THE SOFTWARE IS PROVIDED "AS IS".\n'
        )
        self.assertEqual(audit.find_notice_truncations(), [])

    def test_truncated_boost_license_is_flagged(self) -> None:
        self._write_notice(
            "## Catch2\n\nBoost Software License - Version 1.0\n\n"
            "Permission is hereby granted, free of charge, ...\n"
        )
        self.assertEqual(
            audit.find_notice_truncations(),
            [("Catch2", ["inclusion condition", "warranty disclaimer"])],
        )

    def test_non_mit_notice_entries_are_left_alone(self) -> None:
        """BSD/Apache/zlib/public-domain entries have no MIT grant line and
        must not be judged against MIT's structure."""
        self._write_notice(
            "## a bsd dep\n\nRedistribution and use in source and binary forms"
            ", with or without modification, are permitted provided that ...\n"
        )
        self.assertEqual(audit.find_notice_truncations(), [])

    def test_repo_notice_has_no_truncated_mit_entries(self) -> None:
        truncated = audit.find_notice_truncations()
        self.assertEqual(
            truncated,
            [],
            "NOTICE.md reproduces an incomplete MIT permission notice for: "
            + ", ".join(f"{n} (missing {', '.join(m)})" for n, m in truncated),
        )

    def test_copyleft_tree_contradicting_an_mit_claim_is_flagged(self) -> None:
        """The VST3 regression itself: a tree whose LICENSE offers a GPL
        alternative while the manifest declares MIT."""
        tree = Path(self.enterContext(tempfile.TemporaryDirectory()))
        (tree / "LICENSE.txt").write_text(
            "This Software Development Kit is licensed under the terms of the "
            "Steinberg VST3 License,\nor alternatively under the terms of the "
            "General Public License (GPL) Version 3.\n"
        )
        original = audit.local_source_tree
        audit.local_source_tree = lambda dep: tree
        self.addCleanup(lambda: setattr(audit, "local_source_tree", original))

        status, problems = audit.verify_dep_license({"name": "x", "license": "MIT"})
        self.assertEqual(status, "verified")
        self.assertEqual(len(problems), 1)
        self.assertIn("General Public License", problems[0])
        self.assertIn("FORBIDDEN", problems[0])

    def test_bare_gpl_substring_does_not_false_positive(self) -> None:
        """"GPL" appears inside identifiers such as gPluginFactory, so the
        scan matches the spelled-out phrase instead."""
        tree = Path(self.enterContext(tempfile.TemporaryDirectory()))
        (tree / "LICENSE.txt").write_text(
            "MIT License\n\ngPluginFactory = new CPluginFactory (factoryInfo);\n"
        )
        original = audit.local_source_tree
        audit.local_source_tree = lambda dep: tree
        self.addCleanup(lambda: setattr(audit, "local_source_tree", original))

        _, problems = audit.verify_dep_license({"name": "x", "license": "MIT"})
        self.assertEqual(problems, [])

    def test_missing_tree_reports_unverified_not_pass(self) -> None:
        """An absent tree is not evidence of a correct license."""
        original = audit.local_source_tree
        audit.local_source_tree = lambda dep: None
        self.addCleanup(lambda: setattr(audit, "local_source_tree", original))

        status, problems = audit.verify_dep_license({"name": "x", "license": "MIT"})
        self.assertEqual(status, "unverified")
        self.assertEqual(problems, [])

    def test_external_tree_wins_over_stale_cache_entry(self) -> None:
        """external/<dep> is what the build compiles. A cache holding an older
        ref of the same dependency must not shadow it — that made the audit
        verify a version the repo does not use."""
        base = Path(self.enterContext(tempfile.TemporaryDirectory()))
        external = base / "external"
        (external / "widget").mkdir(parents=True)
        cache = base / "cache"
        (cache / "widget-v1.0.0").mkdir(parents=True)

        originals = (audit.EXTERNAL_DIR, audit.FETCHCONTENT_CACHE)
        audit.EXTERNAL_DIR, audit.FETCHCONTENT_CACHE = external, cache
        self.addCleanup(
            lambda: setattr(audit, "EXTERNAL_DIR", originals[0])
            or setattr(audit, "FETCHCONTENT_CACHE", originals[1])
        )

        tree = audit.local_source_tree(
            {"name": "widget", "version": "v1.0.0", "external_names": ["widget"]}
        )
        self.assertEqual(tree, external / "widget")

    def test_cache_hit_must_match_the_pinned_version(self) -> None:
        """The cache accumulates every ref ever fetched, so a name match alone
        would verify whichever version happened to be lying around."""
        base = Path(self.enterContext(tempfile.TemporaryDirectory()))
        external = base / "external"
        external.mkdir()
        cache = base / "cache"
        (cache / "widget-v1.0.0").mkdir(parents=True)

        originals = (audit.EXTERNAL_DIR, audit.FETCHCONTENT_CACHE)
        audit.EXTERNAL_DIR, audit.FETCHCONTENT_CACHE = external, cache
        self.addCleanup(
            lambda: setattr(audit, "EXTERNAL_DIR", originals[0])
            or setattr(audit, "FETCHCONTENT_CACHE", originals[1])
        )

        dep = {"name": "widget", "external_names": ["widget"]}
        self.assertEqual(
            audit.local_source_tree({**dep, "version": "v1.0.0"}), cache / "widget-v1.0.0"
        )
        self.assertIsNone(audit.local_source_tree({**dep, "version": "v2.0.0"}))

    def test_multi_hyphen_ref_resolves(self) -> None:
        """Cache dirs are "<name>-<ref>" and refs contain hyphens, so the ref
        is stripped using the pinned version rather than by splitting on "-"."""
        base = Path(self.enterContext(tempfile.TemporaryDirectory()))
        external = base / "external"
        external.mkdir()
        cache = base / "cache"
        (cache / "sdl3-release-3.2.12").mkdir(parents=True)

        originals = (audit.EXTERNAL_DIR, audit.FETCHCONTENT_CACHE)
        audit.EXTERNAL_DIR, audit.FETCHCONTENT_CACHE = external, cache
        self.addCleanup(
            lambda: setattr(audit, "EXTERNAL_DIR", originals[0])
            or setattr(audit, "FETCHCONTENT_CACHE", originals[1])
        )

        tree = audit.local_source_tree(
            {"name": "SDL3", "version": "release-3.2.12", "external_names": ["sdl3"]}
        )
        self.assertEqual(tree, cache / "sdl3-release-3.2.12")

    def test_vst3_pin_is_the_mit_relicensed_tag(self) -> None:
        """Every tag before v3.8.0 offers only "Steinberg VST3 License OR
        GPLv3"; Pulp redistributes these headers in its MIT SDK artifacts."""
        dep = next(
            d for d in audit.load_manifest() if d["name"] == "VST3 SDK"
        )
        self.assertEqual(dep["license"], "MIT")
        self.assertEqual(dep["version"], "v3.8.0_build_66")
        self.assertEqual(dep["upstream"]["ref"], "v3.8.0_build_66")

    def test_license_files_deduplicated_on_case_insensitive_filesystems(self) -> None:
        """macOS resolves LICENSE.txt and license.txt to one file, which
        otherwise gets scanned and reported twice."""
        tree = Path(self.enterContext(tempfile.TemporaryDirectory()))
        (tree / "LICENSE.txt").write_text("MIT License\n")
        found = audit.find_license_files(tree)
        self.assertEqual(len(found), 1)


if __name__ == "__main__":
    unittest.main()
