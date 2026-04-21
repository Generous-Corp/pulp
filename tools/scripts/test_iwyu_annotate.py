#!/usr/bin/env python3
"""Fixture tests for tools/scripts/iwyu_annotate.py.

Guards the IWYU-output parser and GitHub-annotation renderer used by
the issue #594 Phase 2 advisory gate. Pure-Python and fast — no IWYU
binary, no compile_commands.json, no network.

Run:
    python3 tools/scripts/test_iwyu_annotate.py
"""

from __future__ import annotations

import io
import pathlib
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import iwyu_annotate as ia  # noqa: E402


# ── Sample IWYU output snippets ────────────────────────────────────────

SINGLE_FILE_ADD = """\
core/signal/include/pulp/signal/fft.hpp should add these lines:
#include <memory>     // for unique_ptr
#include <vector>     // for vector

core/signal/include/pulp/signal/fft.hpp should remove these lines:
- #include <cstdint>  // lines 3-3

The full include-list for core/signal/include/pulp/signal/fft.hpp:
#include <algorithm>
#include <memory>
#include <vector>
---
"""

MULTI_FILE = """\
core/state/include/pulp/state/state_tree.hpp should add these lines:
#include <algorithm>   // for sort

core/state/include/pulp/state/state_tree.hpp should remove these lines:

The full include-list for core/state/include/pulp/state/state_tree.hpp:
#include <algorithm>
---

test/test_cli_version_diag.cpp should add these lines:
#include <atomic>      // for atomic

test/test_cli_version_diag.cpp should remove these lines:

The full include-list for test/test_cli_version_diag.cpp:
#include <atomic>
---
"""

# IWYU emits this exact section when a file is already clean.
NO_CHANGES = """\
(core/view/src/button.cpp has correct #includes/fwd-decls)
"""

# Paths under build/, external/, or _deps/ must be ignored.
NOISE_IN_DEPS = """\
build-coverage/_deps/catch2-build/src/catch2/catch_test_macros.hpp should add these lines:
#include <string>

build-coverage/_deps/catch2-build/src/catch2/catch_test_macros.hpp should remove these lines:

The full include-list for build-coverage/_deps/catch2-build/src/catch2/catch_test_macros.hpp:
#include <string>
---
"""


class ParserTests(unittest.TestCase):
    def test_iter_add_findings_extracts_each_include(self) -> None:
        findings = list(ia.iter_add_findings(SINGLE_FILE_ADD.splitlines()))
        self.assertEqual(
            findings,
            [
                ("core/signal/include/pulp/signal/fft.hpp", "<memory>", "for unique_ptr"),
                ("core/signal/include/pulp/signal/fft.hpp", "<vector>", "for vector"),
            ],
        )

    def test_iter_add_findings_walks_multiple_files(self) -> None:
        findings = list(ia.iter_add_findings(MULTI_FILE.splitlines()))
        self.assertEqual(len(findings), 2)
        paths = {fp for fp, _, _ in findings}
        self.assertEqual(
            paths,
            {
                "core/state/include/pulp/state/state_tree.hpp",
                "test/test_cli_version_diag.cpp",
            },
        )

    def test_iter_add_findings_ignores_remove_section(self) -> None:
        # Confirm we never emit a finding for "should remove these lines".
        # This is the issue #594 non-goal: we aren't enforcing minimal
        # includes, only missing ones.
        findings = list(ia.iter_add_findings(SINGLE_FILE_ADD.splitlines()))
        for _, include, _ in findings:
            self.assertNotIn("cstdint", include)

    def test_iter_add_findings_handles_no_findings(self) -> None:
        findings = list(ia.iter_add_findings(NO_CHANGES.splitlines()))
        self.assertEqual(findings, [])

    def test_iter_add_findings_handles_missing_reason(self) -> None:
        # Not every add line has "// for X" — IWYU sometimes omits it.
        text = """\
foo.hpp should add these lines:
#include <memory>

foo.hpp should remove these lines:
"""
        findings = list(ia.iter_add_findings(text.splitlines()))
        self.assertEqual(findings, [("foo.hpp", "<memory>", "")])


