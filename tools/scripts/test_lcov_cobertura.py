#!/usr/bin/env python3
"""Focused tests for Pulp's vendored LCOV -> Cobertura converter."""

from __future__ import annotations

import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools" / "scripts"))
import lcov_cobertura as lc  # noqa: E402


class ParseTests(unittest.TestCase):
    def test_relpath_value_error_keeps_source_filename(self) -> None:
        lcov = "SF:D:\\cache\\dep.cpp\nDA:10,1\nend_of_record\n"

        with mock.patch.object(lc.os.path, "relpath", side_effect=ValueError):
            data = lc.LcovCobertura(lcov, base_dir="C:\\repo").parse(timestamp=123)

        self.assertEqual(data["timestamp"], "123")
        self.assertEqual(data["summary"]["lines-total"], 1)
        self.assertEqual(data["summary"]["lines-covered"], 1)
        self.assertIn("D:\\cache\\dep.cpp", data["packages"][""]["classes"])

    def test_excluded_packages_are_removed_before_summary(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            keep = root / "src" / "keep" / "a.cpp"
            generated = root / "src" / "generated" / "b.cpp"
            lcov = (
                f"SF:{keep}\nDA:1,1\nend_of_record\n"
                f"SF:{generated}\nDA:1,1\nend_of_record\n"
            )

            data = lc.LcovCobertura(
                lcov,
                base_dir=str(root),
                excludes=[r"^src\.generated$"],
            ).parse(timestamp=123)

        self.assertEqual(set(data["packages"]), {"src.keep"})
        self.assertEqual(data["summary"]["lines-total"], 1)
        self.assertEqual(data["summary"]["lines-covered"], 1)


class MainTests(unittest.TestCase):
    def test_main_writes_cobertura_xml(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            lcov_file = root / "coverage.lcov"
            output = root / "coverage.xml"
            lcov_file.write_text(
                f"SF:{root / 'src' / 'a.cpp'}\nDA:7,3\nend_of_record\n",
                encoding="utf-8",
            )

            lc.main([
                "lcov_cobertura.py",
                str(lcov_file),
                "-b",
                str(root),
                "-o",
                str(output),
            ])

            xml_root = ET.fromstring(output.read_text(encoding="utf-8"))

        self.assertEqual(xml_root.attrib["lines-valid"], "1")
        self.assertEqual(xml_root.attrib["lines-covered"], "1")
        self.assertEqual(
            xml_root.find(".//class").attrib["filename"],
            "src/a.cpp",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
