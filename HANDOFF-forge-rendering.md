# Context for the agent working on Forge's rendering

You are being asked to explain some changes to how Forge renders designs. This
is what they will land on, and the three questions whose answers actually
change what gets built.

Everything here is on branch `explore/modular-rack` in the worktree
`/Volumes/Workshop/Code/pulp-modular-rack` — 34 commits, unmerged, nothing on
`main`.

## What is being built

**Forge Modular** — a sibling to Forge for **VCV Rack**, the open-source
Eurorack modular synthesizer. Separate app, separate SKU, shipping alongside a
`.vcvplugin`. It builds two things:

1. **Modules** — one Eurorack panel plus its DSP. Close to Forge's plugin
   flow, and working: a prompt becomes a manifest and C++, is validated,
   compiled, driven for real, and installed. Eight of eight across a spread,
   every one using Pulp's DSP.
2. **Patches** — a whole rack of modules wired with cables. Forge has no
   equivalent. It is a *teaching* surface as much as a building one: the app
   explains which output goes to which input and why. Eight of eight build and
   pass structural checks; eight of eleven actually make sound, verified by
   running their real DSP.

## The part that depends on you

**Forge Modular's preview pane composites real module images.** It does not
draw modules from code and it does not screenshot a running Rack — Rack has no
facility to capture an assembled patch.

What it has instead, all verified:

- **A real PNG per installed module**, rendered by Rack itself
  (`Rack --screenshot`). 51 captured. `Fundamental/VCO.png` is 135 × 380 px,
  its exact panel size, with correct artwork including vendors who draw panels
  in code rather than in image files.
- **Every port's index, the vendor's own name for it, and its exact jack
  centre** — 185 ports across 19 modules, recorded by a module of ours that
  walks the running rack. Nothing on disk describes ports, so this is the only
  source.
- **Panel sizes**, so modules lay out at true width.

So the preview is: real panel images side by side at true widths, cables drawn
by us between real jack coordinates. It looks like Rack because the panels are
Rack's, and unlike a flat screenshot a composite knows which cable is which —
which is what makes hover-a-line-to-highlight-a-cable possible, the core
interaction of patch mode.

**Its dominant constraint is width.** Every panel is exactly 380 px tall; only
width varies (3 HP = 45 px, 9 HP = 135, 22 HP = 330). A ten-module patch is
1,155 × 380 — a 3:1 strip that no pane shows at 1:1.

## What we need from you

1. **Does the compositing approach survive your changes?** Specifically:
   loading and drawing image files, laying them out at fixed pixel sizes, and
   drawing lines/curves over them.
2. **Do the drawing primitives change?** Patch mode needs per-cable hit
   testing so hovering a line in the chat highlights its cable and vice versa.
   If picking or hit-testing moves, that interaction is the thing affected.
3. **Is the plugin editor the blast radius, or the whole drawing layer?** If
   your work is about how a *generated plugin's UI* renders, that is a
   different surface and Forge Modular's patch preview may be untouched. If it
   is the drawing layer itself, everything above gets rebuilt.

Answering 3 first is the most useful thing, because it decides whether the
other two matter.

## Where to read more

- `planning-draft-forge-modular-ux.md` — the full spec. §11 is the measured
  capabilities, §12 is how agent settings and model roles differ from Forge.
- `design-note-rendering.md` — how the preview is drawn, with the measurements.
- `design-brief-forge-modular.md` — what was asked of Claude Design.
- `design-note-round2.md` — feedback on the prototype that came back.
- `design/prototype/` — the imported design system, prototype and Settings.

Code, if useful: `tools/rack/patch.py` (inventory, explanation, lint, diff,
generation), `tools/rack/generate.py` (module generation), the two gates in
`tools/rack/*_gate.cpp`, and `tools/rack/export_design_data.py`, which turns
the recorded port map into real geometry for a design prototype.
