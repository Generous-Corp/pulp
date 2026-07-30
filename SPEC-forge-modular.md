# Forge Modular — product spec

Ready to implement. Every phase states what "done" means and what proves it.

The standing rule: **nothing is reported as working that has not been driven.**
Not built, not rendered, not "tests pass" — driven, and watched doing the thing.
This project has about twenty instances of a gate that was wrong the first time
it met real material, every one found by running it and none by reading it.

---

## What you get at the end

### Installers, signed and notarized

1. **Forge Modular** — one `.pkg` carrying the standalone app, AU, VST3, CLAP,
   and the `.vcvplugin` with every generated module.
2. **Forge Instrument**, **Forge MIDI**, **Forge FX** — rebuilt from current
   `origin/main` against the shared chrome, each validated, each proven
   pixel-identical to its baseline.

### Forge Modular, working

- **Standalone app and three plugin formats**, all running Forge's real UI —
  same rail, composer card, shelf, motion. Not a copy of it.
- **Home screen** with Module | Patch tabs, mutually exclusive, each changing the
  hero, the button label and the artifact badge.
- **`@` mentions** as a native overlay in the composer: type `@`, a dropdown
  filters 4,705 library modules, keyboard-first, only installable modules
  insertable.
- **Prompt → module**: type, click Build, get a Eurorack panel with DSP,
  behaviour-gated, compiled, packaged, installed, visible in Rack after restart.
- **Prompt → patch**: a whole rack wired and explained, cable by cable, with the
  *why* for each.
- **The working screen**: the generator's live output — which gate rejected an
  attempt, a retry starting, a capability refusal naming free modules that would
  satisfy it. Plus the rack compositing as it is wired, cables drawn, hovering a
  line lighting its cable.
- **Random** offering a real spread, never the same suggestion twice running.
- **The shelf** listing your actual patches and modules, each openable.
- **Settings**: agent settings inherited from Forge, model roles cut by artifact.

### Evidence, committed

- **A/B sheets** per screen: Forge Instrument vs Forge Modular vs the prototype,
  each with a difference number and a written list of every remaining difference.
- **No-leak baselines**: renders of all three existing Forge shells, asserted
  byte-identical.
- **Test suites** you can run: interaction, engine, generation, format
  validation — each with a negative control proving it can fail.
- **A status doc** separating what was proven by running it from what was not.

### And the guarantee about the other three products

Forge Instrument, MIDI and FX are **rebuilt, re-validated and screenshot-compared
against baselines** at the end of every phase that touches shared code. If one
pixel moves, the phase is not done.

---

## Phase 0 — Prove the safety net can fail

**Nothing else starts until this passes.** It is the only thing protecting your
existing products.

- Build all three Forge shells' standalones from `origin/main`.
- Screenshot each headlessly; commit as baselines.
- Make a deliberate one-pixel change to shared chrome.
- **Confirm all three go red.** Revert; confirm green.

**Done means:** a `ctest` case that renders each `ShellKind` and compares to its
baseline, demonstrated failing and passing.

**If it cannot fail:** stop. Shared code is not safe to touch, and we fall back to
keeping our changes as an explicit patch series instead. I will tell you, not
work around it.

---

## Phase 1 — The seam (~60 lines into Forge)

Three product-neutral changes. Forge learns nothing about VCV Rack.

1. Four `switch (kind)` functions — badge, prompt placeholder, build title,
   follow-up placeholder — become one virtual returning a `ChromeCopy` struct.
   The three shells return exactly what the switches return today.
2. The composer's action row becomes a description: left items, right items, each
   a label, icon and callback. The three shells supply what they hard-code now.
3. Two optional view hooks — one above the composer, one in the workspace — both
   defaulting to `nullptr`.

**Done means:** Phase 0's baselines still pass, byte-identical, and all Forge
tests pass. Submitted as one reviewable PR.

**Risk:** item 2 is the one genuinely shared change. Phase 0 exists for it.

---

## Phase 2 — Forge Modular runs Forge's UI

- `pulp-modular-rack` pins Forge as a read-only dependency and compiles its
  chrome unchanged.
- One new file, `modular_shell.cpp`, answering the hooks: product name via
  `FORGE_IDENTITY_PRODUCT_NAME`, its own mark, Eurorack wording, its composer row.
- The old JS shell is deleted.

**Done means:** the standalone opens showing Forge's chrome with Forge Modular's
words. A/B against Forge Instrument, with every difference named and each one
attributable to the product rather than to drift.

---

## Phase 3 — The tabs

Module | Patch above the composer, from the view hook, guarded.

**Done means, each asserted by driving it:** only one highlighted at a time; the
hero, button label and artifact badge follow the selection; Build reaches the
module generator in Module mode and the patch generator in Patch mode — **both
sides asserted**, because checking one side of a boolean is what let "Build always
made a patch" ship. Baselines still pass.

