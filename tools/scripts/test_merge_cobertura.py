#!/usr/bin/env python3
"""Fixture tests for merge_cobertura.py.

Run directly: ``python3 tools/scripts/test_merge_cobertura.py``.
Exits non-zero on any assertion failure. Mirrors the bare-unittest
pattern used by ``test_coverage_diff_comment.py`` and
``test_coverage_tier_check.py`` so the coverage-diff-gate workflow
can run it without PyYAML/pytest dependencies.
"""

from __future__ import annotations

import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

import merge_cobertura as mc


def _write_xml(tmp: Path, name: str, files: dict[str, dict[int, int]]) -> Path:
    """Build a minimal Cobertura XML and write it to ``tmp/name``."""
    coverage = ET.Element("coverage")
    packages = ET.SubElement(coverage, "packages")
    pkg = ET.SubElement(packages, "package", attrib={"name": "x"})
    classes = ET.SubElement(pkg, "classes")
    for filename, per_line in files.items():
        cls = ET.SubElement(
            classes,
            "class",
            attrib={"name": filename.replace("/", "."), "filename": filename},
        )
        lines_el = ET.SubElement(cls, "lines")
        for n, h in per_line.items():
            ET.SubElement(lines_el, "line", attrib={"number": str(n), "hits": str(h)})
    path = tmp / name
    ET.ElementTree(coverage).write(str(path), encoding="utf-8", xml_declaration=True)
    return path


