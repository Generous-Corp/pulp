#!/usr/bin/env python3
"""Build the opsz/trak test font: a self-authored, deterministic fixture.

One glyph, `H`, a plain box. Two typographic behaviours the browser applies
and a native renderer has to reproduce, each with an analytic answer:

* an `opsz` variation axis (8..72, default 12) that widens `H` from 600 to 800
  font units between the default and the axis maximum, so a renderer that
  honours `font-optical-sizing: auto` measures a size-dependent advance;
* an AAT `trak` table whose normal track is -100 units at 10 px and +100 at
  20 px, so tracking is known at, between and beyond the table's sizes.

Units per em is 1000, which keeps every expected value a short decimal.
Regenerate with: python3 make_opsz_trak_font.py (requires fontTools).
"""
from pathlib import Path

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import newTable
from fontTools.ttLib.tables.TupleVariation import TupleVariation
from fontTools.ttLib.tables._t_r_a_k import TrackData, TrackTableEntry

OUT = Path(__file__).with_name("pulp-test-opsz-trak.ttf")


def box(width):
    pen = TTGlyphPen(None)
    pen.moveTo((50, 0))
    pen.lineTo((50, 700))
    pen.lineTo((width - 50, 700))
    pen.lineTo((width - 50, 0))
    pen.closePath()
    return pen.glyph()


def empty():
    return TTGlyphPen(None).glyph()


fb = FontBuilder(unitsPerEm=1000, isTTF=True)
fb.setupGlyphOrder([".notdef", "space", "H"])
fb.setupCharacterMap({0x20: "space", 0x48: "H"})
fb.setupGlyf({".notdef": empty(), "space": empty(), "H": box(600)})
fb.setupHorizontalMetrics({".notdef": (500, 0), "space": (250, 0), "H": (600, 50)})
fb.setupHorizontalHeader(ascent=800, descent=-200)
fb.setupNameTable({"familyName": "Pulp Test Opsz Trak", "styleName": "Regular"})
fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
fb.setupPost()
fb.setupFvar(axes=[("opsz", 8, 12, 72, "Optical Size")], instances=[])
# At opsz 72 the box's right edge and the advance (phantom point 2) move
# +200 units; contour points: (50,0) (50,700) (550,700) (550,0).
fb.setupGvar({
    "H": [TupleVariation({"opsz": (0.0, 1.0, 1.0)},
                         [(0, 0), (0, 0), (200, 0), (200, 0),
                          (0, 0), (200, 0), (0, 0), (0, 0)])],
})

trak = newTable("trak")
trak.version = 1.0
trak.format = 0
horiz = TrackData()
horiz[0.0] = TrackTableEntry({10.0: -100, 20.0: 100}, nameIndex=256)
trak.horizData = horiz
trak.vertData = TrackData()
fb.font["trak"] = trak

fb.save(str(OUT))
print(f"wrote {OUT}")
