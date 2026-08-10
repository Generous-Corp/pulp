---
name: prove-before-showing
description: Prove a UI or generation feature actually works before asking a human to look at it. Covers A/B-ing an imported design against its source render, driving controls headlessly so a dead button cannot ship, proving the generator really spawns, and launching the real host (VCV Rack, a DAW). Use before any "take a look" on imported UI, plugin shells, or prompt-to-artifact pipelines.
---

# Prove it works before showing it

A build is not proof. A screenshot is not proof. A passing suite is not proof
that the thing a human will do actually works. This skill is the checklist that
turns "it looks right" into "I drove it and watched it work".

It exists because the alternative was measured. A shell was handed over that
rendered pixel-close to its design and was **completely inert** — not one
button clickable — because the verification was a screenshot. And after that was
fixed, `Build` still generated a patch whichever mode was selected, so the
headline feature was unreachable, while thirteen tests passed.

## The four proofs

Run every one that applies. Each answers a question the others cannot.

### 1. A/B against the source design

An imported design has a source render. Compare against it, every pass.

```bash
# Capture the source. Use the CLI, never the helper directly.
pulp import-design <prototype.html> --output <dir>

# Capture ours.
<app-binary> --screenshot /tmp/ours.png

# Put them side by side and get a number.
python3 tools/rack/compare_renders.py --out /tmp/sheet.png \
  design=<dir>/browser.png ours=/tmp/ours.png rack=/tmp/host.png
```

`compare_renders.py` reports the mean per-pixel difference. It is blunt but
monotone: it drops when they converge and rises when they diverge, which cannot
be argued with the way "looks close enough" can. Track it; do not let it climb.

**Name every difference you can still see.** A number near zero and a list of
five unfixed deltas is honest. A number alone is not.

Traps:

- **A DOM snapshot is not evidence of which screen is showing.** A
  self-contained prototype has every heading in its source, so a text search
  reports success for a click that did nothing. Only pixels are evidence.
- **`--screenshot` captures the render surface, not the composited window.** So
  window-level chrome — rounded corners, shadow, titlebar — will look wrong in a
  capture even when it is right on screen. Check the running app before
  "fixing" it.
- The importer captures **initial state only** today. Multi-screen capture is
  coming as deterministic CDP actions. Do not inject scripts into the prototype
  to fake it — that was tried for twelve attempts across four strategies and
  produced two distinct images.

### 2. Drive every control, headlessly, in a test

A control that looks pressable and is not is worse than no control.

```cpp
// Click through the ROOT at the control's centre, the way a mouse would.
const auto b = absolute_bounds(*control);
REQUIRE(b.width > 0.0f);              // a control nothing can hit is not one
root->simulate_click({b.x + b.width * 0.5f, b.y + b.height * 0.5f});
```

**Never call the handler directly.** Calling `on_click` passes for a control
that is unreachable, invisible, zero-sized or buried under a sibling — the exact
bug class this catches.

Assert, per control:

- the effect reached the collaborator **once**, with the **exact** input
- **both** sides of every boolean. Asserting one side is not asserting the
  boolean: a mode test that selected Patch and checked for `true` passed while
  the mode was hard-coded `true` and the other mode was unreachable
- the destructive path cannot fire on the non-destructive control
- an empty or invalid input does nothing rather than something

Traps, all paid for:

- **`hit_test()` returns the DEEPEST view.** A label inside a button swallows
  the click: pressing the word "Build" does nothing while pressing the padding
  beside it works. Make control contents `pointer-events: none` by default.
- **A `ToggleButton` fires when a sibling in its radio group turns it off**, so
  handlers that ignore the value run on the way down too. Act on the press.
- **`__dispatch__` swallows handler exceptions.** Define `__dispatchError__` or
  a throw while restyling silently takes the navigation with it.
- **A widget-type cast that fails is silent.** `dynamic_cast<ToggleButton*>` on
  a styled row returns null and the wiring reports it by returning `false` to
  nobody. Make that path fail loudly.
- **An unknown style key is ignored without complaint.** Validate keys against
  the bridge's own source, not a list you maintain.
- **Check the checker's inputs.** A UI checker here validated bridge names
  against a *different checkout* for its whole life.

### 3. Prove the generator really runs

"The button path reaches the generator" is a claim about code until something
spawns.

Point the real client at stub scripts that record their arguments — a genuine
spawn with only the expensive part stood in:

```cpp
::setenv("FORGE_MODULAR_TOOLS", stub_dir.c_str(), 1);
auto engine = make_engine();          // the REAL one
engine->submit("a 12 hp wavefolder", /*patch_mode=*/false);
// assert: which script, which subcommand, and that the prompt survived quoting
```

Assert the **subcommand**, not just the script. A generator invoked without it
may inventory instead of generating and *succeed at doing nothing*.

Then run it for real once, from the click, behind a hidden tag so it never runs
in CI:

```bash
<test-binary> "[.e2e]"     # spawns the real generator; minutes, paid API
```

Traps:

- **A background task's exit 0 can be the wrapper's, not the command's.** Read
  the log. A generation here threw a traceback while the task reported success;
  an artifact count moving by one with an empty log is what prompted looking.
- **Detached work outlives the click.** Wait bounded and fail rather than hang —
  an e2e check that can hang forever gets disabled, and then nobody runs it.

### 4. Launch the real host

The format validators prove a plugin *scans*. They do not prove anything
*happens*. Load it where a user would: VCV Rack for a module or patch, a DAW for
AU/VST3/CLAP.

For Rack specifically:

- **The log is better evidence than a screenshot.** `Loaded plugin <Name>
  <version>` in `~/Library/Application Support/Rack2/log.txt` is the host
  confirming it, and it survives a modal covering the window.
- **Launching opens an audio device.** Say so in the message that dispatches it,
  cap the run, and quit gracefully.
- **Never hard-kill Rack.** It truncates the log and the next launch opens a
  crash-recovery modal whose default button **clears the patch** — someone
  else's work.
- **A new module needs a Rack restart** (`plugin::init()` runs once); a patch
  loads instantly.
- **A fresh `.vcvplugin` reads as an archive** until Rack unpacks it on load, so
  it can look entirely uninstantiable.
- **`auval` cannot pass over SSH** — AU registration needs a GUI login session,
  so it fails regardless of the plugin. A first-run failure on a freshly copied
  AU is usually the registration cache: `killall AudioComponentRegistrar`.

## Every gate ships with a negative control

Non-negotiable, and the reason is a test written here that passed **with and
without** the fix it was meant to prove — the fixture's stepped parameter was
already quantized upstream, so it could never have failed.

So: break the thing on purpose, watch the check go red, restore it, watch it go
green. If it does not go red, the check is decoration.

```bash
# the shape
<edit the fix out>;  <run> # expect FAIL
<restore>;           <run> # expect PASS
```

**Failing for the wrong reason counts as failing.** A check that rejects
correct material is not a strict check, it is a broken one.

## Before you say "take a look"

- [ ] A/B sheet captured, difference number recorded, remaining deltas named
- [ ] Every control driven headlessly; both sides of every boolean asserted
- [ ] The generator proven to spawn, with the right subcommand and intact input
- [ ] One real end-to-end run from the click
- [ ] Loaded in the real host, confirmed from the host's own log
- [ ] Every new check negative-controlled, red then green
- [ ] Formats re-validated **after** the last rebuild — results expire when a
      binary changes

If any line is unchecked, say which and why, rather than showing the work as
finished. Around twenty gates in this project were wrong the first time they met
real material, and every one was found by running it. None by reading it.
