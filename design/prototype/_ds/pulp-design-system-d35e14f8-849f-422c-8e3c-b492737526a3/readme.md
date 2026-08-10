# Pulp Design System — "Ink & Signal"

The default visual language for **Pulp**, a cross-platform framework for building
audio software, creative tools, AI applications and utilities (C++20 core, Swift
on Apple, JS-scripted GPU UIs). This system is what those interfaces render in —
audio plugins, DAW-like editors, AI apps and dev tools — and it is designed to be
immediately recognizable as *a Pulp app*.

> **Source material**
> - Framework repo: https://github.com/danielraffel/pulp/ (MIT, alpha)
> - Docs: https://www.generouscorp.com/pulp/
> - Pulp ships a Flexbox/Grid GPU UI engine (Dawn · Skia · QuickJS) and 15+ native
>   widgets (Knob, Fader, Toggle, ComboBox, XYPad, WaveformView, SpectrumView…).
>   This design system gives those widgets a *look* — it is a visual language,
>   not a component library bolted onto the engine.
> - Direction set with the maintainer: fun, light, playful, timeless; Paul Rand /
>   Charles & Ray Eames sensibility, modern and fresh; "ink & paper, not chrome."

---

## The big idea

**Ink & Signal.** Pulp draws on the heritage of paper-pulp & screenprint — the
Swiss-modern, Rand/Eames era of bold flat color, geometric type and generous
white space — and brings it into a modern GPU studio. Cool graphite neutrals,
luminous "ink" accents that glow like a signal trace, geometric letterforms, and
motion with a little spring. It is **dark-first** (a calm graphite studio, alive
with glow) with a real **paper-light twin**. The primary is a vibrant **signal
teal** — rich and waveform-bright; red is reserved for peaking & danger.

Five commitments:

1. **Native, not web-like.** Inset wells, raised controls, tactile knobs/faders —
   instrument surfaces, not form fields.
2. **Cool & calm.** Neutrals are graphite; dark mode is ink, not `#000`. Warmth
   comes from the amber accent, never from a brown cast. No glassy SaaS gradients.
3. **Luminous, GPU-native.** Accents glow — knob arcs, meters, waveforms and the
   transfer curve bloom softly, showing off what the renderer can do.
4. **Screenprint contrast.** Dark ink rides on bright accent fills — playful,
   distinctive, and AA-accessible.
5. **Springy.** Controls respond instantly and settle with a soft overshoot.

---

## CONTENT FUNDAMENTALS — voice & copy

- **Tone:** confident, warm, a little playful. Studio language, never marketing
  fluff. "Sound, shaped." · "Build instruments, not interfaces."
- **Person:** address the maker as a peer. Imperative verbs on actions
  (*Render, Export, Ship*). Avoid "please/sorry."
- **Casing:** Sentence case for buttons, labels and headings. **UPPERCASE only**
  for mono eyebrows / section labels (tracked +12%) and unit-bearing badges
  (`VST3`, `48 kHz`).
- **Numbers are first-class.** Always monospace, tabular, with a unit:
  `-6.2 dB`, `4.8 :1`, `120.0 BPM`, `00:42.318`. Never hide a value behind a
  knob — show it.
- **Emoji:** none. The mark and the inks carry the personality.
- **Microcopy examples:** "Build succeeded — VST3 · AU · CLAP signed in 4.8s" ·
  "Compiling shaders…" · preset names read like gear ("Glue · Mix Bus",
  "Velvet Plate").

---

## VISUAL FOUNDATIONS

**Color.** A cool *graphite* neutral ramp is the ink & the paper; semantic surface
tokens layer it light-to-dark by elevation. Accents are luminous **inks**:
**signal teal `#16DAC2` (primary)**, violet `#8B6CF5`, indigo `#5E78FF`, amber
`#F6B847` (+ leaf/pink for data). **Coral `#FF5C4D` is reserved for peaking &
danger** — red never carries a neutral action. Dark text (`--on-ink #052320`) sits
on every bright fill. Dark mode app surface is `#161A21` (cool graphite); light
mode is paper `#EDEFF2` with white raised cards. Accents carry GPU **glow** tokens
(`--glow-sm/md/lg`) used on knob arcs, meters, waveforms and the transfer curve.

