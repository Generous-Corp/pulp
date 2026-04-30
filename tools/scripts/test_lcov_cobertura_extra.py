#!/usr/bin/env python3
"""Extra edge coverage for Pulp's vendored LCOV -> Cobertura converter."""

from __future__ import annotations

import contextlib
import io
import runpy
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools" / "scripts"))
import lcov_cobertura as lc  # noqa: E402


class ParseAndXmlEdgeTests(unittest.TestCase):
    def test_zero_hit_line_is_counted_but_not_covered(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "src" / "zero.cpp"
            lcov = f"SF:{source}\nDA:12,0\nend_of_record\n"

            data = lc.LcovCobertura(lcov, base_dir=str(root)).parse(timestamp=123)

        cls = data["packages"]["src"]["classes"]["src/zero.cpp"]
        self.assertEqual(data["summary"]["lines-total"], 1)
        self.assertEqual(data["summary"]["lines-covered"], 0)
        self.assertEqual(cls["lines"][12]["hits"], "0")
        self.assertEqual(data["packages"]["src"]["line-rate"], "0.0")

    def test_duplicate_records_merge_branches_methods_and_non_numeric_hits(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "src" / "lib" / "a.cpp"
            lcov = (
                f"SF:{source}\n"
                "BRDA:5,0,0,-\n"
                "BRDA:5,0,1,3\n"
                "DA:5,not-a-number\n"
                "FNDA:2,_Z3foov\n"
                "FN:9,_Z3barv\n"
                "FNDA:0,_Z3barv\n"
                "BRF:2\n"
                "BRH:1\n"
                "end_of_record\n"
                f"SF:{source}\n"
                "DA:5,4\n"
                "BRDA:5,0,0,1\n"
                "FN:9,_Z3barv\n"
                "FNDA:5,_Z3barv\n"
            )

            converter = lc.LcovCobertura(lcov, base_dir=str(root))
            converter.format = lambda name: f"demangled:{name}"
            data = converter.parse(timestamp=456)
            xml_root = ET.fromstring(converter.generate_cobertura_xml(data))

        cls = data["packages"]["src.lib"]["classes"]["src/lib/a.cpp"]
        self.assertEqual(data["summary"]["lines-total"], 1)
        self.assertEqual(data["summary"]["lines-covered"], 1)
        self.assertEqual(data["summary"]["branches-total"], 2)
        self.assertEqual(data["summary"]["branches-covered"], 1)
        self.assertEqual(cls["lines"][5]["hits"], "4")
        self.assertEqual(cls["lines"][5]["branches-total"], 2)
        self.assertEqual(cls["lines"][5]["branches-covered"], 1)
        self.assertEqual(cls["methods"]["_Z3foov"], ["0", "2"])
        self.assertEqual(cls["methods"]["_Z3barv"], ["9", "5"])

        class_el = xml_root.find(".//class")
        self.assertEqual(class_el.attrib["line-rate"], "1.0")
        methods = {
            method.attrib["name"]: method
            for method in xml_root.findall(".//method")
        }
        self.assertEqual(methods["demangled:_Z3foov"].attrib["line-rate"], "1.0")
        self.assertEqual(methods["demangled:_Z3barv"].attrib["line-rate"], "1.0")
        line = xml_root.find(".//class/lines/line")
        self.assertEqual(line.attrib["condition-coverage"], "50% (1/2)")

    def test_empty_parse_uses_current_timestamp_and_zero_rates(self) -> None:
        with mock.patch.object(lc.time, "time", return_value=789):
            data = lc.LcovCobertura("", base_dir="/tmp/source").parse()

        self.assertEqual(data["timestamp"], "789")
        self.assertEqual(data["summary"]["lines-total"], 0)
        self.assertEqual(data["packages"], {})
        self.assertEqual(lc.LcovCobertura("")._percent(0, 0), "0.0")


class DemanglerTests(unittest.TestCase):
    def test_converter_uses_demangler_when_requested(self) -> None:
        fake_demangler = mock.Mock()
        fake_demangler.demangle.return_value = "pretty()"

        with mock.patch.object(lc, "Demangler", return_value=fake_demangler) as demangler:
            converter = lc.LcovCobertura("", demangle=True)

        demangler.assert_called_once_with()
        self.assertEqual(converter.format("_Z6prettyv"), "pretty()")
        fake_demangler.demangle.assert_called_once_with("_Z6prettyv")

    def test_demangler_round_trips_pipe_and_cleanup(self) -> None:
        class FakeStream:
            def __init__(self, line: bytes | None = None) -> None:
                self.line = line
                self.closed = False
                self.writes: list[bytes] = []
                self.flushed = False

            def write(self, data: bytes) -> None:
                self.writes.append(data)

            def flush(self) -> None:
                self.flushed = True

            def readline(self) -> bytes:
                assert self.line is not None
                return self.line

            def close(self) -> None:
                self.closed = True

        fake_pipe = mock.Mock()
        fake_pipe.stdin = FakeStream()
        fake_pipe.stdout = FakeStream(b"foo()\n")

        with mock.patch.object(lc.subprocess, "Popen", return_value=fake_pipe) as popen:
            demangler = lc.Demangler()
            self.assertEqual(demangler.demangle("_Z3foov"), "foo()")
            demangler.__del__()

        popen.assert_called_once_with(
            [lc.CPPFILT],
            stdin=lc.subprocess.PIPE,
            stdout=lc.subprocess.PIPE,
        )
        self.assertEqual(fake_pipe.stdin.writes, [b"_Z3foov\n"])
        self.assertTrue(fake_pipe.stdin.flushed)
        self.assertTrue(fake_pipe.stdin.closed)
        fake_pipe.terminate.assert_called_once_with()
        fake_pipe.wait.assert_called_once_with()


class MainEdgeTests(unittest.TestCase):
    def test_import_without_cppfilt_leaves_demangling_unavailable(self) -> None:
        script = REPO_ROOT / "tools" / "scripts" / "lcov_cobertura.py"

        with mock.patch("shutil.which", return_value=None):
            module_globals = runpy.run_path(str(script), run_name="lcov_cobertura_no_cppfilt")

        self.assertFalse(module_globals["HAVE_CPPFILT"])

    def test_script_entrypoint_runs_main(self) -> None:
        script = REPO_ROOT / "tools" / "scripts" / "lcov_cobertura.py"

        result = subprocess.run(
            [sys.executable, str(script), "--version"],
            check=True,
            capture_output=True,
            text=True,
        )

        self.assertIn("[lcov_cobertura 2.1.2]", result.stdout)

    def test_main_uses_sys_argv_for_version(self) -> None:
        stdout = io.StringIO()
        with mock.patch.object(sys, "argv", ["lcov_cobertura.py", "--version"]):
            with contextlib.redirect_stdout(stdout):
                with self.assertRaises(SystemExit) as ctx:
                    lc.main()

        self.assertEqual(ctx.exception.code, 0)
        self.assertIn("[lcov_cobertura 2.1.2]", stdout.getvalue())

    def test_main_rejects_missing_lcov_argument(self) -> None:
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            with self.assertRaises(SystemExit) as ctx:
                lc.main(["lcov_cobertura.py"])

        self.assertEqual(ctx.exception.code, 1)
        self.assertIn("Converts LCOV coverage data", stdout.getvalue())

    def test_main_rejects_demangle_when_filter_is_missing(self) -> None:
        with mock.patch.object(lc, "HAVE_CPPFILT", False):
            with self.assertRaisesRegex(RuntimeError, "C\\+\\+ filter executable"):
                lc.main(["lcov_cobertura.py", "--demangle", "coverage.lcov"])

    def test_main_reports_input_io_errors_without_writing_output(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            missing = Path(td) / "missing.lcov"
            output = Path(td) / "coverage.xml"
            stderr = io.StringIO()

            with contextlib.redirect_stderr(stderr):
                lc.main(["lcov_cobertura.py", str(missing), "-o", str(output)])

            self.assertFalse(output.exists())
            self.assertIn(f"Unable to convert {missing}", stderr.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