---

## Phase 4 — `@` mentions as a native overlay

Typing `@` opens a dropdown over the composer, filtering as you type.

**Done means, each asserted by driving it:** `@` opens it; typing filters; up/down
moves; Enter and click both insert; Escape and space dismiss; the three
availability states are distinguished and only installable modules insert.
Screenshot of it open, A/B'd against the prototype.

**Known unknown:** the overlay must sit over the composer and follow the caret. If
chrome cannot host that as a plain child view, it needs a second seam change —
and I will come back before making it rather than widen the PR quietly.

---

## Phase 5 — The working screen

- The generator's log streamed live, distinguishing **done**, **refused** and
  **failed**. A refusal is an answer with a next step, not an error.
- The rack compositing as it is wired, at real geometry.
- Cables drawn; hovering a wiring line lights its cable.

**Done means:** drive a real build and watch a refusal appear on screen — the one
your m5 run produced, "no drum module is installed", with the four free
candidates. Screenshot it. Cables require a vertical inset the widget bridge
lacks; in chrome's C++ that constraint disappears, and if it does not, I say so.

---

## Phase 6 — Everything else that paints like a control

Patch cards open their patch. `Module library →` opens the library. The module
shelf lists your real modules. Settings exists, with the agent settings and the
per-artifact model roles.

**Done means:** one assertion per control that something observable changed. A
control that highlights and does nothing is indistinguishable from a broken one.

---

## Phase 7 — Generation, driven end to end

- Type a prompt, click Build, get a **module** — in Rack, after a restart,
  confirmed from Rack's own log.
- Switch to Patch, click Build, get a **patch** that loads and **makes sound**.
- Capture Rack showing each.

**Done means:** the artifact on disk, Rack's log naming the plugin, and measured
audio for the patch. Not "the button path reaches the generator" — that was
claimed once on the strength of reading the code, and it was wrong.

---

## Phase 8 — Validate, sign, ship

For **all four products**:

- `auval` for each AU, `clap-validator` for each CLAP, load-probe each VST3, run
  each standalone.
- **Load every format in REAPER** and confirm its editor opens, via
  `tools/testing/daw-smoke/reaper_smoke.py`. REAPER is installed here and the
  smoke is enabled in `~/.config/pulp/daw-smoke.toml`. It is log-scrape, so
  headless-safe, and its exit codes keep a SKIP from ever reading as a PASS
  (0 PASS / 1 FAIL / 2 SKIP / 3 INCONCLUSIVE).

  **One small addition needed:** its three modes -- `reload`,
  `live-plugin-swap`, `sequence-loop-seek` -- all assume a hot-swap or transport
  scenario, and Forge Modular needs none of them. It needs "insert the plugin,
  open its editor, confirm it rendered". `insert_and_float.lua` already does the
  insert-and-open, so this is a fourth mode over existing tested machinery
  rather than new infrastructure. Counted as part of Phase 8.
- Re-validate **after** the final rebuild. Results expire when a binary changes —
  this already bit once, when the installer carried a stale binary after the fix
  was verified.
- Sign, notarize, staple, confirm Gatekeeper accepts.
- Install on m5 and confirm the app **runs and generates there**, not just that it
  launches. Two shipping bugs were found precisely because it was installed
  rather than driven.

**Done means:** four installers, every format validated, the three existing
products pixel-identical to baseline, Forge Modular generating on m5.

---

## What I will prove myself, and what I cannot

**Will, autonomously, before handing over:**

Build and A/B all four products · drive every control headlessly at real
coordinates · type into the composer and work the mention overlay · click Build
and watch the real generator run, refuse and retry · generate a module and a
patch · launch Rack and confirm from its log · **load all three formats in REAPER
and open their editors** · validate every format · sign, notarize and verify ·
install on m5 and generate there · negative-control every gate.

The standalone shares its shell with the three plugin formats, so driving the
standalone exercises the same UI code the plugins present. That is a shortcut for
*most* checks, not all of them: a format adapter can inject state no standalone
path has — a synthesized bypass parameter once made every in-DAW reload fail
while every headless check passed — which is exactly why the REAPER pass is
separate and not inferred from the standalone.

**Cannot, and will say so rather than imply otherwise:**

- **`auval` on m5 over SSH** — AU registration needs a GUI login session, so it
  fails regardless of the plugin. I will run it here and state the gap there.
- **How it feels** — latency, hover, whether the dropdown lands where your hand
  expects, whether the tabs read as tabs. I can measure a frame time and assert a
  click lands; I cannot judge whether it feels right. This is now the **only**
  thing on this list, and it is the reason m5 is the handoff rather than the
  proof.

---

## Reporting

At each phase boundary, the status doc gets: what was driven, the A/B numbers,
every remaining difference in writing, and every gate's negative control. Any
phase item I could not prove is listed as unproven with the reason — never folded
into a summary that reads as done.
