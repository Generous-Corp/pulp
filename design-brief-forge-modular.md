# Claude Design prompt — Forge Modular

Paste everything below the line, attaching `Forge - AI Plugin Factory-2.html`.
This asks for a *refinement* of that design into a sibling product, not a new
one from scratch.

---

Attached is **Forge**, my AI plugin factory: you describe an instrument or
effect, an agent writes the DSP and the UI, and you get a real AU/VST3/CLAP
plugin for your DAW.

I'm building **Forge Modular**, a sibling product for **VCV Rack** — the
open-source Eurorack modular synthesizer. Please refine the attached design
into this second product. **Share everything that should be shared** — the
home screen, the chrome, the design language, the generation-animation family
— and diverge only where the subject genuinely differs. It should be obvious
these are one family, and equally obvious which one you're in.

## What's different about it

Forge makes **one plugin** for a DAW. Forge Modular makes **two things**, and
they're different enough to want different treatment:

1. **Eurorack modules** — a single module: a tall narrow front panel with
   knobs and patch jacks, and the DSP behind it. This is close enough to
   Forge's plugin building that it should feel like the same flow.
2. **Patches** — a whole rack of modules wired together with cables. Forge has
   no equivalent. It's as much a *teaching* surface as a building one: the app
   explains what it connected and why, so someone learns modular synthesis by
   watching their idea get patched.

Please don't force patch building into the plugin-builder layout.

## Vocabulary

- **Eurorack** — modules are panels of fixed height, widths in **HP**
  (1 HP ≈ 5 mm), typically 2–20 HP. Tall, narrow, dense with knobs and jacks.
- **Patch cables** — coloured cables joining one module's output jack to
  another's input. They hang in loops over the panels. A busy patch is a
  beautiful mess of colour.
- **Signal roles** — three kinds of thing travel down cables: **audio** (what
  you hear), **pitch/gate** (which note, and when), **modulation** (slow
  changes that make sounds move). Colour-coding by role is the most useful
  visual idea in this product.

## Module building

Like Forge's flow. The preview should show **the actual Eurorack front panel**
being built — tall, narrow, knobs and jacks on it.

One constraint that shapes the ending: **Rack can't load a new module without
restarting.** So the flow ends in a state-dependent button — "Launch Rack with
this module" when Rack isn't running, "Relaunch Rack" when it is. One click.
Design it as deliberate, not as an apology.

## Patch building

Left: chat. Right: the patch — real module panels side by side, cables between
them.

The chat explains the wiring. This is **real output from the working
prototype** for "an evolving ambient drone that plays by itself":

```
AUDIO
  VCO SAW → VCF IN
  VCF LP → VCA IN
  VCA OUT → Audio 2 To "device output 1"

PITCH & GATE
  MULT 1 → SEQ CLK
  SEQ CV → VCO V/OCT
      the sequencer walks the drone's pitch so it never settles on one note

MODULATION
  LFO TRI → VCF CV
      an unrelated slow LFO sweeps the cutoff — the timbre breathes
  ENV ENV → VCA CV
      long attack and release means notes swell in and fade, never strike
```

Worth exploring:
- Each role group colour-keyed, with **the same colour on that cable in the
  diagram**, so reading the text lights up the picture.
- Hovering a line highlights its cable, and vice versa. This is the core
  interaction of the mode.
- The *why* clauses are the teaching — secondary to the connections, not buried.
- A depth control (terse / standard / learning); standard is the default.

Also: asking a question that **doesn't** change the patch ("why did you wire
the LFO there?") versus asking for a change ("add a reverb"). The user should
be able to tell which will happen **before** they send.

## @mentions — a real feature, with real numbers

There are **4,705 modules across 543 plugins** in the VCV library, and the app
has them all indexed locally. Typing `@` searches every one:

```
'vco' — 51 modules, 2 usable now

  ✓ ready     Fundamental/VCO         VCO              [VCO, Polyphonic]
  ↓ free      Befaco/EvenVCO          Even VCO         [VCO, Hardware clone]
  $ premium   ALM034/ALM034           Pamela's Pro Workout
```

Three states, and they matter:

- **✓ ready** — installed, can be used right now
- **↓ free** — free, but not installed; install from Rack's Library first
- **$ premium** — costs money, or comes with a VCV+ subscription

