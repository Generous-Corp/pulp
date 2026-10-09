#!/usr/bin/env python3
"""Positive and negative controls for text_encoding_lint.py."""

from __future__ import annotations

import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SPEC = importlib.util.spec_from_file_location("text_encoding_lint", HERE / "text_encoding_lint.py")
assert SPEC and SPEC.loader
lint = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lint)

FLAGGED = '''import pathlib, subprocess
p = pathlib.Path("x")
a = p.read_text()
p.write_text("y")
b = open("f").read()
c = open("f", "w")
d = p.open()
e = subprocess.run(["git"], text=True)
f = subprocess.check_output(["git"], universal_newlines=True)
g = open("f", mode)
'''

CLEAN = '''import os, pathlib, subprocess
p = pathlib.Path("x")
a = p.read_text(encoding="utf-8")
b = open("f", "rb").read()
c = open("f", "w", encoding="utf-8")
d = p.open("rb")
e = subprocess.run(["git"], text=True, encoding="utf-8")
f = subprocess.run(["git"], capture_output=True)
g = os.open("f", os.O_RDONLY)
'''


def lint_tree(files: dict[str, str], baseline: dict[str, int] | None, *args: str) -> int:
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        for rel, text in files.items():
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_text(text, encoding="utf-8")
        if baseline is not None:
            (root / lint.BASELINE).parent.mkdir(parents=True, exist_ok=True)
            (root / lint.BASELINE).write_text(
                json.dumps({"schema": 1, "files": baseline}), encoding="utf-8")
        return lint.main(["--root", str(root), *args], list_files=lambda _r: sorted(files))


class DetectionTests(unittest.TestCase):
    def test_every_text_io_form_without_encoding_is_flagged(self) -> None:
        lines = [line for line, _ in lint.scan_text(FLAGGED)]
        self.assertEqual(lines, [3, 4, 5, 6, 7, 8, 9, 10])

    def test_binary_modes_explicit_encodings_and_non_file_opens_are_clean(self) -> None:
        self.assertEqual(lint.scan_text(CLEAN), [])

    def test_a_positional_encoding_or_a_same_named_helper_is_clean(self) -> None:
        source = (
            'import pathlib\n'
            'p = pathlib.Path("x")\n'
            'a = p.read_text("utf-8")\n'
            'p.write_text("y", "utf-8")\n'
            'b = families.read_text(root, rel)\n'
        )
        self.assertEqual(lint.scan_text(source), [])
        fixed, count = lint.fix_text(source)
        self.assertEqual((fixed, count), (source, 0))


class FixTests(unittest.TestCase):
    def test_fix_inserts_encoding_and_leaves_unknown_modes(self) -> None:
        fixed, count = lint.fix_text(FLAGGED)
        self.assertEqual(count, 7)
        self.assertEqual([line for line, _ in lint.scan_text(fixed)], [10])
        self.assertIn('p.read_text(encoding="utf-8")', fixed)
        self.assertIn('open("f", "w", encoding="utf-8")', fixed)

    def test_fix_handles_trailing_commas_and_multiline_calls(self) -> None:
        source = 'x = open(\n    "f",\n)\ny = subprocess.run(\n    ["a"],\n    text=True\n)\n'
        fixed, count = lint.fix_text("import subprocess\n" + source)
        self.assertEqual(count, 2)
        self.assertEqual(lint.scan_text(fixed), [])
        compile(fixed, "<fixed>", "exec")


class RatchetTests(unittest.TestCase):
    def test_a_file_at_its_baseline_passes(self) -> None:
        self.assertEqual(lint_tree({"tools/a.py": FLAGGED}, {"tools/a.py": 8}), 0)

    def test_a_file_over_its_baseline_fails(self) -> None:
        self.assertEqual(lint_tree({"tools/a.py": FLAGGED}, {"tools/a.py": 7}), 1)

    def test_a_new_file_must_be_clean(self) -> None:
        self.assertEqual(lint_tree({"tools/a.py": CLEAN, "tools/b.py": FLAGGED},
                                   {}), 1)

    def test_a_count_that_fell_must_be_recorded(self) -> None:
        self.assertEqual(lint_tree({"tools/a.py": CLEAN}, {"tools/a.py": 3}), 1)

    def test_write_refuses_to_raise_the_baseline(self) -> None:
        self.assertEqual(lint_tree({"tools/a.py": FLAGGED}, {"tools/a.py": 2}, "--write"), 1)

    def test_an_empty_tree_is_a_scan_error_not_a_pass(self) -> None:
        self.assertEqual(lint_tree({}, {}), 2)

    def test_raising_the_committed_baseline_against_the_base_is_refused(self) -> None:
        self.assertEqual(lint.baseline_raises({"a.py": 2}, {"a.py": 3}), ["a.py: 2 -> 3"])
        self.assertEqual(lint.baseline_raises({"a.py": 2}, {"b.py": 1}), ["b.py: 0 -> 1"])
        self.assertEqual(lint.baseline_raises({"a.py": 2}, {"a.py": 1}), [])


class RepositoryTests(unittest.TestCase):
    def test_the_committed_baseline_matches_the_tree(self) -> None:
        self.assertEqual(lint.main(["--root", str(ROOT)]), 0)

    def test_the_widening_classifier_treats_the_baseline_as_inert(self) -> None:
        # The baseline names almost every tools/ Python file; if the wide
        # non-native classifier saw it as a referrer, no tools/ change could
        # ever skip the native gate.
        spec = importlib.util.spec_from_file_location("wide_non_native", HERE / "wide_non_native.py")
        assert spec and spec.loader
        wide = importlib.util.module_from_spec(spec)
        sys.modules.setdefault("wide_non_native", wide)  # its dataclasses look themselves up
        spec.loader.exec_module(wide)
        self.assertEqual(wide.TEXT_ENCODING_BASELINE, lint.BASELINE.as_posix())
        self.assertIn(wide.TEXT_ENCODING_BASELINE, wide.INERT_EXACT)

    def test_a_tool_that_prints_marks_survives_a_cp1252_pipe(self) -> None:
        # A Windows pipe defaults to the ANSI code page; PYTHONIOENCODING
        # reproduces it here. The tool must reconfigure its own stdout.
        env = dict(os.environ, PYTHONIOENCODING="cp1252")
        result = subprocess.run(
            [sys.executable, str(HERE / "check_bundled_font_lists.py")],
            cwd=ROOT, env=env, capture_output=True, timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
        self.assertNotIn(b"UnicodeEncodeError", result.stderr)


if __name__ == "__main__":
    unittest.main()
