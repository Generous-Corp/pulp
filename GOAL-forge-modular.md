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

(The helper is on `feature/browser-solved-html-import-20260728` at
`tools/import-design/browser_capture/`. Its blocked-network error tells you to
use `--allow-browser-network`, which **does not exist** — the flag is
`--allow-network`. Worth fixing upstream.)

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
