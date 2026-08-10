#!/usr/bin/env python3
"""Draw Forge Modular's own knobs, jacks, switches, sliders and lights.

Rack ships a component library and generated panels have been using it. Its
*code* is fine to build on -- that is what the plugin licence exception is for
-- but its **graphics are CC BY-NC 4.0**, and that quietly attaches a
non-commercial condition to the artwork of every module anyone builds with
Forge Modular. Someone selling a module outside the VCV Library would inherit
it without ever being told.

Every component turned out to be artwork, including the lights, which load
`res/ComponentLibrary/TinyLight.svg` rather than being drawn in code. So the
whole set is replaced here.

Rack's widget machinery is kept: `SvgKnob` knows how to rotate to a value,
`SvgPort` knows how a cable attaches, `SvgSlider` knows its travel. Only the
pictures change -- which is also why our modules will now be recognisable on
sight rather than looking like everybody else's.

Drawn in the Ink & Signal palette so a Forge Modular panel reads as one.

    components.py <out-dir>      # write the SVGs
"""
from __future__ import annotations

import os
import sys

# Ink & Signal, the same tokens the app uses.
BODY = "#232A35"        # component body, a step above the panel
BODY_HI = "#2E3745"     # lit edge
RIM = "#151A21"         # the shadow a real knob casts into its panel
ACCENT = "#16DAC2"      # indicator, and anything that shows a value
METAL = "#8E9AA8"       # jack rings and screw slots
HOLE = "#0C0F14"        # the dark of an actual socket
LIGHT_RING = "#1B222C"

PX_PER_MM = 75.0 / 25.4


def _svg(w_mm: float, h_mm: float, body: str) -> str:
    w, h = w_mm * PX_PER_MM, h_mm * PX_PER_MM
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w:.3f}" '
            f'height="{h:.3f}" viewBox="0 0 {w:.3f} {h:.3f}">\n{body}</svg>\n')


def knob(d_mm: float, tick: bool = True) -> str:
    """A knob, pointing at its minimum. Rack rotates the whole thing.

    The indicator runs from the centre outward rather than sitting as a dot on
    the rim, because at 45 px across a dot is three pixels and disappears --
    and a knob whose position cannot be read at a glance is the one thing a
    panel cannot afford.
    """
    r = d_mm * PX_PER_MM / 2
    c = r
    parts = [
        f'  <circle cx="{c:.2f}" cy="{c:.2f}" r="{r:.2f}" fill="{RIM}"/>',
        f'  <circle cx="{c:.2f}" cy="{c:.2f}" r="{r * 0.92:.2f}" fill="{BODY}"/>',
        f'  <circle cx="{c:.2f}" cy="{c * 0.94:.2f}" r="{r * 0.82:.2f}" '
        f'fill="{BODY_HI}" opacity="0.35"/>',
        f'  <circle cx="{c:.2f}" cy="{c:.2f}" r="{r * 0.74:.2f}" fill="{BODY}"/>',
    ]
    if tick:
        parts.append(
            f'  <rect x="{c - r * 0.055:.2f}" y="{c - r * 0.80:.2f}" '
            f'width="{r * 0.11:.2f}" height="{r * 0.52:.2f}" rx="{r * 0.055:.2f}" '
            f'fill="{ACCENT}"/>')
    return _svg(d_mm, d_mm, "\n".join(parts) + "\n")


def port() -> str:
    """A 3.5 mm jack: ring, socket, and the shadow that makes it read as a hole."""
    d = 8.2
    r = d * PX_PER_MM / 2
    c = r
    return _svg(d, d, "\n".join([
        f'  <circle cx="{c:.2f}" cy="{c:.2f}" r="{r:.2f}" fill="{RIM}"/>',
        f'  <circle cx="{c:.2f}" cy="{c:.2f}" r="{r * 0.88:.2f}" fill="{METAL}"/>',
        f'  <circle cx="{c:.2f}" cy="{c:.2f}" r="{r * 0.74:.2f}" fill="{BODY}"/>',
        f'  <circle cx="{c:.2f}" cy="{c:.2f}" r="{r * 0.44:.2f}" fill="{HOLE}"/>',
    ]) + "\n")


def screw() -> str:
    d = 3.2
    r = d * PX_PER_MM / 2
    c = r
    return _svg(d, d, "\n".join([
        f'  <circle cx="{c:.2f}" cy="{c:.2f}" r="{r:.2f}" fill="{RIM}"/>',
        f'  <circle cx="{c:.2f}" cy="{c:.2f}" r="{r * 0.82:.2f}" fill="{METAL}" '
        f'opacity="0.55"/>',
        f'  <rect x="{c - r * 0.62:.2f}" y="{c - r * 0.10:.2f}" '
        f'width="{r * 1.24:.2f}" height="{r * 0.20:.2f}" fill="{RIM}"/>',
    ]) + "\n")


