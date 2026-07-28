# Refinement round — Forge Modular

Three changes to the prototype. The third is the substantive one.

## 1. No caption under the generation animation

The build animation currently sits above the word `FORGING`. Drop it — and
don't replace it with another word. The animation is doing the work; a label
underneath is telling someone what they can already see, and it reads as a
loading spinner with a status string rather than as the product making
something.

If a state genuinely needs naming (a failure, a retry), that belongs in the
chat where the rest of the conversation is, not welded under the artwork.

## 1b. Three animations, each specific to what it is making

Forge's existing build animation is deliberately generic — a plugin-shaped
rectangle with knobs. Now that there are three different things being built,
each should look like the thing it is building. That specificity is the whole
charm; a generic shape used three times says the tool doesn't know what it's
making.

- **A DAW plugin** (Forge) — a plugin editor assembling: the panel, its
  controls, a waveform or meter coming alive. It should read as something that
  opens in a DAW window. Worth revisiting rather than inheriting as-is.
- **A Eurorack module** (Forge Modular) — a tall narrow panel: blank, then
  knobs and jacks landing on it, silkscreen appearing, screws at the corners,
  the panel settling into a rack rail. Nothing about this should read as a
  rectangle in a DAW.
- **A patch** (Forge Modular) — cables. Arcing from jack to jack, one at a
  time, settling into hanging loops. The modules are already there; what is
  being made is the connections between them.

All three should be recognisably one family — same easing language, same
palette, same sense of materialising — while being unmistakably three
different acts. Someone glancing at a screen from across a room should know
which of the three they're looking at.

The `fg-slack` and `fg-seat` keyframes in the prototype are exactly the right
instinct: cable slack and jack seating are things that only happen in
modular. More of that, please, and something equivalent for the DAW one.

## 2. The tab seam on the home screen

`Module` / `Patch` float above the composer with a visible gap and their own
borders, so they read as two detached buttons rather than as tabs selecting
the panel below. The composer should read as the selected tab's panel — shared
edge, no daylight between them.

## 3. The patch panels are invented, and they don't need to be

The prototype hardcodes each module's geometry:

```js
{id:'VCO', hp:8, name:'VCO',
 knobs:[['FREQ',.5,116], ...],
 jacks:[['V/OCT',.27,312,'in'], ['SAW',.73,312,'out']]}
```

Reasonable, given you had nothing to go on. But we have the real thing, and
they differ enough to matter:

| Fundamental VCO | Prototype | Actual |
|---|---|---|
| Width | 8 HP | **9 HP** |
| Inputs | 1 — V/OCT | **4** — 1V/octave, Frequency modulation, Sync, Pulse width modulation |
| Outputs | 1 — SAW | **4** — Sine, Triangle, Sawtooth, Square |

What we actually have, per module the user owns:

- **A real PNG of the panel**, rendered by Rack itself — correct artwork,
  silkscreen, knobs, jacks, including vendors who draw panels in code rather
  than in artwork files. `Fundamental/VCO.png` is 135 × 380 px.
- **Every port**: index, the vendor's own name for it ("Frequency modulation",
  not "IN 1"), and the exact centre of the jack in panel coordinates.
- **The panel's true size.**

So the preview should composite real panel images at true widths, with cables
drawn between real jack coordinates. It will look like Rack because the panels
*are* Rack's.

### What this changes for the design

**Panels are busier than the mockup.** A real VCO has eight jacks in two rows
plus trimpots, not two jacks. Modules are denser and less tidy than drawn, and
the layout has to survive that.

**Widths are fixed and various.** 3 HP = 45 px, 5 HP = 75, 7 HP = 105,
9 HP = 135, 22 HP = 330. They cannot be nudged to fit.

**Two honest degradations to design.** A module with no captured image needs a
placeholder at the correct width that reads as not-yet-drawn rather than as
broken. A module never placed in a rack has no port coordinates, so its cables
need to terminate somewhere truthful — docked at the panel edge — rather than
guessing a jack. Both resolve permanently the first time the user opens that
module in Rack.

The layout, interaction and visual language in the prototype are good and
should survive this. It is the module drawing underneath that swaps from
invented geometry to real images and real coordinates.