**Only "ready" can be used in a patch.** If someone @mentions a premium module
they don't have, the app should say so *before generating anything* — building
a patch around something that can't load is wasted time and money.

One honest wrinkle for the copy: **we can't tell whether someone already owns
a premium module they haven't installed.** So the message must never say "you
need to buy this" — it should say something closer to "if you own this, sync
it in Rack's Library; otherwise it's a purchase or VCV+." Getting this wrong
means telling someone to buy what they already own.

Design the mention picker, the three states, and how a blocked mention reads.
Brands work too — `@befaco`, `@4ms`, `@mutable instruments` (425 brands).

## The random button

Forge has a random button next to the prompt — press it and you get something
without having to think of it first. Keep it, and make it **follow the mode
you're in**:

- **Module tab** → a random module. This is the default mode.
- **Patch tab** → a random patch.

Those produce very different things, so the button shouldn't feel like one
generic shuffle. A random module is a single idea — "a 6 HP wavefolder with
drive and symmetry". A random patch is a whole rack that has to make sense as
a piece of music, and it can only use modules the person actually has
installed, which varies enormously between users.

Worth thinking about: whether the button says what it's about to make before
it commits, or just makes it. Forge's version can be reckless because a plugin
is one thing; a random patch is a bigger swing, and a user with 12 modules
installed gets a very different result from one with 400.

## Install

The app compiles real modules, so it needs VCV's Rack SDK (~40 MB), which we
aren't permitted to redistribute — it's fetched from vcvrack.com at install.

**Keep this as close to invisible as possible.** It should download silently
during install. There is exactly one thing the user must see: a licence
acknowledgement, because it's legally required. One checkbox, plain language,
in the installer — not a first-run wizard, not a multi-step flow. Design that
one moment so it reads as honest rather than as a hurdle.

## Animations — the part I most want your eye on

Forge has a generation animation (`fg-materialize`, `fg-shimmer`, `fg-rise`,
`fg-breathe` in the attached file). Forge Modular should have **two of its
own**, clearly the same family, with a Eurorack accent:

1. **Module building** — a front panel coming into being: blank panel, then
   knobs and jacks landing, silkscreen labels appearing, the panel settling
   into the rack. Eurorack panels have a specific look — matte or brushed,
   silkscreened text, screws at the corners, jacks as dark circles with metal
   rings.
2. **Patch building** — **cables being patched**: arcing from jack to jack,
   one at a time, settling into hanging loops. The satisfying thing about
   modular is the physical act of patching, and this is where that feeling
   lives. Consider revealing cables in the same role order the explanation
   uses — audio, then pitch/gate, then modulation — so animation and text tell
   one story.

Both should read as siblings of Forge's animation, not strangers.

## A design opportunity worth taking

Generated panels currently use Rack's stock knobs and jacks. Those are
licensed CC BY-NC, which constrains what someone can do with a module they
built and sell. **If you design our own knobs, jacks, switches and sliders**,
that constraint disappears *and* Forge Modular modules become recognisably
ours on sight. I'd like these as part of the deliverable — a small component
set, in the Forge design language, that reads correctly at Eurorack scale
(a knob is ~12 mm across; a jack ~8 mm).

## Constraints

- **Palette**: Forge's — `#16DAC2` accent, `#F6B847`, `#5E78FF`, `#8B6CF5`,
  `#46F0DB`, `#3FCF77`, on `#161A21` / `#1E2530`.
- **Type**: Jost for UI, JetBrains Mono for wiring text and slugs.
- Rack's own cable colours are `#f3374b #ffb437 #00b56e #3695ef #8b4ade`;
  where the diagram shows real cables, those are what the user sees in Rack.
- Dark theme first. Desktop, resizable, design at ~1280×800.

## What I'd like back

1. The shared home screen, and both modes at rest and mid-generation —
   including the mode-aware random button.
2. The patch explanation treatment — the heart of the product.
3. The @mention picker with its three states.
4. The two animations, as working CSS/JS.
5. The Eurorack component set (knobs, jacks, switches, sliders).
6. Any structural disagreement. In particular: I've assumed module building
   and patch building are two tabs in one window, but a module belongs to
   *every* patch while a patch is one document — if that tension suggests a
   different shape, say so.
