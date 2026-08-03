#!/usr/bin/env python3
"""Read CSS gradient geometry off Chromium's own render.

This is where the expected numbers in test/test_css_gradient_geometry.cpp come
from. A gradient's geometry is arithmetic — radii, angles, endpoints — and a
fixture derived from the same formula as the implementation agrees with it by
construction, including when both are wrong. Chromium is an independent
implementation of the same spec, so it can disagree, which is the only reason
these tests can fail for a real cause.

Each case renders a HARD-EDGED gradient: one colour up to a boundary stop and a
different colour immediately past it, so the colour boundary traces the ending
shape (or the gradient line's half-way point) exactly, and a scanline scan
reads its position to the pixel. A soft gradient would only be comparable
through a full-image diff, which cannot say WHERE the geometry is wrong.

The box is 160x100 on purpose. A square box hides every defect worth testing:
it makes an ellipse a circle and makes a 45-degree angle its own reflection.

Usage:
    python3 tools/import-validation/chrome_gradient_oracle.py
    python3 tools/import-validation/chrome_gradient_oracle.py \\
        'radial-gradient(closest-side at 30% 60%, #ff0000 0%, #ff0000 99.9%, #0000ff 100%)'

With no argument it prints the full case list the committed tests assert. With
a CSS argument it prints the scan for that one string, which is how a new case
is added: render it here first, paste the numbers into the test.

Requires Chromium/Chrome. Set PULP_CHROME to override the binary path.
"""

import os
import subprocess
import sys
import tempfile

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow required (pip install Pillow)")

CHROME = os.environ.get(
    "PULP_CHROME", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")

W, H = 160, 100
ROWS = (0, 25, 50, 75, 99)
COLS = (0, 40, 80, 120, 159)

# The cases test_css_gradient_geometry.cpp asserts, in the same order.
CASES = [
    "radial-gradient(90% 70% at 50% 30%, #ff0000 0%, #ff0000 99.9%, #0000ff 100%)",
    "radial-gradient(closest-side at 30% 60%, #ff0000 0%, #ff0000 99.9%, #0000ff 100%)",
    "radial-gradient(farthest-side at 30% 60%, #ff0000 0%, #ff0000 99.9%, #0000ff 100%)",
    "radial-gradient(closest-corner at 30% 60%, #ff0000 0%, #ff0000 99.9%, #0000ff 100%)",
    # The corner keywords always cover the whole box, so their size is only
    # observable partway along the gradient line — hence the 50% boundary.
    "radial-gradient(farthest-corner at 25% 40%, #ff0000 0%, #ff0000 49.9%, #0000ff 50%)",
    "radial-gradient(#ff0000 0%, #ff0000 49.9%, #0000ff 50%)",
    "radial-gradient(circle farthest-corner at 25% 40%, #ff0000 0%, #ff0000 49.9%, #0000ff 50%)",
    "radial-gradient(circle 40px at 50% 50%, #ff0000 0%, #ff0000 99.9%, #0000ff 100%)",
    "linear-gradient(45deg, #ff0000 0%, #ff0000 49.9%, #0000ff 50%)",
    "linear-gradient(150deg, #ff0000 0%, #ff0000 49.9%, #0000ff 50%)",
    "linear-gradient(to bottom right, #ff0000 0%, #ff0000 49.9%, #0000ff 50%)",
]


def render(css, w=W, h=H):
    if not os.path.exists(CHROME):
        sys.exit(f"Chrome not found at {CHROME}; set PULP_CHROME")
    tmp = tempfile.mkdtemp(prefix="pulp-gradient-oracle-")
    page = os.path.join(tmp, "case.html")
    shot = os.path.join(tmp, "case.png")
    with open(page, "w") as fh:
        fh.write(
            "<!doctype html><meta charset=utf-8><style>\n"
            # Green backdrop: the two gradient colours are red and blue, so a
            # gradient that failed to paint shows as green rather than being
            # read as one box-filling shape. The stop colours are hex, not the
            # `red`/`blue` keywords, because Pulp's CSS colour parser has no
            # named-colour table and silently resolves an unknown name to
            # opaque WHITE — which would make both sides of the comparison
            # white and every scan read as empty.
            "html,body{margin:0;padding:0;background:#0f0}\n"
            f"#g{{position:absolute;left:0;top:0;width:{w}px;height:{h}px;"
            f"background:{css}}}\n"
            "</style><div id=g></div>")
    subprocess.run(
        [CHROME, "--headless", "--disable-gpu", "--no-sandbox",
         f"--screenshot={shot}", f"--window-size={w},{h}", "--hide-scrollbars",
         "--force-device-scale-factor=1", "file://" + page],
        capture_output=True, check=True)
    return Image.open(shot).convert("RGB")


def inside(px):
    """Inside is red, outside is blue; an antialiased boundary pixel is a blend
    of the two and falls on the side it is mostly on. Green is the backdrop and
    counts as neither — see `unpainted`."""
    return px[0] > px[2] and not unpainted(px)


def unpainted(px):
    """The backdrop showing through, i.e. the gradient did not paint here."""
    return px[1] > px[0] and px[1] > px[2]


def row_span(im, y):
    xs = [x for x in range(im.width) if inside(im.getpixel((x, y)))]
    return (xs[0], xs[-1]) if xs else None


def col_span(im, x):
    ys = [y for y in range(im.height) if inside(im.getpixel((x, y)))]
    return (ys[0], ys[-1]) if ys else None


def report(css):
    im = render(css)
    print(f"--- {css}")
    bare = sum(1 for y in range(im.height) for x in range(im.width)
               if unpainted(im.getpixel((x, y))))
    if bare:
        print(f"    !! {bare} backdrop pixels — Chrome did not paint this CSS "
              f"over the whole box; the spans below are not its geometry")
    for y in ROWS:
        print(f"    row y={y:3d} inside x: {row_span(im, y)}")
    for x in COLS:
        print(f"    col x={x:3d} inside y: {col_span(im, x)}")


def main():
    for css in (sys.argv[1:] or CASES):
        report(css)


if __name__ == "__main__":
    main()
