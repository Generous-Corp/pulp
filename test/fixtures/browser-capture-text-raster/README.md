# browser-capture-text-raster

Glyph rasterization against Chromium on macOS. The labels are the Spectr panel's
toolbar and header strings (JetBrains Mono, 9 to 11 px, letter-spaced). Two
of them are drawn right-to-left.

| File | What it is |
|---|---|
| `labels.json` | The label set. `text_raster_decomposition.py html` turns it into the Chromium page. |
| `chromium-antialiased.png` | Chrome for Testing 151 at DPR 2, `-webkit-font-smoothing: antialiased`. |
| `chromium-auto.png` | The same page with no smoothing property, which is `auto`. |
| `chromium-antialiased-offset.png` | The `antialiased` page translated by 0.25 CSS px. This is the metric-sensitivity control. |
| `chromium-antialiased.baselines.json` | Per-label left edge and baseline, read from the page. |
| `pulp-default.png` | The same labels drawn through `SkiaCanvas::fill_text` on a CPU raster surface at scale 2 with default `SkSurfaceProps`. This is today's macOS skia screenshot path. |
| `pulp-hinting-none-gamma-1.png` | The same draw with `SkFontHinting::kNone` and `SkSurfaceProps(0, kUnknown_SkPixelGeometry, 0, 1)`. |

What the renders show (`test_text_raster_decomposition.py` pins it):

- `pulp-default` matches `chromium-auto` pixel for pixel. Against `antialiased` it carries about 1.30x the ink.
- `pulp-hinting-none-gamma-1` matches `chromium-antialiased` exactly for left-to-right runs. Edging does not change ink on this surface.
- Right-to-left letter-spaced runs still sit half the letter-spacing to the right.

The font is `external/fonts/JetBrainsMono-Regular.ttf`, which is static. The
labels therefore stay at weight 400; a weight-600 request on a static face is
synthesized differently by the two engines.
