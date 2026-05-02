#!/usr/bin/env python3
"""Focused edge tests for merge_cobertura.py."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

import merge_cobertura as mc


SCRIPT = Path(__file__).with_name("merge_cobertura.py")


def _write_tree(path: Path, root: ET.Element) -> Path:
    ET.ElementTree(root).write(str(path), encoding="utf-8", xml_declaration=True)
    return path


class ParseXmlEdgeTests(unittest.TestCase):
    def test_parse_ignores_incomplete_and_invalid_line_records(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            coverage = ET.Element("coverage")
            classes = ET.SubElement(
                ET.SubElement(ET.SubElement(coverage, "packages"), "package"),
                "classes",
            )

            ET.SubElement(classes, "class")
            ET.SubElement(
                classes,
                "class",
                attrib={"name": "missing-lines", "filename": "core/missing_lines.cpp"},
            )

            cls = ET.SubElement(
                classes,
                "class",
                attrib={"name": "core.edge.cpp", "filename": "core/edge.cpp"},
            )
            lines = ET.SubElement(cls, "lines")
            ET.SubElement(lines, "line", attrib={"number": "1"})
            ET.SubElement(lines, "line", attrib={"hits": "1"})
            ET.SubElement(lines, "line", attrib={"number": "two", "hits": "1"})
            ET.SubElement(lines, "line", attrib={"number": "3", "hits": "many"})
            ET.SubElement(lines, "line", attrib={"number": "4", "hits": "5"})
            ET.SubElement(lines, "line", attrib={"number": "4", "hits": "1"})
            ET.SubElement(lines, "line", attrib={"number": "5", "hits": "0"})

            parsed = mc.parse_xml(_write_tree(tmp / "edge.xml", coverage))

        self.assertEqual(
            parsed,
            {
                "core/missing_lines.cpp": {},
                "core/edge.cpp": {4: 5, 5: 0},
            },
        )


class RenderEdgeTests(unittest.TestCase):
    def test_render_handles_empty_totals_and_empty_file_line_sets(self) -> None:
        root = mc.render({}).getroot()
        self.assertEqual(root.get("line-rate"), "0.0000")
        self.assertEqual(root.get("lines-valid"), "0")
        self.assertEqual(root.get("lines-covered"), "0")
        self.assertEqual(list(root.iter("class")), [])

        root = mc.render({"core/empty.cpp": {}}).getroot()
        cls = next(root.iter("class"))
        self.assertEqual(cls.get("filename"), "core/empty.cpp")
        self.assertEqual(cls.get("line-rate"), "0.0000")
        self.assertEqual(list(cls.iter("line")), [])


class CliEntrypointTests(unittest.TestCase):
    def test_script_entrypoint_writes_output(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            coverage = ET.Element("coverage")
            classes = ET.SubElement(
                ET.SubElement(ET.SubElement(coverage, "packages"), "package"),
                "classes",
            )
            cls = ET.SubElement(
                classes,
                "class",
                attrib={"name": "core.cli.cpp", "filename": "core/cli.cpp"},
            )
            lines = ET.SubElement(cls, "lines")
            ET.SubElement(lines, "line", attrib={"number": "9", "hits": "2"})
            src = _write_tree(tmp / "src.xml", coverage)
            out = tmp / "nested" / "merged.xml"

            proc = subprocess.run(
                [sys.executable, str(SCRIPT), "--out", str(out), str(src)],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            reparsed = mc.parse_xml(out)

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("merged 1 XML(s)", proc.stderr)
        self.assertEqual(reparsed, {"core/cli.cpp": {9: 2}})


if __name__ == "__main__":
    unittest.main(verbosity=2)
