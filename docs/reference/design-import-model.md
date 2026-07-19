# The Figma → Pulp Design-Import Model

This is the conceptual reference for how a design authored in Figma becomes a
Pulp UI. It is written for both designers and developers: designers who want to
know *what to draw* and *how much they can restyle*, and developers who want to
know *what gets wired automatically* and *what they still own*.

For the practical how-to (installing the plugin, exporting, running
`pulp import-design`), see the [Figma plugin guide](../guides/figma-plugin.md).
For the CLI and pipeline reference, see
[Design Import API Reference](design-import.md).

---

## The model: semantic, not visual

Most design-to-code tools treat a design as a picture: they try to reproduce
pixels. Pulp's import is different. When you drop a **Pulp library component**
— "Pulp / Knob", "Pulp / Fader", "Pulp / Meter", "Pulp / XYPad", "Pulp / Waveform",
"Pulp / Spectrum" — into your design, you are not
just placing a graphic. You are declaring **what the control *means***.

A Pulp library component carries semantic metadata alongside its appearance:

- the kind of control it is (knob, fader, meter, …),
- its label,
- its parameter range (min / max / default),
- its units,
- and the parameter it should bind to.

The picture tells Pulp how the control should *look*. The library component
tells Pulp what the control *is*. Pulp imports the meaning, not just the
pixels.

### Three layers worth naming

It helps to separate three distinct concerns:

| Layer          | Who defines it          | What it controls |
|----------------|-------------------------|------------------|
| **Meaning**    | The Pulp library        | The semantic contract: this node is a knob bound to a parameter, with a label, range, and units. Fixed by the library component you chose. |
| **Appearance** | You, the designer       | Everything visual: fill, size, typography, layout, corner radius, gradients, the whole look. Fully customizable. |
| **Route**      | Pulp, per control       | How the control is realized at runtime — a native widget, a custom-rendered widget, or a live/visual fallback. Decided by the importer from the meaning it recognized. |

You own **appearance**. The library defines **meaning**. Pulp decides the
**route**. Keeping these three apart is the key to understanding everything
below.

---

## The additive-fidelity guarantee

The single most important property of this pipeline: **the Pulp library is
purely additive fidelity. It only ever helps — it never gates the import.**

Concretely:

- **A design that uses *none* of the Pulp library components still imports.**
  It does not break. Nodes the importer doesn't recognize as semantic controls
  fall back to a visual/live route — the importer's fallback route for an
  untagged node is `hybrid`, with a recorded `fallback_reason` so the choice is
  always explainable. You get a faithful import of the layout and visuals; you
  just wire up the behavior yourself afterward.

- **Linking a "Pulp / Knob" *upgrades* that node.** Where a recognized library
  component is present, the importer keys off its `audio_widget` semantic field
  and imports the node as a **native Knob** with its parameter binding already
  wired. Recognition is by the published component's **key** (authoritative),
  and as a fallback by **name prefix** ("Pulp / Knob…") for designs that follow
  the naming convention without having pulled in the published library.

- **Net effect:** every Pulp library component you use raises the fidelity of
  that one control from "draw it, wire it yourself" to "recognized, bound,
  native." Components you *don't* use cost you nothing — they import as before.
  There is no all-or-nothing cliff. Start with zero Pulp components and a design
  still imports; add them one at a time and each one upgrades exactly the
  control it sits on.

> Mapping precedence inside the importer is `audio_widget` first, then the
> node's element type, then HTML-style subtype attributes — with anything
> unsupported degrading to a diagnostic rather than failing the import.

---

## Restyle freely

A common worry: *if I change how the Pulp / Knob looks, will Pulp stop
recognizing it?* No.

Recognition keys off the **component identity** — the Figma library component
and its name — **not the pixels**. In Figma, an instance keeps its link to the
main component even when you override its fill, size, text, layout, or corner
radius. Those overrides are exactly what instance overrides are *for*. So:

- Repaint the knob, resize it, swap its label font, restructure its
  auto-layout — **the semantic link survives.** The instance is still a
  "Pulp / Knob", so it still imports as a native, bound Knob.
- The library defines **meaning**; your design defines **appearance**. Changing
  appearance never severs meaning.

This is what makes the model usable for real design work: you are not boxed
into the library's default look. You inherit the library's *semantics*, and your
restyling drives the imported control's appearance — the importer samples your
captured component and skins the native widget to match it (the knob's silver
finish, the fader's track/thumb, the meter's gradient).

> **Fidelity note.** Recognition and binding from a restyled instance are solid.
> Reproducing your *exact* per-instance visuals on the native control is an
> actively improving path — the knob is faithful today; fader and meter
> appearance fidelity (precise track/thumb width, meter width) is being tightened
> in [#3191](https://github.com/Generous-Corp/pulp/issues/3191). Where exact
> pixels matter more than live interaction, the captured visual is preserved in
> the export and can back a recorded-paint route.

---

## One design → multiple runtimes

The same imported design is not tied to a single output. Author once in Figma,
then flow it into whichever runtime fits the moment:

- **Live JavaScript UI** — for rapid iteration and hot reload. Change the design
  or the script and see it immediately.
- **Native desktop / mobile app** — the design materializes into Pulp's native
  view tree.
- **Fully compiled C++ app or audio plugin** — the design bakes into C++ for the
  production plugin or app.

You don't rebuild the UI by hand for each target. The import pipeline carries
one design into all three. (The CLI flags that select these — `--mode live` vs
`--mode baked`, `--emit js|ir-json|cpp` — are documented in the
[Design Import API Reference](design-import.md).)

---

## Where the contract lives (cross-links)

The model above is backed by machine-readable schemas and an agent-facing
skill, so the behavior is checkable, not just described:

- **The semantic source contract** —
  [`tools/import-validation/schemas/source-contract-v0.schema.json`](https://github.com/Generous-Corp/pulp/blob/main/tools/import-validation/schemas/source-contract-v0.schema.json).
  One serialized shape for the per-node source / route / value / event / state /
  style evidence, shared by the C++ importer and the audit summary so both can
  be checked against a single definition. This is where the route taxonomy
  (`native_cpp`, `native_html`, `live_js`, `hybrid`, `recorded_paint`,
  `unsupported`) and the `fallback_reason` field are defined.

- **The Figma plugin export schema** —
  [`tools/figma-plugin/schema/figma-plugin-export-v1.json`](https://github.com/Generous-Corp/pulp/blob/main/tools/figma-plugin/schema/figma-plugin-export-v1.json).
  The JSON shape the "Design for Pulp" plugin emits, including the audio-widget
  fields that the importer maps to `audio_widget` / `audio_label` /
  `audio_min` / `audio_max` / `audio_default`.

- **The agent-facing skill** —
  [`.agents/skills/import-design/SKILL.md`](https://github.com/Generous-Corp/pulp/blob/main/.agents/skills/import-design/SKILL.md).
  How Claude Code and Codex drive the import end to end, including the mapping
  precedence and native-resolution layer.

---

## See also

- [Figma plugin guide](../guides/figma-plugin.md) — install the plugin, export,
  and run the importer.
- [Importing Designs](../guides/importing-designs.md) — the broader import guide
  covering all sources (Figma, Stitch, v0, Pencil, …).
- [Design Import API Reference](design-import.md) — CLI flags, IR, validation.
- [Layout model](layout-model.md) — why Pulp is Flexbox + Grid only, and why
  that keeps the design-import contract clean.
