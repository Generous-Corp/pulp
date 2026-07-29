# Goal — Forge Modular, proven working before anyone is asked to look

Paste everything below the line. It assumes nothing about the session that
wrote it.

---

Finish **Forge Modular** — a sibling to Forge for VCV Rack that turns a prompt
into a Eurorack module or a whole patch — to the point where it is **proven to
work by tests you ran**, then installed on a second machine.

Work in `/Volumes/Workshop/Code/pulp-modular-rack` on branch
`explore/modular-rack`. Only `tools/dsp_vocabulary.py` and its self-test have
gone to `main` (PR #6820, merged); nothing else should without being asked.

**Read first:** `planning/2026-07-29-forge-modular-build-status.md` in
pulp-planning, then `DECISIONS.md`, then `planning-draft-forge-modular-ux.md`
(§11 capabilities, §12 agent settings, §13 the DAW plugin). **Keep the status
document current. It is the handoff.**

## The rule that matters most

**Do not show anyone work you have not proven works.** Not "it builds", not
"it renders", not "the log says the window opened" — *works*, meaning you drove
it and watched it do the thing.

This was violated and cost the user's time. A shell was handed over that
rendered correctly in a screenshot and was **completely inert**: not one button
was clickable. A screenshot had been treated as proof of function. It is proof
of paint and nothing else.

The related history: every gate written for this pipeline was wrong the first
time it met real material — manifest rules rejecting correct modules, the
behavioural gate failing six of eleven working ones, the preflight reading
"hat" out of "that", the explainer calling cross-modulation self-modulation, a
UI check passing a shell whose every label was unparented, an installer that
silently shipped no modules, an installed app that came up blank while its log
reported a window and a GPU, and a test that passed **with and without** the fix
it was written to prove. Around twenty instances. **Every one found by running
it. None by reading it.**

So: anything that checks, rejects or explains ships with a corpus it must pass
and a negative control it must fail; failing for the *wrong reason* counts as a
failure. Render before reasoning about a design. And **drive before claiming**.

## The architecture error to correct first

Forge Modular's UI was built as ~700 lines of `createRow` / `createCol` in
`ui/main.js`. Those are styled boxes. They have no `on_click`, no hover, no
focus ring, no cursor. That one choice is why the shell is inert **and** why it
looks flat and square next to Forge.

**Forge does it differently and Forge is the brand.** `/Volumes/Workshop/Code/forge`
builds its chrome in **C++** (`src/chrome.cpp`, ~1800 lines) from real widget
types — `TextButton` with `on_click` and `set_style(primary)`, `TextEditor`,
`setCornerRadius` — and its `ui/main.js` is **129 lines**.

Match that. Real widgets, chrome in C++, JS kept small. `wire()` in `shell.cpp`
already `dynamic_cast`s to `ToggleButton` and silently gives up when the cast
fails, which is exactly what happened; whatever replaces it must fail **loudly**
instead.

## The work, and the proof each piece owes

### 1. Make the shell real and interactive

Rebuild on real widgets. Every control must be clickable, show hover, and take
focus where relevant.

**Proof owed — an automated interaction harness, not a screenshot.** Headless,
in `ctest`, using `View::simulate_click` / `simulate_drag` / `simulate_hover`
and direct `on_text_input` / `on_key_event`. It must assert:

- clicking `Build` with text in the composer calls the engine **once**, with
  that exact text, and with `patch_mode` matching the selected tab
- clicking `Ask` calls the engine with the non-mutating flag — an Ask turn must
  never be able to rewrite the patch
- clicking `Build` with an **empty** composer calls nothing
- clicking the `Module` / `Patch` tabs changes mode, and the heading, subtitle,
  button labels and artifact chip all follow
- the mention picker opens on `@` and inserts what is chosen
- every rail icon and shelf card that looks pressable **is** pressable

**Negative control:** delete a handler and confirm the harness goes red. A test
that passes against a broken build is worse than no test — that already
happened once here.

### 2. Reach 1:1 with the design, and prove it three ways

The user wants a **three-way** comparison, every time:

1. **the source** — the prototype HTML rendered
2. **our render** — the standalone and each plugin format
3. **VCV Rack** — what the generated module or patch actually looks like in Rack

Capture all three, put them side by side, and list every difference you can
still see. Known-open: rounded window corners (see below).

Use **`pulp import-design`**, never `capture.mjs` directly. Both spellings of
the network flag work now. Initial-state capture works; multi-screen capture is
coming as deterministic CDP actions (click / type / wait) against Pulp's own
isolated Chrome profile. **Do not inject scripts into the prototype or fight the
shared Chrome profile** — twelve attempts, four strategies, two distinct images.

`dom-snapshot.json` is **not** evidence of which screen is showing: the
prototype is self-contained, so every heading appears in it regardless. Only
pixels are evidence.

### 3. Prove generation end to end, by running it

Not "the button path reaches `patch.py`". Actually:

- type a prompt, click `Build module`, and get a **module** that appears in Rack
- switch to Patch, click `Build patch`, and get a **patch** that loads and makes
  sound
- capture Rack showing each result

### 4. Build and validate every target

All four formats plus the `.vcvplugin`. `auval` for the AU, `clap-validator`
for the CLAP, the load probe for the VST3, a run for the standalone.
**Re-validate after any change that rebuilds a binary** — results expire.

### 5. Sign, notarize, install, and confirm on m5

Credentials are in `~/.config/pulp/secrets/`; `tools/scripts/ensure_signing_ready.sh`
prepares the keychain. Confirm on m5 by **driving** the app, not by launching it.

## What is already true (verified this way, not assumed)

- Module generation 8/8 across a spread, every one using Pulp DSP.
- Patch generation 8/8, gated so a silent patch is rejected and retried.
- Behaviour gate 12/12 plus a negative control. Patch lint 13/13. Preflight 8/8.
- AU passes `auval`; VST3 loads and returns a factory; CLAP 33 pass / 2 fail
  (see below); standalone runs.
- Signed + notarized installer, Gatekeeper-accepted on a second machine, and it
  carries app, three plugins and the `.vcvplugin`.
- The app renders identically on m5 — but **renders only**; it is inert there
  too.

## Facts that are true and easy to get wrong

- **The CLAP's 2 failures are not a defect.** `clap-validator` draws each
  stepped parameter's new value with `random_range(range).round()` from a
  hard-coded seed (`0x1337_6767`, `src/tests/rng.rs:17`) and fails if nothing
  changed (`params.rs:243`). Forge Modular's only parameter is the synthesized
  Bypass — two legal values, sitting at 0. `tools/clap_param_probe.c` shows
  flush applies values before and after `activate()`. Do not "fix" this.
- **A real defect nearby, unfixed:** a bypass parameter accepts and returns
  non-integral values while advertised `IS_STEPPED`, because `StateStore`
  quantizes discrete parameters but not bypass. Rounding in `clap_params_flush`
  fixes it. The obvious test is a **false green** — the `test_clap_entry`
  fixture's stepped parameter is discrete and already quantized upstream, so it
  passes either way. A real test needs a bypass parameter in the fixture.
- **Rounded window corners** are a *window* property, not a widget one.
  `set_client_decoration()` exists, does the right thing on macOS, and has **no
  callers anywhere** — wiring it up would change every Pulp standalone. Also
  macOS rounds windows itself while `--screenshot` captures the render surface,
  so a capture may show square corners on a window that is round on screen.
  **Settle that by looking at the running app before building anything.**
- The bundle must carry its own `ui/`. `FORGE_MODULAR_UI_DIR` is an absolute
  source path; without the bundled copy an installed app opens blank with no
  error. Source is tried **first** so editing `ui/main.js` still works on the
  build machine — the other order makes the stale bundled copy win every time.
- `package.sh` on the **signed** path had to be taught to include the
  `.vcvplugin` (before signing) and to pass `--no-notarize` rather than
  `--notarize`, which the combined-installer script rejects.
- A new module needs a **Rack restart** (`plugin::init()` runs once). A patch
  loads instantly. This asymmetry shapes both flows.
- Rack does **not** silently drop missing modules: it names them, offers the
  Library, keeps their cables.
- Rack unpacks a `.vcvplugin` **only on load**, so a fresh one reads as an
  archive and looks uninstantiable.
- Nothing on disk describes a module's ports; **index order is not visual
  order**. Model slugs are **not unique** across the library.
- **No plugin can instantiate another** or tell its host to open a file.
- A first-run **`auval` failure is usually the registration cache**:
  `killall AudioComponentRegistrar`. Over SSH it fails regardless — AU
  registration needs a GUI login session.
- The **AU factory symbol** derives from the CMake target: the bundle wants
  `<target>AUFactory`, the SDK emits `<ClassName>Factory`, so the class must be
  `<target>AU`. Renaming a target leaves stale object dirs that link cleanly
  and export the old symbol.
- **`createLabel` is `(id, text, parent)`** and every widget but the root needs
  a parent as the second argument. `setFlex(id,"display","none")` is a no-op;
  `setVisible` hides. `createPanel` does not take a background the way a row or
  column does. `setOverflow("hidden")` does not clip a child's background.
  There are `start` / `end` insets but **nothing vertical**, so absolute
  placement controls one axis only.

## Etiquette

Launching Rack or the standalone opens an audio device — say so in the message
that dispatches it, cap the run, and quit gracefully rather than killing it (a
hard kill truncates Rack's log and triggers a crash-recovery modal that
swallows the next patch argument). Never regenerate while Rack is reading the
plugin.
