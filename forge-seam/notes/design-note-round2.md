# Forge Modular — feedback on prototype 2

The structure, the ask/build split, the wiring list and the mention picker all
landed. Four changes. The last is the substantive one.

## 1. The home tabs don't meet the composer

They're already *styled* to connect and then held apart:

```html
<div style="display:flex;gap:8px;margin-bottom:12px">
  <div style="border-radius:11px 11px 0 0; border:1px solid var(--line);
              border-bottom:none; ...">Module</div>
```

and the composer directly below:

```html
<div style="border-radius:0 18px 18px 18px; ...">
```

A square top-left corner on the composer, `border-bottom:none` and top-only
radii on the tabs — every part of that says "these join." Then
`margin-bottom:12px` puts 12 px of daylight between them, so they read as two
floating buttons above an unrelated box, and the composer's flat top-left
corner looks like a mistake rather than a joint.

- Drop the gap to `0` (or `-1px` to overlap the borders cleanly).
- The selected tab's fill should match the composer's surface so they read as
  one continuous shape.
- The unselected tab should sit *behind* the seam, not level with it.
- `gap:8px` between the two tabs is worth revisiting too — as drawn they're
  separate pills wearing tab shapes.

## 2. No caption under the build animation

The animation currently sits above the word `FORGING`. Drop it, and don't
replace it. The artwork is doing the work; a label underneath makes it read as
a loading spinner with a status string rather than as the product making
something.

If a state genuinely needs naming — a failure, a retry — that belongs in the
chat with the rest of the conversation, not welded under the artwork.

## 3. Three build animations, each specific to what it makes

Forge's current one is deliberately generic. Now that three different things
get built, each should look like the thing it's building. That specificity is
the charm; one generic shape used three times says the tool doesn't know what
it's making.

- **A DAW plugin** (Forge) — a plugin editor assembling: panel, controls, a
  meter or waveform coming alive. Should read as something that opens in a DAW
  window. Worth revisiting rather than inherited as-is.
- **A Eurorack module** — a tall narrow panel: blank, then knobs and jacks
  landing, silkscreen appearing, screws at the corners, settling onto a rack
  rail. Nothing about it should read as a rectangle in a DAW.
- **A patch** — cables. Arcing jack to jack, one at a time, settling into
  hanging loops. The modules are already there; what's being made is the
  connections between them.

One family — same easing, same palette, same sense of materialising — three
unmistakably different acts. Someone glancing from across a room should know
which they're looking at.

`fg-slack` and `fg-seat` in the prototype are exactly the right instinct:
cable slack and jack seating only happen in modular. More of that, and
something equivalent for the DAW one.

## 4. The patch panels are invented, and don't need to be

The prototype hardcodes each module's geometry:

```js
{id:'VCO', hp:8, name:'VCO',
 knobs:[['FREQ',.5,116], ...],
 jacks:[['V/OCT',.27,312,'in'], ['SAW',.73,312,'out']]}
```

Reasonable given you had nothing to go on. But we have the real thing, and it
differs enough to matter:

| Fundamental VCO | Prototype | Actual |
|---|---|---|
| Width | 8 HP | **9 HP** |
| Inputs | 1 — V/OCT | **4** — 1V/octave, Frequency modulation, Sync, Pulse width modulation |
| Outputs | 1 — SAW | **4** — Sine, Triangle, Sawtooth, Square |

What we have, per module the user owns:

- **A real PNG of the panel**, rendered by Rack itself — correct artwork,
  silkscreen, knobs, jacks, including vendors who draw panels in code rather
  than in artwork files. `Fundamental/VCO.png` is 135 × 380 px.
- **Every port**: its index, the vendor's own name for it ("Frequency
  modulation", not "IN 1"), and the exact centre of the jack.
- **The panel's true size.**

So the preview composites real panel images at true widths, with cables drawn
between real jack coordinates. It looks like Rack because the panels *are*
Rack's — and unlike a flat screenshot, a composite knows which cable is which,
which is what makes hover-to-highlight possible.

### What that changes for the design

**Panels are busier than the mockup.** A real VCO has eight jacks in two rows
plus trimpots, not two jacks. Denser and less tidy than drawn; the layout has
to survive it.

**Widths are fixed and various**: 3 HP = 45 px, 5 HP = 75, 7 HP = 105,
9 HP = 135, 22 HP = 330. They can't be nudged to fit.

**Two degradations to design.** A module with no captured image needs a
placeholder at the correct width reading as not-yet-drawn rather than broken.
A module never placed in a rack has no port coordinates, so its cables must
terminate somewhere truthful — docked at the panel edge — rather than guessing
a jack. Both resolve permanently the first time the user opens that module in
Rack.

The layout, interaction and visual language should survive this unchanged. It
is the module drawing underneath that swaps from invented geometry to real
images and real coordinates.
