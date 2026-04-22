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

    def test_all_missing_inputs_fails(self) -> None:
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
            self.assertEqual(rc, 1)
            self.assertFalse(out.exists(), "should not write merged XML when every input is missing")


if __name__ == "__main__":
    # Verbose run so the coverage-diff-gate log records which
    # fixtures actually ran — matters when diagnosing CI-only
    # regressions after a merge_cobertura.py change.
    unittest.main(verbosity=2)
