# Forge Modular — panel + component SVGs (Ink & Signal)

Two directions, both themes. Generated to the brief's measured numbers.

```
dir-a/  "Plate"  raised header band, hairline sections, concentric knob, teal output squares
dir-b/  "Score"  full-bleed 1mm teal rule, raised full-width I/O field, wedge knob, light output squares
  dark/ | light/
    components/         px canvases at Rack widget sizes — SHIPS
    panels/             mm plates, labels outlined to stroked paths — SHIPS
    panels-jost-text/   same plates, labels as live <text> font-family="Jost" — for the Skia converter
    mockups/            plate + widgets composited — REVIEW ONLY, do not ship
```

## Pixel-alignment guarantees

Screws, accent rule, plate top and plate bottom are at identical y on every width; module names share one cap height and one baseline.

Every plate is byte-identical in vertical geometry regardless of HP, and two things were changed so that survives rasterization:

- **Full-bleed geometry overdraws the viewBox.** The plate rect, dir-a's header band and both directions' accent rules are drawn from `x="-1" y="-1"` to 1 mm past each edge; the viewBox clips them. Without this, an edge sitting exactly on the viewBox boundary antialiases into partial transparency by a width-dependent sub-pixel amount, and narrow plates visibly fail to reach the top. Verified: top and bottom row alpha = 255 on all six widths.
- **dir-a's accent rule is 0.6 mm, not 0.35 mm.** At 75 dpi 0.35 mm is ~1 px, thin enough that resampling phase shifts it a full pixel between widths. 0.6 mm ≈ 1.8 px and lands on the same rows at every width.

`_review/` holds copies with integer px root sizes for on-screen comparison only — an mm-sized SVG has a fractional intrinsic size (15.24 mm = 57.6 px), and browsers rasterize at that fractional size before resampling, which reintroduces width-dependent phase. Rack rasterizes from the mm files directly at one scale per zoom level, so this only affects HTML review. **Ship from `panels/`, never `_review/`.**

## Mechanical receipt (§7)

- `<text>` in `panels/`, `components/`, `mockups/`: **0**
- `<filter>`, `<mask>`, `<clipPath>`, `<image>`, `<style>`, blend modes, group opacity: **0**
- Gradients: **0** — every fill is flat
- Panel height: `128.6933mm` exactly, viewBox `0 0 W 128.6933`
- Panel width: `hp × 5.08mm` exactly (3 → 15.24, 6 → 30.48, 8 → 40.64, 12 → 60.96)
- Screw recess rings at Rack's own coordinates: x = 7.62 and W−7.62 mm (single centred ring at 3 HP), y = 2.54 and 126.153 mm
- Accent rule y: dir-a `13.0`–`13.6` mm (under a 13.6 mm header band), dir-b `13.4`–`14.4` mm — identical on all six panels
- Component canvases: 54, 36, 25.5, 17, 24.3, 15 px squares; toggle 10 × 20.6; LED 8.2
- Two constructs to know about: `<circle>`/`<rect rx>` and elliptical `A` arcs (knob arc tracks only). nanosvg handles both — flagged so your rasterizer diff has no surprises.

## Lettering

Labels are a hand-built mono-line geometric alphabet: caps on a 62 × 100 unit box, `M`/`L`/`C` only, emitted as stroked paths at `stroke-width = 0.16 × cap-height`, round caps and joins. Not Jost — Jost-adjacent, and chosen because at 1.5–2.2 mm cap height a mono-line stroke resolves better at 100 % zoom than an outlined Jost fill does.

The alphabet source is `_gen/glyphs.json`. If you'd rather ship real Jost, `panels-jost-text/` has the identical plates with `<text ... font-family="Jost" font-weight="500">` for your text→path pass; nothing else differs.

## Clearance rules

Two rules, enforced by an audit pass over every label on every panel:

1. **No label sits closer than 1.6 mm to any widget** it does not label. Measured box-to-circle, not centre-to-centre. Tightest in the set is now 1.6 mm.
2. **Controls of different sizes on the same row share a centre line and a label baseline.** A small knob beside a trimpot at their own y values reads as broken even when each is individually correct — the LFO's WAVE/FM row and the SEQ's CLK/RST/LEN row are both built this way (row centre, then one baseline set from the *largest* control in the row plus clearance). Mixed-size rows are also spaced on cell centres (W/4, 3W/4) rather than eyeballed x values.

## Label scale

Module names are a single cap height across the whole family. Scaling the name with panel width (the obvious move) makes narrow modules read as sitting lower and smaller than wide ones in a rack row, because a shared baseline plus a smaller cap puts the letter tops in a different place on every module. One size fixes the whole family optically. `MULT` at 3.2 mm is 10.24 mm wide, which fits a 15.24 mm plate — that's the constraint that sets the size, so **names are capped at 4 characters at 3 HP**.

| use | cap height | where |
|---|---|---|
| module name | 3.2 mm — **one size at every HP** | header |
| primary control / jack | 1.8–2.2 mm | CUTOFF, RES, CV, LP |
| secondary | 1.5–1.7 mm | PEAK, SYNC, LEN — dropped entirely in dir-b |

dir-b multiplies primary caps by 1.22 and omits every secondary label.

## Input vs output

Carried by the **plate**, not the jack: an output is a 10.6 mm rounded square filled behind the socket, an input is bare plate. Shape difference first, colour second — it survives greyscale and a dark room. dir-a fills teal (`#16DAC2` dark / `#0A9E8B` light), dir-b fills light (`#D6DCE4` dark / `#1E2530` light).

The CV pair on the VCF is one unit: the trimpot and its jack sit inside a shared 14 × 23 mm container (hairline outline in dir-a, raised fill in dir-b) with the `CV` label between them.

## Trimpots

Indicator is a **radius pointer from centre to rim**, not a screwdriver slot. A slot is a diameter, so it reads the same at a setting and its opposite — you cannot tell which end is the value. Same reason the knobs use a pointer.

## Knobs

Indicator is a **pointer line from centre to rim** in both directions — the Rack convention (Rogan, Davies, Trimpot all use a printed pointer or notch). No wedge: a filled wedge reads as a *range* rather than a position, and nothing in stock Rack draws one.

Rack draws its own modulation indicator over a param when CV is patched to it, so don't bake a modulation arc into the panel or the knob. Component SVGs contain the **inset arc track only** (`#2C333E` dark / `#CBD1D8` light, 270° from 135°) plus body and pointer. The teal value arc you see in `mockups/` is what Rack draws on top at runtime — don't bake it in.

## Palette

Verbatim from `forge/ui/theme.json` for dark. Light is derived, not inverted: plate `#EDEEF0`, raised `#FFFFFF`, well `#DBDFE4`, border `#B4BCC6`, ink `#161A21`, secondary ink `#333B45`, teal `#0A9E8B`, screw ring `#C6CCD3`.
