# PLAN — close the gap to the prototype

The prototype (`ForgeModular.dc.html`) showed things the shipped app does not.
These are not new ideas; they were designed and then not landed. This plan names
the gap precisely, so it can be closed and checked.

The house style is Forge Instrument's — the prototype's *content* is what we
want, expressed in the chrome the other three products already use. Nothing here
adds a new visual language.

---

## What already exists

- `ExplainDepth { terse, standard, learning }` — the three depths are real and
  the explanation text already varies by them.
- `PatchExplanation` in the chat column, `RackPreview` alone on the stage.
- Panel SVGs: the emitter writes one per module (23 KB each, light and dark).
- `Canvas::draw_svg` — Skia-backed, already in Pulp.

## Gap 1 — a built module shows no faceplate

**Symptom.** Build a module, and the stage shows an empty rectangle with the
module's name. The prototype shows the panel: knobs with coloured rings, labels,
a slider, switches, jacks.

**Cause.** `RackPreview` drew a placeholder for every module and never read the
artwork the emitter had already written beside it. Two things that should have
been one: the emitter knew where panels go, the preview did not.

**Fix.** `RackPreview::set_panel_directory()`, path derived from `tools_dir()`
rather than spelled out a second time; `draw_svg` the module's own panel and fall
back to the placeholder only when there is genuinely no artwork.

**Check.** A render test with a panel whose artwork is an unmistakable colour:
that colour must appear on the stage, and must NOT appear when the panel
directory is unset. The negative control is the point — without it the test
passes on a preview that draws nothing.

## Gap 2 — a module has no specification

**Symptom.** The prototype's left column carries the module's description and
then a spec table: WIDTH, CONTROLS, I/O, DSP, PANEL. The app shows the prompt
and the verdict, and nothing about what was built.

**Fix.** A `ModuleSummary` accessory in the chat column for the module artifact,
mirroring `PatchExplanation`'s place for the patch artifact. Every field is read
from the generated manifest — none is retyped, because a spec that disagrees with
the module is worse than no spec.

**Check.** Generate a module, assert each rendered row against the manifest it
was derived from. A row we cannot derive is not shown.

## Gap 3 — the patch explanation has no roles

**Symptom.** The prototype groups cables by role — AUDIO, PITCH & GATE, CLOCK,
MODULATION — each with a count, a coloured dot matching the cable in the rack,
and per-cable reasoning. The app lists cables flat.

**Fix.** Group by the role the generator already writes into each cable's colour
field. The dot and the cable share one source, so a patch that reads "AUDIO"
cannot be drawn in the modulation colour.

**Check.** The dot's painted colour equals the cable's painted colour for the
same connection — asserted on the rendered pixels, not on the enum. The depth
tabs already exist; assert each depth changes the rendered line count.

---

## Ordering

Gap 1 first: it is the one the eye lands on, and it is nearly done. Then Gap 3,
because the patch is the artifact with the weaker story. Gap 2 last — it is the
most code for the least surprise.

None of this changes generation. It changes what the app tells you about what it
generated, which is where the prototype was ahead of us.