def switch(pos: int, of: int) -> str:
    """One frame of a switch. Rack swaps frames as the value changes."""
    w, h = 3.4, 8.4 if of > 2 else 7.0
    W, H = w * PX_PER_MM, h * PX_PER_MM
    travel = H * 0.62
    top = (H - travel) / 2
    # Position 0 sits at the bottom, matching how a physical toggle reads.
    y = top + travel * (1 - pos / max(1, of - 1)) - H * 0.14
    return _svg(w, h, "\n".join([
        f'  <rect x="0" y="0" width="{W:.2f}" height="{H:.2f}" '
        f'rx="{W * 0.34:.2f}" fill="{RIM}"/>',
        f'  <rect x="{W * 0.14:.2f}" y="{H * 0.06:.2f}" width="{W * 0.72:.2f}" '
        f'height="{H * 0.88:.2f}" rx="{W * 0.28:.2f}" fill="{BODY}"/>',
        f'  <rect x="{W * 0.18:.2f}" y="{y:.2f}" width="{W * 0.64:.2f}" '
        f'height="{H * 0.28:.2f}" rx="{W * 0.26:.2f}" fill="{ACCENT}"/>',
    ]) + "\n")


def slider_bg() -> str:
    w, h = 5.0, 28.0
    W, H = w * PX_PER_MM, h * PX_PER_MM
    return _svg(w, h, "\n".join([
        f'  <rect x="{W * 0.36:.2f}" y="0" width="{W * 0.28:.2f}" '
        f'height="{H:.2f}" rx="{W * 0.14:.2f}" fill="{RIM}"/>',
    ]) + "\n")


def slider_handle() -> str:
    w, h = 5.0, 5.6
    W, H = w * PX_PER_MM, h * PX_PER_MM
    return _svg(w, h, "\n".join([
        f'  <rect x="0" y="0" width="{W:.2f}" height="{H:.2f}" '
        f'rx="{H * 0.28:.2f}" fill="{RIM}"/>',
        f'  <rect x="{W * 0.08:.2f}" y="{H * 0.10:.2f}" width="{W * 0.84:.2f}" '
        f'height="{H * 0.80:.2f}" rx="{H * 0.24:.2f}" fill="{BODY}"/>',
        f'  <rect x="{W * 0.20:.2f}" y="{H * 0.44:.2f}" width="{W * 0.60:.2f}" '
        f'height="{H * 0.12:.2f}" rx="{H * 0.06:.2f}" fill="{ACCENT}"/>',
    ]) + "\n")


def light(d_mm: float) -> str:
    """A light. Rack tints the white part; the ring is the unlit surround."""
    r = d_mm * PX_PER_MM / 2
    c = r
    return _svg(d_mm, d_mm, "\n".join([
        f'  <circle cx="{c:.2f}" cy="{c:.2f}" r="{r:.2f}" fill="{LIGHT_RING}"/>',
        f'  <circle cx="{c:.2f}" cy="{c:.2f}" r="{r * 0.72:.2f}" fill="#FFFFFF"/>',
    ]) + "\n")


# Diameters chosen to match the footprints the layout validator already
# enforces, so replacing the art cannot silently change what fits on a panel.
SET = {
    "knob-large.svg": lambda: knob(18.3),
    "knob.svg": lambda: knob(12.2),
    "knob-small.svg": lambda: knob(8.64),
    "trimpot.svg": lambda: knob(5.76, tick=True),
    "port.svg": port,
    "screw.svg": screw,
    "toggle-0.svg": lambda: switch(0, 2),
    "toggle-1.svg": lambda: switch(1, 2),
    "switch3-0.svg": lambda: switch(0, 3),
    "switch3-1.svg": lambda: switch(1, 3),
    "switch3-2.svg": lambda: switch(2, 3),
    "slider-bg.svg": slider_bg,
    "slider-handle.svg": slider_handle,
    "light-tiny.svg": lambda: light(1.6),
    "light-small.svg": lambda: light(2.4),
    "light-medium.svg": lambda: light(3.2),
    "light-large.svg": lambda: light(4.6),
}


def write(out_dir: str) -> list:
    os.makedirs(out_dir, exist_ok=True)
    written = []
    for name, fn in SET.items():
        path = os.path.join(out_dir, name)
        with open(path, "w") as f:
            f.write(fn())
        written.append(path)
    return written


def main(argv):
    out = argv[1] if len(argv) > 1 else "examples/forge-modular/res/components"
    paths = write(out)
    print(f"{len(paths)} components -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