**Type.** **Jost** — geometric, Futura-lineage — for display, UI and body
(weights 400–800; display 800 at −3% tracking). **JetBrains Mono** for all values,
meters, code, logs and version chips. Eyebrows are mono, uppercase, +12% tracked.
Comfortable scale: 15px UI base, big confident display.

**Spacing & shape.** 4px grid, breathable. Rounded shape language — buttons/inputs
`10px`, cards/panels `20px`, plugin shells `28px`, knobs & toggles full-round.

**Elevation.** Dark mode leans on layered warm surfaces + a top inner highlight on
raised controls; light mode uses soft warm-gray shadows. Tracks, inputs and meters
are **inset wells** (`--elev-inset`). Cards `shadow-md`, menus `shadow-lg`, dialogs
`shadow-xl`.

**Borders.** Hairline, warm, low-contrast (`--line` ≈ 12% ink). Strong only where a
control needs definition.

**Focus.** A 3px coral ring (`--focus-ring`) — always visible, springy, on-brand.

**Motion.** Springy & playful. `--ease-spring cubic-bezier(.34,1.56,.64,1)` for
press-release, toggles and popovers (overshoot); `--ease-out` for entrances/hover
lift; durations 80–480ms. Hover lifts & brightens; press dips & scales to .95–.97;
meters & playheads animate linearly. Respects `prefers-reduced-motion`.

**Backgrounds.** Flat warm surfaces. A *single* very-soft coral radial wash is
permitted behind a hero plugin window — never multi-stop gradients, never noise.

**Imagery.** Functional data viz only (waveforms, transfer curves, meters) drawn
from real signal math in the accent inks — no decorative illustration, no stock.

---

## ICONOGRAPHY

Pulp's engine renders icons as resolution-independent **SDF / vector primitives**
in-GPU. For these HTML specimens and kits we use **[Lucide](https://lucide.dev)**
(CDN) as the closest open match — clean, geometric, consistent ~1.75px stroke that
sits naturally with Jost. *(Substitution flagged: swap for Pulp's own SDF icon set
in production.)* Icons are line-style, currentColor, 16–18px in UI, never filled
except status glyphs. No emoji; units and short labels are typeset, not iconified.

---

## Index / manifest

```
styles.css              ← consumers link THIS (import list only)
components.css           ← shared .pulp-* primitive + audio classes
tokens/
  fonts.css             Jost + JetBrains Mono (Google Fonts)
  colors.css            stone ramp + ink accents + data/signal
  semantic.css          theme-adaptive surfaces/text/lines (dark + light)
  typography.css        families, scale, weights, tracking
  spacing.css           4px space scale + radius + control sizing
  elevation.css         shadows, inset wells, focus ring, borders
  motion.css            easings, durations, keyframes
assets/
  pulp-mark.svg         the signal-pulse mark (currentColor)
guidelines/             foundation specimen cards (Design System tab)
  brand-logo · color-* · type-* · space/radius/elevation · motion-*
components/             reusable primitives (JSX + .d.ts + .prompt + card)
  buttons/   Button, IconButton
  forms/     Slider, RangeSlider, ValueField  (+ inputs card)
  toggles/   Switch  (+ chips/checkbox card)
  navigation/Tabs    (+ menus card)
  audio/     Knob, ModulationKnob, Meter, MusicalTyping,
             WaveformEditor, Recorder, ChannelStrip
             (+ faders, transport/waveform cards; waveform.js helper)
  surfaces/  cards, panels, toasts, GroupBox, PropertyPanel
  feedback/  progress, spinner, skeleton card
ui_kits/
  audio-plugin/index.html   Velvet — a bus compressor (flagship demo)
SKILL.md                Agent-Skill manifest for download/Claude Code
```

**Component API:** import from the generated bundle namespace (see the card files).
Each component is styled purely through the `--*` tokens, so it adapts to light/dark
automatically and stays identical to its specimen card.

---

## Caveats / what's next
- Fonts are the open Jost + JetBrains Mono. Swap for licensed Futura / Neue Haas
  if you want the "real" Rand-era cut.
- Icons are Lucide (CDN) standing in for Pulp's in-engine SDF set.
- Built deep on the maintainer's priorities (primitives, foundations, audio
  patterns, motion, one plugin). Dev-tool patterns (consoles, diagnostics, graph
  editors) and additional example apps (AI app, creative editor) are documented in
  the language but not yet built as kits — easy to add next.
