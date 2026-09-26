#!/usr/bin/env python3
"""Cover the status-tone signal lint in both directions.

A lint that only ever returns clean is indistinguishable from a lint that
cannot see anything, so every assertion here has a counterpart: something that
must be caught, and something that must not be.
"""

from __future__ import annotations

import importlib.util
import pathlib
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    "status_tone_signal_lint",
    pathlib.Path(__file__).with_name("status_tone_signal_lint.py"),
)
assert SPEC is not None and SPEC.loader is not None
lint = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lint)


class LintTest(unittest.TestCase):
    def _scan(self, name: str, text: str) -> list:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            src = root / "core" / "view" / "src"
            src.mkdir(parents=True)
            (src / name).write_text(text, encoding="utf-8")
            return lint.violations(root)

    def test_colour_from_tone_without_a_mark_is_caught(self):
        found = self._scan(
            "banner.cpp",
            "Color c() { return resolve_color(tone_token(tone_)); }\n")
        self.assertEqual([str(p) for p, _l in found],
                         ["core/view/src/banner.cpp"])

    def test_colour_from_tone_with_a_mark_is_clean(self):
        """The control. Without it the check above could be firing on any file
        that merely mentions a tone, which would make it useless noise."""
        self.assertEqual(
            self._scan(
                "banner.cpp",
                "Color c() { return resolve_color(tone_token(tone_)); }\n"
                "void p() { paint_tone_glyph(canvas, tone_, 1, 2, 3); }\n"),
            [])

    def test_the_mark_may_live_in_another_function(self):
        # The helper that resolves the colour and the paint that draws the mark
        # are normally separate, so the signal is looked for file-wide.
        self.assertEqual(
            self._scan(
                "widgets.cpp",
                "namespace { const char* tone_token(Tone t); }\n"
                "void A::paint() { paint_tone_glyph(c, tone_, 0, 0, 8); }\n"
                "Color B::fill() { return resolve_color(tone_token(tone_)); }\n"),
            [])

    def test_a_file_that_never_colours_by_tone_is_ignored(self):
        self.assertEqual(
            self._scan("plain.cpp", "void p() { canvas.fill_rect(0,0,1,1); }\n"),
            [])

    def test_a_skip_marker_escapes_one_line(self):
        self.assertEqual(
            self._scan(
                "legacy.cpp",
                "Color c() { return resolve_color(tone_token(t)); }"
                "  // status-tone-signal-lint: skip text names the state\n"),
            [])

    def test_a_commented_call_does_not_count(self):
        self.assertEqual(
            self._scan("doc.cpp", "// resolve_color(tone_token(tone_)) is the old way\n"),
            [])

    def test_only_source_suffixes_are_scanned(self):
        # A header declaring the helper is not a paint path.
        self.assertEqual(
            self._scan("decl.hpp", "const char* tone_token(Tone t);\n"),
            [])

    def test_main_exit_codes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            src = root / "core" / "view" / "src"
            src.mkdir(parents=True)
            (src / "ok.cpp").write_text("void p() {}\n", encoding="utf-8")
            self.assertEqual(lint.main(["--root", str(root)]), 0)
            (src / "bad.cpp").write_text(
                "Color c() { return tone_token(t); }\n", encoding="utf-8")
            self.assertEqual(lint.main(["--root", str(root)]), 1)


if __name__ == "__main__":
    unittest.main()
