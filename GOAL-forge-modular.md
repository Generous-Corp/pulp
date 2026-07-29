# Goal — deliver Forge Modular, testable on another machine

Paste everything below the line. It assumes nothing about the session that
wrote it.

---

Finish **Forge Modular** — a sibling to Forge for VCV Rack that turns a prompt
into a Eurorack module or a whole patch — to the point where it can be
installed and used on a second machine.

Work in `/Volumes/Workshop/Code/pulp-modular-rack` on branch
`explore/modular-rack`. Only `tools/dsp_vocabulary.py` and its self-test have
gone to `main` (PR #6820, merged); nothing else should without being asked.

**Read first:** `planning/2026-07-29-forge-modular-build-status.md` in
pulp-planning (what is proven, what remains, what cost time), then
`DECISIONS.md` (the arguable calls and what would change our mind about each —
do not silently re-decide any; say so if you think one is wrong), then
`planning-draft-forge-modular-ux.md` (§11 capabilities, §12 agent settings,
§13 the DAW plugin). **Keep the status document current. It is the handoff.**

## The standing rule, learned expensively

Every gate written for this pipeline was wrong the first time it met real
material — manifest rules rejecting correct modules, the behavioural gate
failing six of eleven working ones, the preflight reading "hat" out of "that",
the explainer describing a correct cross-modulation patch as self-modulation,
the UI check passing a shell whose every label was unparented. Twelve
instances. **Every one found by running it; none by reading it.**

So anything that checks, rejects or explains ships with a corpus it must pass
and a negative control it must fail, and failing for the *wrong reason* counts
as a failure. And **render before you reason about a design** — two rounds were
lost this session to conclusions drawn from files that had never been opened.

## What is already true

- Module generation: 8/8 across a spread, every one using Pulp's DSP.
- Patch generation: 8/8, gated so a silent patch is rejected and retried.
- Four formats build. **AU passes `auval`.** All three installed here.
- The button path provably reaches `patch.py`.
- One unsigned installer, 64 MB, carrying app and all three formats.
- 22 modules in Rack, drawn with our own components.

## What is wrong, specifically

**The shell does not match the design.** The reference render is at
`/tmp/bcap-out/browser.png` — regenerate it with:

```
node <browser_capture>/capture.mjs capture \
  --browser "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
  --root "$HOME/Downloads/Design brief prototype-2" \
  --input "$HOME/Downloads/Design brief prototype-2/ForgeModular.dc.html" \
  --output /tmp/bcap-out --profile-dir /tmp/bcap-prof \
  --initial-width 1330 --initial-height 900 --dpr 2 --allow-network
```

Use **`pulp import-design`**, not `capture.mjs` directly. Both spellings of the
network flag are accepted now. Initial-state capture works; multi-screen
capture is coming as deterministic CDP actions (click / type / wait) against
Pulp's own isolated Chrome profile. **Do not inject scripts into the prototype
or fight the shared Chrome profile** — that was tried here, twelve attempts
across four strategies, two distinct images. Reach for an isolated DevTools
session only if a secondary-screen reference is immediately necessary.

Missing from `examples/forge-modular/app/ui/main.js`, all present in the
render: the **left icon rail**; the **top bar** with the `MODULE · 12 HP` chip
and the `RACK NOT RUNNING` / `VCV / VST3 / STANDALONE` status chips; the
**centred hero** (`FORGE MODULAR · FOR VCV RACK` eyebrow, *"What should the
module do?"*, subtitle); the **composer centred at ~1000 px with the
Module/Patch tabs joined to its top-left corner**; **icon+label buttons** with a
glowing teal `Build module` pill; and the **project shelf** (`Patches / My
modules`, gradient cards with module and cable counts, `Module library →`).

The structural error underneath: **the home screen has no chat/preview split.**
That is a second screen, reached after building. The current implementation
collapses the two, which is why nothing lines up.

## Capturing the other screens — what does not work

Every screen lives in the DOM at once and is switched by visibility, not by
mounting. Two consequences, both of which cost a pass here:

- **A DOM snapshot cannot tell you which screen is showing.** All four headings
  ("What should the module do?", "What should the rack do?", "Wiring",
  "Building") are present in every capture, so text search reports success for a
  click that did nothing. Only the pixels are evidence.
- **Injected click scripts did not switch mode.** Twelve attempts across four
  strategies -- `.click()` on the label, a full pointer sequence, walking the
  ancestor chain, pressing the nearest clickable ancestor -- produced two
  distinct images between them, and the second was only a hover ring.

The promising route, untried: since every screen is already rendered, force the
visibility directly rather than simulating input. Failing that, drive a real
browser over CDP. The chrome-devtools MCP refuses to attach while a browser
holds its profile, so it needs `--isolated` or a separate `userDataDir`.

## The work

1. **Rebuild `ui/main.js` to the render, 1:1.** Screenshot the standalone with
   `--screenshot` after every pass and compare against `browser.png` until they
   agree. Do not ask anyone to look before that comparison has been made.
2. **Build the working screen** — chat with role-grouped wiring lines and their
   *why* clauses, preview compositing real panels, hover-a-line-lights-a-cable.
   `patch_layout.hpp` already computes the geometry and is tested.
3. **Rebuild and revalidate** all four formats. `auval` for the AU;
   `clap-validator` for the CLAP (not installed here — install it). Never leave
   a plugin in a plug-in folder that has not passed.
4. **Sign and notarize** the installer. Credentials are in
   `~/.config/pulp/secrets/`; `pulp ship doctor` prepares the keychain.
5. **Install on m5 and confirm** — the standalone opens, the three plugins load
   in a DAW, the modules appear in Rack, and a prompt produces a module.

## Validation state

`clap-validator` is installed (`~/.local/bin/clap-validator`, built from
source). Forge Modular's CLAP runs 44 tests: **33 pass, 2 fail**. Both
failures are one cause -- after `clap_plugin_params::flush()` the parameter
values do not change, with set and with null cookies.

This is Forge Modular's own bug, not a framework one: a stock `PulpGain` built
from the same tree fails four *different* tests (`param-conversions` and the
three `state-reproducibility` variants) and passes both flush tests. The
suspect is that Forge Modular declares no parameters at all, so the only one
present is the adapter's synthesized Bypass, and flush does not apply it.

Narrowed but not fixed. Two obvious explanations are both ruled out:
`clap_params_flush` does write incoming `CLAP_EVENT_PARAM_VALUE` events through
`store.set_value` (`clap_adapter.cpp:1655`), and `maybe_synthesize_bypass`
injects the Bypass param *into that same store* before the id is detected
(`clap_adapter.cpp:291`). So the param exists and flush does write it. The
remaining suspect is the read path -- whether `clap_params_get_value` reports
the bypass param from somewhere other than the store, or normalizes
differently from `set_value`. That is where to look next; do not guess a fix,
because `PulpGain` passes both flush tests and would regress silently.

**The CLAP must not go into a plug-in folder until this passes.** The AU
passed `auval` on the previous pass and the four formats rebuild clean.

## Facts that are true and easy to get wrong

- A new module needs a **Rack restart** (`plugin::init()` runs once). A patch
  loads instantly. This asymmetry shapes both flows.
- Rack does **not** silently drop missing modules: it names them, offers the
  Library, and keeps their cables.
- Rack unpacks a `.vcvplugin` **only on load**, so a freshly installed one
  reads as an archive and looks entirely uninstantiable.
- Nothing on disk describes a module's ports; **index order is not visual
  order**.
- Model slugs are **not unique** across the library (Fundamental also ships
  VCO, VCF, VCA, LFO).
- **No plugin can instantiate another** or tell its host to open a file.
  Standalone Rack can be launched and handed a patch; a Rack Pro instance in a
  DAW can be neither.
- A first-run **`auval` failure on a freshly copied AU is usually the
  registration cache**, not the plugin. `killall AudioComponentRegistrar`.
  Nothing in the error says so.
- The **AU factory symbol is derived from the CMake target**: the bundle wants
  `<target>AUFactory`, the SDK emits `<ClassName>Factory`, so the class must be
  `<target>AU`. Renaming a target leaves stale object directories that link
  cleanly and export the old symbol.
- **`createLabel` is `(id, text, parent)`** and every widget but the root needs
  a parent as the second argument. `setFlex(id,"display","none")` is a no-op;
  `setVisible` hides.

## Etiquette

Launching Rack opens an audio device — say so in the message that dispatches
it, cap the run, and quit gracefully rather than killing it (a hard kill
truncates Rack's log and triggers a crash-recovery modal that swallows the next
patch argument). Never regenerate while Rack is reading the plugin.

## CLAP flush — two hypotheses already ruled out

Do not re-walk these:

- **Not the read path.** `params_get_value` reads `store.get_value`
  (`clap_entry.hpp:376`) — the same store `clap_params_flush` writes
  (`clap_adapter.cpp:1655`).
- **Not the host-write guard.** `ScopedClapHostParamWrite` is read only by the
  outbound listener and the two gesture callbacks
  (`clap_adapter.cpp:260-273`), where it suppresses the *event echo*. It never
  blocks the store write.

What is left, and the first thing to test: `maybe_synthesize_bypass` is a no-op
when the host quirk is off, so under clap-validator this plugin may expose **no
parameters at all** — and a plugin with zero parameters trivially cannot show a
value change after a flush. If that is the cause, flush is not broken; the
plugin simply gives the validator nothing to flush, and the question becomes
whether Forge Modular should declare a real Bypass. `DECISIONS.md` currently
answers no, deliberately. Log `params_count` under the validator before
changing anything — `PulpGain` passes both flush tests and would regress
silently.

One more data point against the zero-parameter theory above: `param-set-events`
**failed** rather than being skipped, and nine other tests did skip. A plugin
exposing no parameters would be expected to skip it. So a parameter probably
does exist and flush genuinely does not apply it. Confirm by reading
`params_count` directly — a short host that dlopens the bundle and calls it is
worth more here than another pass of reading the adapter.

## CLAP flush — diagnosed

Settled by `tools/clap_param_probe.c`, which dlopens the built bundle and calls
the params extension directly. Reading the adapter had ruled out three theories
without confirming one; one run of the probe answered it.

- The plugin exposes **one** parameter — the synthesized Bypass, id 1883404656,
  range 0..1, flags `0x31` (`IS_STEPPED | IS_BYPASS | IS_AUTOMATABLE`). So the
  zero-parameter theory is dead too.
- `flush()` **does** apply values: request 1.0, read back 1.0.
- But request **0.37** and it reads back **0.37**. A parameter flagged
  `CLAP_PARAM_IS_STEPPED` over 0..1 has two legal values, 0 and 1. Accepting
  and returning 0.37 breaks the stepped contract, and is the likeliest reason
  the validator's flush-versus-process comparison disagrees.

So the fix is quantization of stepped parameters on the way into the store, not
anything to do with flush. That is shared-adapter surface: it will change
`PulpGain` and every other Pulp CLAP, so it needs its own test and a
re-validation of both plugins. It also wants checking against the VST3 and AU
adapters, which may or may not quantize the same way.

## CLAP flush — resolved: not a Pulp defect

`clap-validator` picks each parameter's new value with
`random_range(range).round()` for stepped parameters, from a **hard-coded seed**
(`0x1337_6767`, `src/tests/rng.rs:17`), then fails if no value changed
(`src/tests/plugin_instance/params.rs:243`). Forge Modular's only parameter is
the synthesized Bypass — two legal values, 0 and 1, sitting at 0. When the
seeded draw is 0, nothing changes and the test fails on a plugin that behaved
correctly. `PulpGain` has many continuous parameters, so something always
changes and it passes both flush tests.

`tools/clap_param_probe.c` confirms the plugin side is sound: flush applies
values before *and* after `activate()`.

**So do not "fix" this.** The two failures are an artifact of a single
two-state parameter meeting a fixed seed. What to do instead is decide whether
a plugin with no automatable parameters should expose a lone Bypass at all.

### A real defect found on the way, still unfixed

A bypass parameter accepts and returns non-integral values — flush 0.37 into
Bypass and 0.37 reads back — while `params_get_info` reports it as
`CLAP_PARAM_IS_STEPPED`. `StateStore` quantizes *discrete* parameters, so this
affects bypass only.

Rounding in `clap_params_flush` fixes it, and was tried here and reverted,
because the obvious test is a **false green**: the `test_clap_entry` fixture's
stepped parameter is discrete, so it is already quantized upstream and the test
passes with or against the change. A real test needs a bypass parameter in the
fixture. Shared-adapter surface, so it needs that test before it lands.
