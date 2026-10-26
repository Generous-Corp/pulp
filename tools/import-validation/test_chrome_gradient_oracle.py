#!/usr/bin/env python3
"""Unit tests for chrome_gradient_oracle.py's screenshot verdict.

A fake Chrome stands in for the real binary, so these run without a browser
and pin the one decision that matters: success is a valid PNG of the requested
size, never Chrome's exit status.
"""

from __future__ import annotations

import importlib.util
import os
import stat
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

try:
    import PIL  # noqa: F401
except ImportError:
    print("SKIP: Pillow not installed")
    sys.exit(77)

SCRIPT = Path(__file__).with_name("chrome_gradient_oracle.py")
SPEC = importlib.util.spec_from_file_location("chrome_gradient_oracle", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
oracle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(oracle)

# Each fake reads the same --screenshot / --window-size flags Chrome does.
# `mode` selects what it leaves behind and how it exits.
FAKE_CHROME = textwrap.dedent("""\
    #!{python}
    import sys
    from PIL import Image
    args = sys.argv[1:]
    shot = next(a.split("=", 1)[1] for a in args if a.startswith("--screenshot="))
    w, h = (int(v) for v in next(
        a.split("=", 1)[1] for a in args if a.startswith("--window-size=")).split(","))
    mode = {mode!r}
    if mode == "png-then-exit-2":
        Image.new("RGB", (w, h), (255, 0, 0)).save(shot)
        sys.stderr.write("Teardown watchdog expired; recording dump and terminating.\\n")
        sys.exit(2)
    if mode == "nothing":
        sys.exit(0)
    if mode == "wrong-size":
        Image.new("RGB", (w + 1, h), (255, 0, 0)).save(shot)
        sys.exit(0)
    if mode == "garbage":
        open(shot, "wb").write(b"not a png")
        sys.exit(0)
    sys.exit(f"unknown mode {{mode}}")
    """)


class ScreenshotVerdictTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self._saved_chrome = oracle.CHROME
        self.addCleanup(setattr, oracle, "CHROME", self._saved_chrome)

    def use_fake(self, mode):
        path = Path(self._tmp.name) / f"chrome-{mode}"
        path.write_text(FAKE_CHROME.format(python=sys.executable, mode=mode), encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        oracle.CHROME = str(path)

    def test_png_written_then_nonzero_exit_is_success(self):
        self.use_fake("png-then-exit-2")
        im = oracle.render("linear-gradient(#ff0000, #0000ff)", w=40, h=30)
        self.assertEqual(im.size, (40, 30))
        self.assertEqual(im.getpixel((0, 0)), (255, 0, 0))

    def test_no_png_fails_loudly(self):
        self.use_fake("nothing")
        with self.assertRaises(SystemExit) as ctx:
            oracle.render("linear-gradient(#ff0000, #0000ff)", w=40, h=30)
        self.assertIn("wrote no screenshot", str(ctx.exception.code))

    def test_wrong_size_fails(self):
        self.use_fake("wrong-size")
        with self.assertRaises(SystemExit) as ctx:
            oracle.render("linear-gradient(#ff0000, #0000ff)", w=40, h=30)
        self.assertIn("expected 40x30", str(ctx.exception.code))

    def test_unreadable_png_fails(self):
        self.use_fake("garbage")
        with self.assertRaises(SystemExit) as ctx:
            oracle.render("linear-gradient(#ff0000, #0000ff)", w=40, h=30)
        self.assertIn("unreadable screenshot", str(ctx.exception.code))


class ColumnMeanTest(unittest.TestCase):
    """--colors reads a column MEAN, so Chrome's dither cancels: rows that
    alternate one code value either side of the true colour average to it."""

    def test_dithered_column_averages_to_its_colour(self):
        from PIL import Image
        im = Image.new("RGB", (3, 4))
        for y in range(4):
            d = 1 if y % 2 else -1
            im.putpixel((0, y), (100 + d, 50, 0))
            im.putpixel((1, y), (10, 20 + d, 30 - d))
            im.putpixel((2, y), (0, 0, 255))
        self.assertEqual(oracle.column_means(im, cols=(0, 1, 2)),
                         [(100.0, 50.0, 0.0), (10.0, 20.0, 30.0), (0.0, 0.0, 255.0)])

    def test_midpoint_column_is_half_way(self):
        # The geometry the colour cases rely on: in a COLOR_W box the middle
        # sampled column's pixel centre sits at exactly t = 0.5.
        mid = oracle.COLOR_COLS[len(oracle.COLOR_COLS) // 2]
        self.assertEqual((mid + 0.5) / oracle.COLOR_W, 0.5)


if __name__ == "__main__":
    unittest.main()