class ParseXmlTests(unittest.TestCase):
    def test_parses_single_file_single_line(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            p = _write_xml(tmp, "a.xml", {"core/foo.cpp": {10: 3}})
            result = mc.parse_xml(p)
            self.assertEqual(result, {"core/foo.cpp": {10: 3}})

    def test_missing_file_returns_empty(self) -> None:
        self.assertEqual(mc.parse_xml(Path("/nonexistent/xyz.xml")), {})

    def test_duplicate_class_entries_take_max_hits(self) -> None:
        """A single source compiled into multiple TUs may produce two
        ``<class>`` entries with the same filename. The parser must
        surface the higher hit count — treating it as 0 would regress
        coverage for files compiled into both a test binary and the
        library (see PR #627's initial split-TU footgun)."""
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            # Hand-build an XML with two <class> entries for one file.
            coverage = ET.Element("coverage")
            pkgs = ET.SubElement(coverage, "packages")
            pkg = ET.SubElement(pkgs, "package", attrib={"name": "x"})
            classes = ET.SubElement(pkg, "classes")
            for hits in (0, 5):
                cls = ET.SubElement(
                    classes,
                    "class",
                    attrib={"name": "core.foo.cpp", "filename": "core/foo.cpp"},
                )
                lines_el = ET.SubElement(cls, "lines")
                ET.SubElement(lines_el, "line", attrib={"number": "10", "hits": str(hits)})
            p = tmp / "dup.xml"
            ET.ElementTree(coverage).write(str(p), encoding="utf-8", xml_declaration=True)
            result = mc.parse_xml(p)
            self.assertEqual(result["core/foo.cpp"][10], 5)

    def test_ignores_incomplete_or_invalid_class_line_entries(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            coverage = ET.Element("coverage")
            pkgs = ET.SubElement(coverage, "packages")
            pkg = ET.SubElement(pkgs, "package", attrib={"name": "x"})
            classes = ET.SubElement(pkg, "classes")

            ET.SubElement(classes, "class", attrib={"name": "missing.filename"})
            ET.SubElement(classes, "class", attrib={"filename": "core/no-lines.cpp"})

            cls = ET.SubElement(
                classes,
                "class",
                attrib={"name": "core.foo.cpp", "filename": "core/foo.cpp"},
            )
            lines_el = ET.SubElement(cls, "lines")
            ET.SubElement(lines_el, "line", attrib={"number": "10"})
            ET.SubElement(lines_el, "line", attrib={"hits": "1"})
            ET.SubElement(lines_el, "line", attrib={"number": "bad", "hits": "1"})
            ET.SubElement(lines_el, "line", attrib={"number": "10", "hits": "5"})
            ET.SubElement(lines_el, "line", attrib={"number": "10", "hits": "1"})
            ET.SubElement(lines_el, "line", attrib={"number": "11", "hits": "0"})

            p = tmp / "invalid-lines.xml"
            ET.ElementTree(coverage).write(str(p), encoding="utf-8", xml_declaration=True)

            result = mc.parse_xml(p)
            self.assertEqual(result["core/no-lines.cpp"], {})
            self.assertEqual(result["core/foo.cpp"], {10: 5, 11: 0})


class MergeTests(unittest.TestCase):
    def test_union_takes_max(self) -> None:
        a = {"core/foo.cpp": {10: 0, 11: 3}}
        b = {"core/foo.cpp": {10: 5, 12: 1}}
        merged = mc.merge([a, b])
        self.assertEqual(merged, {"core/foo.cpp": {10: 5, 11: 3, 12: 1}})

    def test_platform_specific_file_carries_through(self) -> None:
        """The motivating case: an Apple-only file only appears in the
        macOS XML. Merge must preserve it — dropping it is the exact
        silent-skip bug this tool exists to fix (pulp#635)."""
        linux = {"core/runtime/foo.cpp": {1: 5}}
        macos = {
            "core/runtime/foo.cpp": {1: 5},
            "core/format/src/au_adapter.mm": {1: 10, 2: 0},
        }
        windows = {"core/runtime/foo.cpp": {1: 5}}
        merged = mc.merge([linux, macos, windows])
        self.assertIn("core/format/src/au_adapter.mm", merged)
        self.assertEqual(merged["core/format/src/au_adapter.mm"], {1: 10, 2: 0})

    def test_empty_inputs_produce_empty_merge(self) -> None:
        self.assertEqual(mc.merge([]), {})


class RenderTests(unittest.TestCase):
    def test_render_produces_parseable_xml_with_correct_totals(self) -> None:
        merged = {
            "a.cpp": {1: 1, 2: 0, 3: 5},
            "b.cpp": {1: 0, 2: 0},
        }
        tree = mc.render(merged)
        root = tree.getroot()
        # Totals: 5 lines valid, 2 covered (a.cpp:1, a.cpp:3).
        self.assertEqual(root.get("lines-valid"), "5")
        self.assertEqual(root.get("lines-covered"), "2")
        # Files are sorted deterministically so diff-cover output is
        # reproducible across runs (matters for snapshot testing).
        filenames = [c.get("filename") for c in root.iter("class")]
        self.assertEqual(filenames, ["a.cpp", "b.cpp"])
        # Each <class> carries a <lines> subtree matching the input.
        a_lines = {
            int(ln.get("number")): int(ln.get("hits"))
            for c in root.iter("class")
            if c.get("filename") == "a.cpp"
            for ln in c.iter("line")
        }
        self.assertEqual(a_lines, {1: 1, 2: 0, 3: 5})


class MainCliTests(unittest.TestCase):
    def test_end_to_end_three_xmls(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            linux = _write_xml(tmp, "linux.xml", {"core/foo.cpp": {10: 0, 11: 3}})
            macos = _write_xml(
                tmp,
                "macos.xml",
                {
                    "core/foo.cpp": {10: 1},  # hits line 10 where Linux missed it
                    "core/format/src/au_adapter.mm": {1: 4, 2: 0},
                },
            )
            windows = _write_xml(
                tmp,
                "windows.xml",
                {"core/runtime/winfoo.cpp": {1: 2}},
            )
            out = tmp / "merged.xml"
            rc = mc.main(["--out", str(out), str(linux), str(macos), str(windows)])
            self.assertEqual(rc, 0)
            # Re-parse and assert the union.
            reparsed = mc.parse_xml(out)
            self.assertEqual(reparsed["core/foo.cpp"], {10: 1, 11: 3})  # linux's 0 lost to macos's 1
            self.assertEqual(reparsed["core/format/src/au_adapter.mm"], {1: 4, 2: 0})
            self.assertEqual(reparsed["core/runtime/winfoo.cpp"], {1: 2})

    def test_missing_inputs_skipped_but_one_present_succeeds(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            linux = _write_xml(tmp, "linux.xml", {"core/foo.cpp": {1: 1}})
            out = tmp / "merged.xml"
            rc = mc.main(
                [
                    "--out",
                    str(out),
                    str(linux),
                    str(tmp / "nonexistent-macos.xml"),
                    str(tmp / "nonexistent-windows.xml"),
                ]
            )
            self.assertEqual(rc, 0)
            reparsed = mc.parse_xml(out)
            self.assertEqual(reparsed, {"core/foo.cpp": {1: 1}})

    def test_all_missing_inputs_returns_dedicated_sentinel(self) -> None:
        """Codex P1 review on PR #660: the all-missing case must use a
        DEDICATED exit code (2), not the catch-all "uncaught exception"
        code 1, so the CI workflow can distinguish a benign
        "no artifacts uploaded" from a real failure (parse error,
        script bug). Pin the contract here."""
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            out = tmp / "merged.xml"
            rc = mc.main(
                [
                    "--out",
                    str(out),
                    str(tmp / "a.xml"),
                    str(tmp / "b.xml"),
                ]
            )
            self.assertEqual(rc, mc.EXIT_ALL_INPUTS_MISSING)
            self.assertEqual(rc, 2, "exit code 2 is the workflow contract; do not change without updating coverage.yml")
            self.assertFalse(out.exists(), "should not write merged XML when every input is missing")

    def test_corrupt_xml_input_exits_with_real_error_code(self) -> None:
        """Codex P1 review on PR #660: a malformed Cobertura artifact
        (e.g. truncated upload causing ParseError) must NOT take the
        all-missing fallback path — that would silently bypass the
        required diff-coverage gate. Exit code 1 (real-error) ensures
        the workflow's `rc -eq 2` branch does not match."""
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            corrupt = tmp / "corrupt.xml"
            corrupt.write_text("<<<this is not valid XML>>>")
            out = tmp / "merged.xml"
            rc = mc.main(["--out", str(out), str(corrupt)])
            self.assertEqual(rc, 1, "corrupt-input failure must use the real-error exit code, not the all-missing sentinel (2)")
            self.assertNotEqual(rc, mc.EXIT_ALL_INPUTS_MISSING)

    def test_windows_backslash_paths_normalise_to_forward_slash(self) -> None:
        """Windows Cobertura artifacts emit filenames with backslash
        separators (`core\\format\\src\\clap_adapter.cpp`). Linux and
        macOS use forward slashes. Without normalisation the merge
        treats them as TWO files; diff-cover then matches the backslash
        variant against the PR diff (which uses forward slashes from
        git), finds 0 hits, and silently reports 0% coverage on
        cross-platform code that was actually exercised on Linux. PR
        #660 hit this bug.

        Pin the contract: backslash filenames in the input collapse to
        forward-slash keys in the merged map, with hit counts unioned
        across both spellings.
        """
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            linux = _write_xml(tmp, "linux.xml", {"core/format/src/clap_adapter.cpp": {100: 5}})
            # Hand-build a Windows XML with backslashes — _write_xml
            # uses path-string-as-given, so backslashes survive.
            windows = _write_xml(tmp, "win.xml", {"core\\format\\src\\clap_adapter.cpp": {100: 0, 101: 3}})
            merged = mc.merge([mc.parse_xml(linux), mc.parse_xml(windows)])
            self.assertIn("core/format/src/clap_adapter.cpp", merged)
            self.assertNotIn("core\\format\\src\\clap_adapter.cpp", merged)
            # Linux's hit on line 100 must survive (max(5, 0) = 5);
            # Windows's hit on line 101 must also survive (max(_, 3) = 3).
            self.assertEqual(merged["core/format/src/clap_adapter.cpp"], {100: 5, 101: 3})

    def test_excludes_test_and_external_paths(self) -> None:
        """Mirror of `COVERAGE_IGNORE_REGEX` in scripts/run_coverage.sh:
        test/, _deps/, external/, build/, examples/, Catch2/,
        fetchcontent-src/ are not coverage-bearing. The Windows
        cobertura leg historically leaked `test\\*` because its
        backslash paths slipped past the OS-side regex; this script's
        normalisation + filter catches them uniformly. Pin both ends
        of the contract."""
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            xml = _write_xml(
                tmp,
                "mixed.xml",
                {
                    "core/format/src/clap_adapter.cpp": {1: 5},
                    "test/test_clap.cpp": {10: 2},
                    "test\\test_winonly.cpp": {1: 1},  # backslash variant
                    "external/AAX-SDK/Foo.cpp": {1: 1},
                    "_deps/catch2-src/catch.cpp": {1: 1},
                    "examples/foo/bar.cpp": {1: 1},
                },
            )
            parsed = mc.parse_xml(xml)
            self.assertEqual(set(parsed.keys()), {"core/format/src/clap_adapter.cpp"})

    def test_corrupt_xml_with_other_valid_inputs_still_fails(self) -> None:
        """Even when a usable input is present alongside a corrupt one,
        the corrupt input fails the merge — partial coverage from the
        good legs would otherwise hide the corruption and silently
        downgrade the gate's strictness."""
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            good = _write_xml(tmp, "good.xml", {"core/foo.cpp": {1: 5}})
            corrupt = tmp / "corrupt.xml"
            corrupt.write_text("<coverage><not-closed>")
            out = tmp / "merged.xml"
            rc = mc.main(["--out", str(out), str(good), str(corrupt)])
            self.assertEqual(rc, 1)


if __name__ == "__main__":
    # Verbose run so the coverage-diff-gate log records which
    # fixtures actually ran — matters when diagnosing CI-only
    # regressions after a merge_cobertura.py change.
    unittest.main(verbosity=2)
