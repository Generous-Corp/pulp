# How the patch preview actually gets drawn

Answering "unclear how these will be rendered" — everything here is measured
on a real machine, not proposed.

## What we cannot do

- **No live Rack embedded in our window.** Rack is a separate application.
- **No screenshot of an assembled patch.** Rack has no "capture this patch"
  facility. Its `--screenshot` flag captures *each installed module
  individually*, not a rack.
- **No port positions for a module nobody has ever placed.** Port coordinates
  live only in each module's compiled code (see below).

## What we can do, and have

**1. A real image of every installed module.** Rack's `--screenshot` writes one
PNG per module. Verified: `Fundamental/VCO.png` is **135 × 380 px** — its exact
panel size — and it is genuine artwork, knobs, jacks, silkscreen and all,
because Rack drew it. 51 modules captured in one pass.

**2. Real port data.** A small module of ours walks the running rack and
records, for every module present: panel size, and per port its index, its
**real name** ("1V/octave pitch", "Frequency modulation"), and its exact jack
centre. Verified: 185 ports across 19 modules.

**3. The patch itself** — module order and position, and every cable as
`(from module, output N) → (to module, input M)`.

## So the preview is a composite, not a screenshot

Per-module PNGs laid out left to right in the patch's own order, at their true
widths, with cables drawn by us between real jack coordinates. It looks like
Rack because the panels *are* Rack's, but we control the layout, the cable
routing and the highlighting.

That is what makes hover-to-highlight possible: a screenshot is pixels, and a
composite knows which cable is which.

## The constraint that shapes everything: it is WIDE

Every Eurorack panel is exactly **380 px tall**. Only width varies:

| Module | HP | Width |
|---|---|---|
| Fundamental Mixer | 3 | 45 px |
| Fundamental VCA | 5 | 75 px |
| Fundamental VCF | 7 | 105 px |
| Fundamental VCO | 9 | 135 px |
| Fundamental SEQ3 | 22 | 330 px |

Measured, from Rack.

**A 10-module patch is 1,155 × 380 px — a 3:1 strip.** Eight modules is roughly
900 × 380. A preview pane around 800 px wide therefore cannot show eight
modules at 1:1, and this is the single biggest thing to design around.

The options, none free:

- **Scale to fit** — everything visible, but at 8 modules the silkscreen is
  unreadable and jacks are ~5 px. The picture stops teaching.
- **Horizontal scroll at 1:1** — readable, but you never see the whole patch,
  which is the thing the explanation is describing.
- **Wrap to rows** — Rack supports multiple rack rows, so this is legitimate
  rather than a cheat, and it trades width for height honestly.
- **Semantic layout** — ignore Rack's real positions and lay the modules out
  by signal flow instead. Truer to the *explanation*, less true to what they
  will see when they open Rack.

Worth designing at two densities: a small patch (4–5 modules, ~500 px, fits
comfortably) and a large one (12–14 modules, ~1,600 px, definitely does not).

## On sizing the demo

Seven or eight modules is a good demo — it is what a real voice with modulation
takes, and it is where the width problem starts to bite, which means the design
has to confront it rather than dodge it. **Large patches must work**, but they
can be the second case rather than the first.

## Two degradations to design for

**Panels we have no image for.** A module the user has installed but which was
not captured, or captured before a plugin update. Needs a placeholder panel at
the correct width, clearly not-artwork rather than broken.

**Ports we have no coordinates for.** Any module the user has never placed in a
rack is unmapped, so a cable to it has no jack to land on. It needs to
terminate somewhere honest — docked at the panel edge, say — rather than
guessing a position. A cable drawn confidently into the wrong jack teaches
something false, and this whole mode is a teaching surface.

Both are temporary per module: the first time someone opens a patch containing
it, it gets mapped and every later preview is exact.
