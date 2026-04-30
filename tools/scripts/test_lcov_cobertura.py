#!/usr/bin/env python3
"""Focused tests for Pulp's vendored LCOV -> Cobertura converter."""

from __future__ import annotations

import io
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from contextlib import redirect_stderr, redirect_stdout
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

    def test_duplicate_records_merge_branch_and_method_data(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "include" / "widget.hpp"
            lcov = (
                f"SF:{source}\n"
                "FN:4,_Z6widgetv\n"
                "FNDA:0,_Z6widgetv\n"
                "DA:4,0\n"
                "BRDA:4,0,0,-\n"
                "BRDA:4,0,1,3\n"
                "BRF:2\n"
                "BRH:1\n"
                "end_of_record\n"
                f"SF:{source}\n"
                "FNDA:2,_Z6widgetv\n"
                "DA:4,5\n"
                "BRDA:4,0,0,1\n"
                "BRDA:4,0,1,0\n"
                "end_of_record\n"
            )

            data = lc.LcovCobertura(lcov, base_dir=str(root)).parse(timestamp=123)

        package = data["packages"]["include"]
        klass = package["classes"]["include/widget.hpp"]
        line = klass["lines"][4]

        self.assertEqual(data["summary"]["lines-total"], 1)
        self.assertEqual(data["summary"]["lines-covered"], 1)
        self.assertEqual(data["summary"]["branches-total"], 2)
        self.assertEqual(data["summary"]["branches-covered"], 1)
        self.assertEqual(line["hits"], "5")
        self.assertEqual(line["branch"], "true")
        self.assertEqual(line["branches-total"], 2)
        self.assertEqual(line["branches-covered"], 1)
        self.assertEqual(klass["methods"]["_Z6widgetv"], ["0", "2"])


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

    def test_main_version_prints_and_exits_zero(self) -> None:
        stdout = io.StringIO()

        with self.assertRaises(SystemExit) as cm, redirect_stdout(stdout):
            lc.main(["lcov_cobertura.py", "--version"])

        self.assertEqual(cm.exception.code, 0)
        self.assertIn("[lcov_cobertura ", stdout.getvalue())

    def test_main_usage_error_exits_one(self) -> None:
        stdout = io.StringIO()

        with self.assertRaises(SystemExit) as cm, redirect_stdout(stdout):
            lc.main(["lcov_cobertura.py"])

        self.assertEqual(cm.exception.code, 1)
        self.assertIn("Usage:", stdout.getvalue())

    def test_main_missing_input_reports_conversion_error(self) -> None:
        stderr = io.StringIO()

        with tempfile.TemporaryDirectory() as td, redirect_stderr(stderr):
            missing = Path(td) / "missing.lcov"
            lc.main(["lcov_cobertura.py", str(missing)])

        self.assertIn("Unable to convert", stderr.getvalue())
        self.assertIn("missing.lcov", stderr.getvalue())

    def test_main_demangle_requires_cppfilt(self) -> None:
        with mock.patch.object(lc, "HAVE_CPPFILT", False):
            with self.assertRaisesRegex(RuntimeError, "C\\+\\+ filter executable"):
                lc.main(["lcov_cobertura.py", "-d", "coverage.lcov"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