class FilterTests(unittest.TestCase):
    def test_is_relevant_accepts_source_and_headers(self) -> None:
        self.assertTrue(ia.is_relevant("core/signal/src/fft.cpp"))
        self.assertTrue(ia.is_relevant("core/signal/include/pulp/signal/fft.hpp"))
        self.assertTrue(ia.is_relevant("apple/src/foo.mm"))

    def test_is_relevant_rejects_non_cpp(self) -> None:
        self.assertFalse(ia.is_relevant("README.md"))
        self.assertFalse(ia.is_relevant("CMakeLists.txt"))

    def test_is_relevant_rejects_external_and_build_trees(self) -> None:
        self.assertFalse(ia.is_relevant("external/choc/audio/foo.h"))
        self.assertFalse(ia.is_relevant("build/CMakeFiles/foo.cpp"))
        self.assertFalse(ia.is_relevant("build-coverage/_deps/catch2-src/x.hpp"))

    def test_load_changed_files_missing_is_empty_set(self) -> None:
        # Missing file (e.g. push to main with no diff) should return
        # empty set, not None — None means "no filter at all".
        path = pathlib.Path("/does-not-exist-12345.txt")
        self.assertEqual(ia.load_changed_files(path), set())

    def test_load_changed_files_strips_blank_lines(self) -> None:
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as fh:
            fh.write("core/foo.cpp\n\n  core/bar.hpp  \n")
            tmp = pathlib.Path(fh.name)
        try:
            self.assertEqual(
                ia.load_changed_files(tmp),
                {"core/foo.cpp", "core/bar.hpp"},
            )
        finally:
            tmp.unlink()


class RunTests(unittest.TestCase):
    def test_run_filters_to_changed_files_on_pr(self) -> None:
        # Only the state_tree.hpp change is in scope for this PR.
        annotations, summary = ia.run(
            MULTI_FILE.splitlines(),
            changed_files={"core/state/include/pulp/state/state_tree.hpp"},
            advisory=True,
            flip_date="2026-05-05",
        )
        self.assertEqual(len(annotations), 1)
        self.assertIn("state_tree.hpp", annotations[0])
        self.assertIn("algorithm", annotations[0])
        # The unrelated test file's finding must be suppressed.
        self.assertNotIn("test_cli_version_diag", " ".join(annotations))
        # Summary lists the single finding.
        self.assertIn("Found **1**", summary)

    def test_run_no_filter_reports_everything_relevant(self) -> None:
        annotations, summary = ia.run(
            MULTI_FILE.splitlines(),
            changed_files=None,
            advisory=True,
            flip_date="2026-05-05",
        )
        self.assertEqual(len(annotations), 2)
        self.assertIn("Found **2**", summary)

    def test_run_drops_noisy_deps_paths(self) -> None:
        annotations, summary = ia.run(
            NOISE_IN_DEPS.splitlines(),
            changed_files=None,
            advisory=True,
            flip_date="2026-05-05",
        )
        self.assertEqual(annotations, [])
        self.assertIn("No missing-include", summary)

    def test_run_empty_input_emits_clean_summary(self) -> None:
        annotations, summary = ia.run(
            iter([]),
            changed_files=None,
            advisory=True,
            flip_date="2026-05-05",
        )
        self.assertEqual(annotations, [])
        self.assertIn("No missing-include", summary)

    def test_annotation_shape_matches_github_workflow_command(self) -> None:
        # GitHub parses `::warning file=...,line=...,title=...::message`
        # and strips the message at newlines. We escape CR/LF.
        out = ia.render_annotation("foo.hpp", "<memory>", "for unique_ptr\nmultiline")
        self.assertTrue(out.startswith("::warning "))
        self.assertIn("file=foo.hpp", out)
        self.assertIn("line=1", out)
        self.assertIn("%0A", out)  # newline encoded per GH spec
        # Message body is present.
        self.assertIn("memory", out)

    def test_summary_mentions_flip_date_and_issue(self) -> None:
        _, summary = ia.run(
            SINGLE_FILE_ADD.splitlines(),
            changed_files=None,
            advisory=True,
            flip_date="2026-05-05",
        )
        self.assertIn("2026-05-05", summary)
        self.assertIn("#594", summary)


if __name__ == "__main__":
    unittest.main(verbosity=2)
