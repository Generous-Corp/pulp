# Claude Design prompt — Forge Modular

Paste everything below the line into Claude Design, attaching
`Forge - AI Plugin Factory-2.html` as the reference. It is written to be read
cold, so it restates what Forge is rather than assuming the conversation.

---

I'm designing **Forge Modular**, a sibling product to Forge (the attached
`Forge - AI Plugin Factory-2.html` is Forge's design — please match its
language, not copy its screens).

## What Forge is, and what Forge Modular is

**Forge** turns a text prompt into a finished audio plugin — AU/VST3/CLAP —
for use in a DAW. You describe an instrument or effect, an agent writes the
DSP and the UI, and you get a real plugin you can play.

**Forge Modular** does the same for **VCV Rack**, the open-source Eurorack
modular synthesizer. It is a separate app with two things it can build:

1. **Modules** — a single Eurorack module: a front panel with knobs and
   patch jacks, and the DSP behind it. Structurally very close to Forge's
   plugin building, so this flow should feel like a sibling of it.
2. **Patches** — a whole rack of modules wired together with virtual cables.
   This has **no equivalent in Forge** and needs its own design thinking. It
   is as much a *teaching* surface as a building one: the app explains what
   it connected and why, so the user learns modular synthesis by watching
   their idea get patched.

Please don't retrofit patch building into the plugin-builder layout. Modules
and patches are different activities and deserve to feel different, while
still obviously being one family with Forge.

## Vocabulary you'll need

- **Eurorack** — a hardware modular standard. Modules are panels of fixed
  height in a rack, widths measured in **HP** (1 HP ≈ 5 mm). A module is
  typically 2–20 HP: tall, narrow, dense with knobs and jacks.
- **Patch cables** — coloured cables physically connecting one module's
  output jack to another's input jack. They hang in loops. Rack draws them
  over the panels; a busy patch is a beautiful mess of colour.
- **Signal roles** — three kinds of thing travel down cables: **audio**
  (what you hear), **pitch/gate** (which note, and when), and **modulation**
  (slow changes that make sounds move). Colour-coding by role is the single
  most useful visual idea in this product.
- **A voice** — the classic chain: oscillator → filter → amplifier, with an
  envelope opening the amplifier. Most patches are variations on it.

## The two things to design

### A. Module building

Close to Forge's plugin flow: prompt → generated → preview → use it. The
preview should show **the actual Eurorack front panel** being built — tall,
narrow, knobs and jacks laid out on it.

One honest constraint that shapes this flow: **VCV Rack cannot load a new
module without restarting.** A newly built module can't appear in a running
Rack. So the flow ends in a state-dependent button — "Launch Rack with this
module" when Rack isn't running, "Relaunch Rack" when it is. Please design
this as a confident single step rather than an apology; it's one click, but
it should feel deliberate rather than like a failure.

### B. Patch building

Left: chat. Right: the patch — real module panels side by side with cables
drawn between them.

The chat explains the wiring in a readable, grouped form. This is real
output from the working prototype, for "an evolving ambient drone that plays
by itself":

```
AUDIO
  VCO SAW → VCF IN
  VCF LP → VCA IN
  VCA OUT → Audio 2 IN 0

PITCH & GATE
  MULT 1 → SEQ CLK
  SEQ CV → VCO V/OCT
      the sequencer walks the drone's pitch so it never settles on one note
  EUCL GATE → ENV GATE
      euclidean rests against the 8-step loop keep the swells from lining up

MODULATION
  LFO SQR → MULT IN
      a very slow clock, split two ways — the patch plays itself, no keyboard
  LFO TRI → VCF CV
      an unrelated slow LFO sweeps the cutoff — the timbre breathes
  ENV ENV → VCA CV
      long attack and release means notes swell in and fade, never strike
```

Design how this should look. Ideas worth exploring, not prescriptions:
- Each role group colour-keyed, with the **same colour used for that cable in
  the patch diagram**, so reading the text highlights the picture.
- Hovering a line in the chat highlights that cable in the diagram, and vice
  versa. This is the core interaction of the whole mode.
- The *why* clauses are the teaching. They should be visually secondary to
  the connection lines but not buried.
- A depth control (terse / standard / learning) — standard is the default.

### Patch-specific interactions to consider

- Dragging an existing `.vcv` patch file onto the chat to have it explained,
  or added to. (Explaining is free — it's computed from the file.)
- Asking a question that does **not** change the patch ("why did you wire the
  LFO to the filter?") versus asking for a change ("add a reverb"). The user
  should be able to tell which will happen **before** they send.
- The patch may be edited by the user inside Rack. On return the app shows
  what changed structurally and never silently overwrites their work.

## Animations — the part I most want your eye on

Forge has a generation animation (see `fg-materialize`, `fg-shimmer`,
`fg-rise`, `fg-breathe` in the attached file). Forge Modular should have
**two of its own**, clearly the same family, with a Eurorack accent:

1. **Module building** — something that evokes a front panel coming into
   being: the panel blank, then knobs and jacks landing on it, silkscreen
   labels appearing, the panel settling into the rack. Eurorack panels have a
   specific look — brushed or matte, silkscreened text, screws at the
   corners, jacks as dark circles with metal rings.
2. **Patch building** — something that evokes **cables being patched**: cables
   arcing from jack to jack, one at a time, settling into hanging loops. The
   satisfying thing about modular is the physical act of patching, and this
   animation is where that feeling lives. Consider revealing cables in the
   same role order the explanation uses — audio path first, then pitch/gate,
   then modulation — so the animation and the text tell the same story.

Both should read as siblings of Forge's animation, not strangers.

## Constraints to design within

- **Palette**: Forge's — `#16DAC2` (accent), `#F6B847`, `#5E78FF`, `#8B6CF5`,
  `#46F0DB`, `#3FCF77`, on the dark `#161A21` / `#1E2530` surfaces.
- **Type**: Jost for UI, JetBrains Mono for wiring text and module slugs.
- **Rack's own cable colours** are `#f3374b #ffb437 #00b56e #3695ef #8b4ade`.
  Where the diagram shows real cables it should probably use these, since
  that's what the user sees in Rack.
- Dark theme first.
- Desktop app, resizable. Assume ~1280×800 as the design size.

## What I'd like back

1. Both tabs, at rest and mid-generation.
2. The patch explanation treatment, in all three depths if you have the
   appetite — this is the heart of the product.
3. The two animations, as working CSS/JS in the deliverable.
4. Any structural disagreement you have with the above. In particular: I've
   assumed module building and patch building are two tabs in one window, but
   a module belongs to *every* patch while a patch project is one document —
   if that tension suggests a different shape to you, say so.
