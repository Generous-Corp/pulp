#!/usr/bin/env python3
"""Self-test for check_test_file_paths.py.

Two directions are load-bearing. Every path form the lint names must be
reported, or a rewrite that slips past one pattern lands the defect again.
And a label use of __FILE__ (a registry entry, a provenance field, a log
argument) must pass, or the gate reports code that never opens anything.
The command-line run also proves a finding fails the gate, a clean file
passes it, and a scan that matches no file refuses to report a clean result.
"""
from __future__ import annotations

import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import unittest

LINT = pathlib.Path(__file__).with_name("check_test_file_paths.py")
_spec = importlib.util.spec_from_file_location("check_test_file_paths", LINT)
lint = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(lint)

PATH_USES = [
    'auto root = fs::path(__FILE__).parent_path().parent_path();',
    'return (std::filesystem::path(__FILE__).parent_path() / "fixtures/x").string();',
    'std::filesystem::path here{__FILE__};',
    'fs::path here = __FILE__;',
    'std::vector<fs::path> seeds = { std::filesystem::path(__FILE__), };',
    'auto source_file = std::filesystem::path(__FILE__);',
    'std::string self(__FILE__); auto dir = self.substr(0, self.rfind(\'/\'));',
    'std::ifstream in(std::string(__FILE__) + ".json");',
    'FILE* f = fopen(__FILE__, "rb");',
    'const char* fixture = __FILE__ "/../fixtures/a.wav";',
    'auto dir = std::string_view{__FILE__}.substr(0, 10);',
]

LABEL_USES = [
    'registry.register_constant("test_cutoff", __FILE__, __LINE__, 0.5f);',
    'prov.source_file = __FILE__;',
    'INFO("at " << __FILE__ << ":" << __LINE__);',
    '// fs::path(__FILE__).parent_path() is what this used to do',
    'auto root = fs::path(__FILE__).parent_path();  // file-path-lint: allow reason given',
]


class Findings(unittest.TestCase):
    def test_every_path_form_is_reported(self):
        for line in PATH_USES:
            with self.subTest(line=line):
                self.assertEqual(len(lint.findings(line)), 1, line)

    def test_label_uses_and_escapes_pass(self):
        for line in LABEL_USES:
            with self.subTest(line=line):
                self.assertEqual(lint.findings(line), [], line)

    def test_findings_carry_the_line_number(self):
        text = "int a;\n" + PATH_USES[0] + "\n"
        self.assertEqual([number for number, _ in lint.findings(text)], [2])


class CommandLine(unittest.TestCase):
    def run_lint(self, root: pathlib.Path, *files: str) -> subprocess.CompletedProcess:
        args = [sys.executable, str(LINT), "--root", str(root)]
        for name in files:
            args += ["--file", name]
        return subprocess.run(args, capture_output=True, text=True)

    def test_a_finding_fails_and_a_clean_file_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "bad.cpp").write_text(PATH_USES[0] + "\n")
            (root / "good.cpp").write_text(LABEL_USES[0] + "\n")
            bad = self.run_lint(root, "bad.cpp")
            self.assertEqual(bad.returncode, 1, bad.stdout + bad.stderr)
            self.assertIn("bad.cpp:1: __FILE__ used as a path", bad.stdout)
            good = self.run_lint(root, "good.cpp")
            self.assertEqual(good.returncode, 0, good.stdout + good.stderr)

    def test_a_scan_that_matches_nothing_refuses_a_clean_result(self):
        with tempfile.TemporaryDirectory() as tmp:
            empty = self.run_lint(pathlib.Path(tmp))
            self.assertEqual(empty.returncode, 2, empty.stdout + empty.stderr)
            self.assertIn("proves nothing", empty.stderr)


if __name__ == "__main__":
    unittest.main()
