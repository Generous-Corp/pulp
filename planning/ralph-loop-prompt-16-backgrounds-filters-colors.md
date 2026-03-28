"Implement CSS Backgrounds L3 + Filter Effects L1 + Color L4 for Pulp.

References:
- https://www.w3.org/TR/css-backgrounds-3/
- https://www.w3.org/TR/filter-effects-1/
- https://www.w3.org/TR/css-color-4/
Tracking: planning/w3c-css-support-matrix.md
GitHub Issues: #26 (Filters), #29 (Backgrounds), #30 (Color)

BACKGROUNDS TO IMPLEMENT:
1. background: linear-gradient() as view background (not just canvas)
2. background: radial-gradient() as view background
3. Per-corner border-radius (border-top-left-radius etc)
4. border-style: dashed, dotted
5. outline / outline-offset
6. Multiple backgrounds (layered)
7. background-image: url() (requires image loading)

FILTERS TO IMPLEMENT:
1. filter: blur(px) — per-element Gaussian blur via SkImageFilters
2. filter: brightness(%) — color matrix
3. filter: contrast(%) — color matrix
4. filter: saturate(%) — color matrix
5. filter: grayscale(%) — color matrix
6. filter: hue-rotate(deg) — color matrix rotation
7. filter: invert(%) — color matrix
8. filter: sepia(%) — color matrix
9. filter: drop-shadow() — shadow filter
10. Multiple filter composition: 'blur(4px) brightness(1.2)'
11. backdrop-filter (already partial — extend)

COLOR PARSING TO IMPLEMENT:
1. rgb(r, g, b) / rgba(r, g, b, a) parsing in bridge
2. hsl(h, s, l) / hsla(h, s, l, a) parsing
3. Named colors (148 CSS named colors lookup table)
4. transparent keyword
5. currentColor keyword (inherit from parent text color)
6. color-mix() function (blend two colors)

ARCHITECTURE:
- Backgrounds: add gradient_shader_ to View, render in paint_all before children
- Filters: apply SkImageFilters chain per-view via saveLayer in paint_all
- Colors: expand parseHexColor in bridge to parseColor accepting any CSS format
- Per-corner radius: change corner_radius_ from float to {tl, tr, br, bl}

JS BRIDGE:
- setBackgroundGradient(id, 'linear-gradient(to right, #ff0000, #0000ff)')
- setFilter(id, 'blur(4px) brightness(1.2)')
- setBorderRadius(id, tl, tr, br, bl)
- All color params accept: hex, rgb(), hsl(), named, transparent

TESTING:
- Color parsing: unit tests for every format
- Gradient: screenshot verification
- Filters: screenshot verification
- Per-corner radius: visual test

EACH ITERATION: build, test, screenshot, commit.
COMPLETION: Output 'BACKGROUNDS FILTERS COLORS COMPLETE'" --completion-promise "BACKGROUNDS FILTERS COLORS COMPLETE" --max-iterations 25
