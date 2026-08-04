# Goal — finish Forge Modular, and prove each piece by using it

Paste everything below the line. It assumes nothing about the session that
wrote it.

---

Finish **Forge Modular** — a sibling to Forge for VCV Rack that turns a prompt
into a Eurorack module or a whole patch. Work in
`/Volumes/Workshop/Code/pulp-modular-rack` on branch `explore/modular-rack`.
Only `tools/dsp_vocabulary.py` and its self-test are on Pulp `main` (PR #6820);
nothing else goes without being asked.

**Read first:** `.agents/skills/prove-before-showing/SKILL.md` — the verification
discipline, and the reason it exists. Then
`planning/2026-07-29-forge-modular-build-status.md` (what is proven vs assumed),
then `DECISIONS.md`.

## The rule

**Do not show anyone work you have not used yourself.** Not "it builds", not
"it renders", not "the tests pass" — *used*. Every item below names the proof it
owes; an item without its proof is not done.

This is not abstract. A human found five defects in ten minutes that a green
build, a passing suite and a screenshot all missed: nothing was clickable, Build
generated a patch in either mode, Random offered one hard-coded suggestion
forever, Build left you on the home screen while the generator's refusal went to
a log nobody opens, and "My modules" did nothing. Each was a *silent* failure.

**Every fix ships with a negative control.** Break it, watch the check go red,
restore it, watch it go green. A check that cannot fail is decoration — a test
here once passed with *and* without the fix it was written to prove.

## 1 — Finish the working screen

The screen exists and navigates. It shows a one-line status.

- **Stream the generator's output into it.** The log at
  `~/Library/Application Support/Forge Modular/last-run.log` already carries
  everything worth seeing: which gate rejected an attempt and why, a retry
  starting, a capability refusal naming free modules that would satisfy it. Tail
  it and render it as it arrives.
- **Distinguish the three endings** — done, refused, failed — and say which. A
  refusal is not an error; it is an answer with a next step.
- **Composite the rack as it is wired**, replacing the placeholder. Panels
  already draw at real geometry from `patch_layout.hpp`.
- **Draw the cables.** Blocked today: the bridge exposes `start`/`end` insets
  and nothing vertical, so an overlay can only control one axis. Add the missing
  inset to the bridge, with a test, or state why not.
- **Hover a wiring line, light its cable.** The geometry is computed and tested.

**Proof:** drive a real build from a click and watch the refusal that m5
produced — "no drum module is installed", four free candidates — appear on
screen. Screenshot it.

## 2 — Wire everything that paints like a control

`tools/rack/test_ui_script.py` now rejects any `btn-`/`tab-`/`rail-`/`shelf-` id
that is not a real control. It does not yet know whether a control *does*
anything. These have no handler:

- **Patch cards** — click one, open that patch.
- **`Module library →`** — open the library view.
- **The mention picker (`@`)** — never verified end to end. Assert it opens,
  filters, inserts, and that only `ready` modules can be wired.
- **`rail-settings`** — highlights and nothing more. Needs the settings screen:
  agent settings inherited from Forge, model roles cut by artifact.
- **The module shelf** — a placeholder string. List the 25 real modules from
  `examples/forge-modular/modules/*.json`.

**Proof:** one assertion per control that something observable changed. A tab
that highlights and changes nothing is indistinguishable from a broken one.

## 3 — Reach 1:1 with every prototype screen

`compare_renders.py` gives design / ours / Rack side by side and a mean
difference — 17.5/255 on the home screen. Every screen needs the same.

- **Capture the other screens.** Blocked: `pulp import-design` captures initial
  state only; multi-screen capture is coming as deterministic CDP actions. Do
  **not** inject scripts into the prototype — twelve attempts, four strategies,
  two distinct images. Use an isolated DevTools session if a reference is needed
  now.
- **Per-artifact build animations.** Asked for early: distinct for synth, module
  and patch, and **no text under the animation**.
- **Rounded window corners.** A *window* property. `set_client_decoration()`
  exists, does the right thing on macOS, and has **no callers** — wiring it up
  changes every Pulp standalone. Also macOS rounds windows itself while
  `--screenshot` captures the render surface, so a capture can show square
  corners on a window that is round. **Look at the running app before building
  anything.**

**Proof:** a comparison sheet per screen, the difference recorded, and every
remaining delta named. A number alone is not honest.

## 4 — Prove generation from a click, both kinds, on m5

A module has been generated from a click on the build machine (`DIVIDELY`, via
the `[.e2e]` case). Still owed:

- **A patch from a click**, all the way to a rack that loads and makes sound.
- **Both from a click on m5**, which is where the shipping bugs surfaced. Two
  were found there precisely because the app was installed rather than driven.
- **Rack showing each result.** Rack's log is better evidence than a screenshot;
  `Loaded plugin ForgeModular <version>` is the host confirming it.

**Proof:** the artifact on disk, Rack's log naming it, and audio for a patch.

## 5 — Validate and ship

- **Load the three plugins in a DAW.** Never done, anywhere. This is the last
  claim resting on nothing.
- **`auval` on m5** cannot pass over SSH — AU registration needs a GUI login
  session. Either run it in a GUI session or state the gap; do not report the
  build machine's pass as m5's.
- **Re-validate after every rebuild.** Results expire when a binary changes.
  This was caught once already: the app was rebuilt, the fix verified, and the
  *installer* still carried the old binary.
- **The bypass quantization defect** is real and unfixed: a bypass parameter
  accepts non-integral values while advertised `IS_STEPPED`, because
  `StateStore` quantizes discrete parameters but not bypass. The obvious test is
  a **false green** — the fixture's stepped parameter is discrete and already
  quantized upstream. A real test needs a bypass parameter in the fixture.
- **The CLAP's 2 failures are NOT a defect** — `clap-validator` draws stepped
  values from a hard-coded seed and Forge Modular's only parameter is a two-state
  Bypass sitting at 0. `tools/clap_param_probe.c` proves flush works. **Do not
  "fix" this.**
- **87 commits, never through CI.** Before landing: the `hosting` skill-sync flag
  needs a note or trailer, and the `planning` pointer must not be bumped without
  a `Planning-Bump:` trailer.

## 6 — Close the class of bug, not the instances

Three separate silent no-ops shipped this session because a call took the wrong
arity and the bridge ignored it: `setFlex(id, "flex_direction", …)`,
`setCornerRadius(id, 12)`, and `createLabel(id, parent)`. Each was fixed alone.

- **Validate every bridge call's arity** in `test_ui_script.py`, derived from the
  bridge's own source, the way `setFlex` keys already are.
- **Make the bridge reject what it cannot honour** where that is cheap — a
  silently-ignored call is worse than an error.

**Proof:** introduce one wrong-arity call of each shape and watch the checker
reject it.

## Facts that are true and easy to get wrong

- **`hit_test()` returns the DEEPEST view**, so a label inside a button swallows
  the click. Labels are `pointer-events: none` by default (`textLabel()`).
- **A `ToggleButton` latches**, and fires again when a sibling turns it off. Use
  `onPress()` for momentary actions; act on the press only.
- **`__dispatch__` swallows handler exceptions** — `__dispatchError__` surfaces
  them. Navigation must happen before cosmetics, or a restyle that throws takes
  the navigation with it.
- **Bundles must carry their own `ui/` and `tools/`.** Both compiled-in paths
  point into the source tree; without the bundled copies an installed app opens
  blank and cannot generate. Source is tried first so hot reload survives.
- **A background task's exit 0 can be the wrapper's.** Read the log.
- **`generate.py install()` copies `res/` entries** — a directory there needs
  `copytree`, or packaging dies after all the expensive work.
- **A new module needs a Rack restart**; a patch loads instantly. A fresh
  `.vcvplugin` reads as an archive until Rack unpacks it on load.
- **Rack names missing modules** and offers the Library; it does not drop them.
- **Never hard-kill Rack** — the next launch's crash-recovery modal defaults to
  clearing the patch.
- **The AU factory symbol derives from the CMake target**: the bundle wants
  `<target>AUFactory`, so the class must be `<target>AU`. Renaming leaves stale
  object dirs that link cleanly and export the old symbol.

## Etiquette

Launching Rack, a DAW, or the standalone opens an audio device — say so in the
message that dispatches it, cap the run, and quit gracefully. Never regenerate
while Rack is reading the plugin.
