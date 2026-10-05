#!/usr/bin/env python3
"""Self-test for catch_test_name_guard.py."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import catch_test_name_guard as guard  # noqa: E402


def repo(files: dict[str, str]) -> tempfile.TemporaryDirectory:
    tmp = tempfile.TemporaryDirectory()
    root = Path(tmp.name)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    for name, text in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(text))
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    return tmp


class ProblemTests(unittest.TestCase):
    def test_each_spec_operator_is_named(self) -> None:
        for name in ("~View() clears", "exclude:x", "Exclude:x", "*suffix", "prefix *",
                     '"quoted', "-flag"):
            with self.subTest(name=name):
                self.assertIsNotNone(guard.problem(name))

    def test_ordinary_names_pass(self) -> None:
        for name in ("View destructor clears", "a ~ in the middle", "a * in the middle",
                     "OSC address wildcard ?", "x, y [tag-like]", "trailing - dash"):
            with self.subTest(name=name):
                self.assertIsNone(guard.problem(name))


class NameTests(unittest.TestCase):
    def test_adjacent_literals_join_and_every_macro_form_is_read(self) -> None:
        text = textwrap.dedent('''
            TEST_CASE("~View() "
                      "clears", "[view]") {}
            SCENARIO("plain") {}
            TEST_CASE_METHOD(Fixture, "*wild", "[x]") {}
            TEMPLATE_TEST_CASE("tmpl *", "[t]", int, float) {}
        ''')
        self.assertEqual([name for _, name in guard.names(text)],
                         ["~View() clears", "plain", "*wild", "tmpl *"])


class ScanTests(unittest.TestCase):
    def test_the_focused_input_shape_is_caught_and_its_rename_passes(self) -> None:
        bad = 'TEST_CASE("~View() clears focused_input_ if this holds it", "[view]") {}\n'
        with repo({"test/test_x.cpp": bad, "external/vendor/test.cpp": bad}) as td:
            found, seen = guard.scan(Path(td))
            # external/ is vendored code and is not scanned.
            self.assertEqual(seen, 1)
            self.assertEqual([(str(p), line) for p, line, _, _ in found],
                             [("test/test_x.cpp", 1)])
            self.assertEqual(guard.main(["--root", td]), 1)
        good = bad.replace("~View()", "View destructor")
        with repo({"test/test_x.cpp": good}) as td:
            self.assertEqual(guard.main(["--root", td]), 0)

    def test_the_skip_marker_exempts_its_line(self) -> None:
        text = ('TEST_CASE("~x", "[t]") {}  // catch-test-name-guard: skip escaped by hand\n'
                'TEST_CASE("~y", "[t]") {}\n')
        with repo({"test/test_x.cpp": text}) as td:
            found, _ = guard.scan(Path(td))
            self.assertEqual([name for _, _, name, _ in found], ["~y"])

    def test_no_names_is_an_instrument_failure_not_a_pass(self) -> None:
        with repo({"README.md": "nothing\n"}) as td:
            self.assertEqual(guard.main(["--root", td]), 2)

    def test_this_checkout_is_clean(self) -> None:
        found, seen = guard.scan(Path(__file__).resolve().parents[2])
        self.assertGreater(seen, 1000)
        self.assertEqual(found, [])


if __name__ == "__main__":
    unittest.main()
