#!/usr/bin/env python3
"""Read how Chromium composites backdrop-filter under opacity.

This is where the expected pixels in test/test_css_backdrop_opacity.cpp come
from. The fixture, test/fixtures/import-differential/22-backdrop-opacity.html,
lays five `backdrop-filter: blur(8px)` boxes over 1px gridlines: own opacity
0, 0.5 and 1, then a box at full opacity inside a group at opacity 0.5 and one
inside a group at opacity 0. A gridline pixel and a gap pixel under each box
say how much of the blurred backdrop and how much of the crisp page reached
the screen, which is the whole question: Chrome hides the backdrop result at
opacity 0, mixes it with the page at 0.5, and treats a group below opacity 1
as the backdrop's root, so the box under it blurs the group's own (empty)
content rather than the page.

Usage:
    python3 tools/import-validation/chrome_backdrop_opacity_oracle.py

It prints one C++ initializer per box, in the order the test lists them.

Requires Chromium/Chrome. Set PULP_CHROME to override the binary path.
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow required (pip install Pillow)")

CHROME = os.environ.get(
    "PULP_CHROME", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")
FIXTURE = (Path(__file__).resolve().parents[2]
           / "test/fixtures/import-differential/22-backdrop-opacity.html")

W, H = 512, 160
ROW = 80
# Each box is 80 px wide at y 40..120. Gridlines sit at x = 5 mod 10, so the
# sampled line and gap are 35 px into the box, clear of the blur's edge.
BOXES = [("opacity-0", 16), ("opacity-half", 112), ("opacity-1", 208),
         ("group-half", 304), ("group-0", 400)]


def sample_columns(left):
    line = next(x for x in range(left + 35, left + 80) if x % 10 == 5)
    return line, line - 5


def render():
    if not os.path.exists(CHROME):
        sys.exit(f"Chrome not found at {CHROME}; set PULP_CHROME")
    shot = os.path.join(tempfile.mkdtemp(prefix="pulp-backdrop-oracle-"), "case.png")
    result = subprocess.run(
        # MacAppCodeSignClone: Chrome otherwise copies its app bundle into the
        # per-user temp dir and leaves it behind whenever it does not exit cleanly.
        [CHROME, "--headless", "--disable-gpu", "--no-sandbox",
         "--disable-features=MacAppCodeSignClone",
         f"--screenshot={shot}", f"--window-size={W},{H}", "--hide-scrollbars",
         "--force-device-scale-factor=1", FIXTURE.as_uri()],
        capture_output=True)
    # A complete screenshot can come with a non-zero exit from the teardown
    # watchdog, so the PNG is the verdict, not the status.
    if not os.path.isfile(shot):
        tail = (result.stderr or b"").decode("utf-8", "replace").strip().splitlines()[-5:]
        sys.exit(f"Chrome (exit {result.returncode}) wrote no screenshot\n" + "\n".join(tail))
    with Image.open(shot) as im:
        rgb = im.convert("RGB")
    if rgb.size != (W, H):
        sys.exit(f"Chrome wrote a {rgb.width}x{rgb.height} screenshot, expected {W}x{H}")
    return rgb


def main():
    if len(sys.argv) > 1:
        print(__doc__)
        return 0 if sys.argv[1] in ("-h", "--help") else 2
    im = render()
    print(f"// Chrome, {FIXTURE.name}, row y={ROW}: box, line x, line rgb, gap x, gap rgb")
    for name, left in BOXES:
        line, gap = sample_columns(left)
        lr, lg, lb = im.getpixel((line, ROW))
        gr, gg, gb = im.getpixel((gap, ROW))
        print(f'{{"{name}", {line}, {{{lr}, {lg}, {lb}}}, {gap}, {{{gr}, {gg}, {gb}}}}},')
    return 0


if __name__ == "__main__":
    sys.exit(main())
