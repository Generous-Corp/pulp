---
name: forge-modular
description: Forge Modular's generator, patch checker, module pack and Forge-worktree seam — the traps that make green results untrue
---

# forge-modular — generating VCV Rack modules and patches, and proving it

Covers `tools/rack/` (the generator, the patch idiom library, the gates),
`examples/forge-modular/` (the module pack), and `forge-seam/` (the app's
sources, which live here and are built inside a Forge worktree).

Read this before trusting any green result in this area. Everything below cost
a session at least once, and most of it was a test or a tool reporting success
for work it had not done.

## Diagnose from a saved attempt, not from another run

Both gates are standalone binaries that take a patch and a plugin directory, so
replaying a failure is free where reproducing it costs a dozen model calls:

```bash
PATCH_GATE_TRACE=1 DYLD_LIBRARY_PATH="$HOME/SDKs/Rack-SDK" \
  ~/.cache/forge-modular/patch-gate <patch.vcv> \
  "$HOME/Library/Application Support/Rack2/plugins-mac-arm64"
```

`prove_idioms.sh` keeps every failed attempt's patch and full report under
`tools/rack/patch_idioms/regressions/idiom-proof-logs/NN-attempts/`. Three
separate defects were found and fixed there in the time one model call takes.
If a failure is not diagnosable from what is on disk, fix the harness first —
that is cheaper than another run, every time.

## Reading the patch gate's trace

- `out0=...` is the **instantaneous** voltage at the end of the run. The
  silence verdict is a separate figure: `mean_abs` over the whole run, and only
  for cables into the audio interface. A module showing `0.000` in the trace is
  not necessarily silent, and vice versa.
- `ch=` is `outputs[0].channels` — the FIRST output's channel count, not the
  module's. It is a red herring nine times out of ten; a whole diagnosis was
  built on it once and had to be thrown away.
- `| pN=value` after the pipe is the params. **Check them before the wiring.**
  A level, gain or amount knob MULTIPLIES its CV, so `p0=0.000` on a VCA is
  silence however hard its envelope fires — and the gate's advice used to say
  "its CV never rose", which sent five attempts to re-trigger an envelope that
  had been firing the whole time.

## What the model knows about a module

`render_inventory()` in `patch.py` is the catalogue the model receives. It
carries ports AND params (name, range, default). It did not carry params for a
long time, and the model wrote values blindly — which is how a kick drum came
back with its VCA level at zero. If you add a field to a manifest that the
model should reason about, it has to reach `render_inventory` or it may as well
not exist.

## Rendering a view in a test, when the view places itself

Two of these cost three attempts each, and both produce an EMPTY frame that
passes every assertion around it:

- **The overlay needs a parent.** `MentionOverlay::build()` returns a view that
  positions itself absolutely, which resolves against a containing view. Render
  it as a root and nothing is placed. Wrap it in a `View`, size the wrapper,
  `layout_children()`, render the wrapper.
- **`build()` comes before the content.** Rows are made into the view `build()`
  returns, so filling the list first leaves them nowhere to go.
- **Size the frame to what you want to see.** The list sits ~448 points below
  the composer; a 300-point frame renders it off the bottom.

So any render test here needs a floor — `REQUIRE(read_all(path).size() > N)` —
or "it rendered" and "it rendered nothing" are the same result. The same trap
in the key path: a test that CALLS `on_global_key` passes while the window
never reaches it, because the host dispatches to its own root and the shell's
view is a child of an outer chrome. Wrap the shell's view in a parent, or the
broken arrangement is indistinguishable from the working one.

## The idiom checker rejects correct patches, and it fails intermittently

`idiom_check.py` asserts a patch against a claimed idiom. Its failure mode is
not "lets bad patches through" — it is rejecting correct ones, which the code's
own comment calls the worst thing a checker can do.

Because a generator can express one correct answer several ways, a too-narrow
rule fails at random: **a pass rate that moves between runs with no code change
is a checker problem wearing a generator's clothes.** Today's rate went
6 → 11 → 10 → 12 → 10 → 12 with the model untouched.

Things that have been too narrow:

- **Relaying.** A signal through a multiple, attenuator, slew, switch, mixer or
  quantizer is still that signal. Those roles are `transparent` in
  `_roles.json`. Two traps: the relayed module's OWN jacks are generic (a
  mult's outputs are `1 2 3`, role Cv, whatever is fed in), so the kind must be
  checked at the cable ENTERING the chain; and `relayed` must be tracked
  separately from `reached`, because when `from_module` is `any` every module is
  a candidate already and "was it added by widening" is always false.
- **Labels vs roles.** `_port_matches` falls back to labels for modules nobody
  has cartographed. A port kind can list `not_ports` — roles that rule it out
  whatever the label says. `gate_out` accepts `SQR`/`PLS` (an LFO's square
  fires an envelope) but rules out role `Audio`, or an oscillator's pulse
  counts as a clock at audio rate.
- **The same jack under two names.** Our manifests call a V/OCT input role
  `Pitch`; the vendor inference calls it `Cv`. A requirement naming only one
  holds for Fundamental's VCO and not for ours.

`at_least` under-states what a patch needs: a requirement names the roles at
both ends of a cable and only some appear there. `patch_vocabulary.needed_modules()`
derives the real list from both. The self-test cannot catch this on its own —
its fixture builds from the TOPOLOGY, inventing whatever role a requirement
names, while the model gets `at_least`. The instrument knows more than the
thing it tests.

Widen only with `test_idioms.py` watching: 54 idioms, 114 negative controls and
the textbook fixtures. The corpus has already caught a "fix" that broke
Fundamental's kick.

## The module pack builds green and fails at load

A Rack plugin is a shared **MODULE**, linked with `-undefined dynamic_lookup`.
A source list missing 25 of 28 files produces a dylib that builds with exit 0
and 28 undefined `model` symbols, and Rack rejects the plugin **whole** when it
`dlopen`s it — every other module with it.

So gate on the artifact, never on the build:

```bash
nm -u build-rack/rack/ForgeModular/plugin.dylib | grep model   # must be empty
python3 tools/rack/test_pack_links.py                          # + 3 static checks
```

`examples/forge-modular/CMakeLists.txt` globs `src/*.cpp` with
`CONFIGURE_DEPENDS` for this reason. Do not replace it with a list.

The Rack SDK is developer-supplied, so most machines never configure the target
and the failure waits for whoever does have it. `tools/rack/fetch_sdk.py
--check` reports where it is (usually `~/SDKs/Rack-SDK`) — use it rather than
searching the filesystem.

## Generated artefacts and the panel shaper

`plugin.json`, `plugin.cpp`, `plugin.hpp`, `generated_modules.hpp` and every
`res/*.svg` are EMITTED from `modules/*.json`. A manifest/binary mismatch is
rejected by Rack whole, so never hand-edit an output.

The emitter needs `build/shape_text`, built by `tools/rack/build_shape_text.sh`
(it needs a populated Skia checkout — headers alone compile and fail at link).
It used to default to `/tmp/shape_text`, and when macOS cleared /tmp the panels
could not be regenerated from a clone at all.

A module built from the app lands in the INSTALLED pack only.
`tools/rack/copy_back.py` lists what is stranded; `--apply` brings it here.

## Two paths draw a module's controls, and they must agree

OURS come from the manifest a panel was emitted from — always present, exact,
never needs a scan. ANYBODY ELSE'S come from what CARTOG measured inside Rack,
because a vendor's control positions exist only in compiled widget code.

Both must follow the same rule: **draw what you know, skip what you don't.** A
slider or a switch drawn as a circle is wrong in a way a reader cannot see, so
the manifest path maps four knob kinds to diameters and skips the rest. The
measured path used to push every control as a knob sized `min(w, h)`, which
turned a vendor's fader into a small dial — the exact thing the other path
exists to prevent. `PortMap::draws_as_knob()` is the shared rule now.

An **empty** `kind` means the scan predates classification and must still draw;
refusing those empties panels that are correct today. Absent is *unknown*,
never *knob*. Same shape as the port map's scan version: "no data" and "data
saying no" are different answers and collapsing them is the bug.

CARTOG still writes `lights`, `displays` and `type` that nothing reads.
`tools/rack/test_portmap_fields.py` lists them with reasons so a dropped field
is a decision rather than an oversight, and fails on one that appears without
one.

## The port map is merged, so entries outlive their scanner

CARTOG writes `~/Library/Application Support/Rack2/forge-portmap.json` and each
scan MERGES rather than rewrites — a module measured once is carried forward
untouched forever. Entries therefore record only what the scanner of the day
knew, which is why every entry carries `"scan": N` (`PortMap::kScanVersion`).

Three states look alike and must not be confused: params recorded (known);
none, recorded by a scanner that looks for them (known — the module HAS none,
like Merge or Split); none, from one that did not (unknown). Only the scan
version separates the last two. `PortMap::controls_known()` is the rule; the
UNMAPPED badge reads it.

## Count the readers before you trust a format

Every shared format here had more parsers than anybody was checking, and the
gap is invisible because each reader works perfectly in isolation:

| format | readers | tested before |
|---|---|---|
| the success line `built N modules, M cables → path` | 4 | 1 |
| the generators' endings (`raise SystemExit`) | 4 | 0 |
| the port map | 3 | 1 |
| the `.why.json` sidecar | 1 | 1 |

The success line is parsed by `drive_app`'s regex, the app's scan for a `.vcv`,
and a `sed` of its own in each prove script — so pinning one is pinning
nothing, and a prove script that finds no path reports a PASS with nothing to
open. The port map is read by the app for drawing AND by `patch.py` for real
jack names, so a renamed field costs every explanation its labels silently.

So before trusting a format: `grep -rl` the filename or a distinctive phrase,
count what comes back, and check them all against the producer's own source.
Never against a fixture — a fixture written by the same person as the parser
agrees with it by construction, which is how three of these stayed hidden.

## Everything here reads a log, and every reader was blind

Four things watch the generator's output and turn it into a verdict: the app's
`BuildMonitor`, `drive_app.py`, `prove_surfaces.sh` and `prove_idioms.sh`. All
four were wrong in the same way, and the symptoms differ enough that they look
like four unrelated bugs:

- **The monitor** knew one of ten ways a generation ends. The rest read as
  progress, so the outcome stayed `running` and the app watched a dead build
  forever — no verdict, no artifact, nothing to open.
- **drive_app** knew two, and the rest fell through to INCONCLUSIVE — "the
  harness could not tell" when the generator had said exactly what went wrong.
- **Both prove scripts** reported `tail -2 | head -1`: an arbitrary line. That
  produced a mid-sentence fragment for a login failure and a bare `^^^^^^` for
  a crash, and both times somebody opened the log by hand.

Three questions are worth asking of any of them:

1. Can it see a run end **badly**? Every `raise SystemExit` in `patch.py` and
   `generate.py` is an ending.
2. Can it see one end **well**? A success it cannot see hangs exactly as badly.
3. Does every rule still match something a tool **prints**? A rule whose
   wording changed is a verdict silently no longer made.

`tools/rack/test_generator_endings.py` asks all three across the generators,
the monitor and drive_app. **But a textual match is only a screen**: it once
reported an ending as "recognised" that `classify()` returned as *progress*,
because it had matched a SUCCESS rule — which is worse than matching nothing,
since a failed run would then report done and offer an artifact nobody wrote.
The authoritative check runs `classify()` itself.

`reason.sh` is the shared "why did it stop" shim, for the same reason `cap.sh`
is shared: this rule already existed twice and both copies were wrong.

## Proving a surface

`prove_surfaces.sh` runs the CLI, the app by clicking Build, and a DAW.

- `FORGE_HOST=<host>` re-execs over SSH. It **refuses** rather than falling
  back when the toolchain is missing there — it silently ran locally and
  printed PASS for a long time, so any older "proved on the M5" was a claim
  about the machine it ran on.
- The **app and DAW surfaces open an audio device.** Announce before running
  them anywhere a person can hear it. The CLI surface does not: the generator
  only writes the device NAME into the patch so it makes sound when someone
  opens it in Rack.
- Over SSH the model CLI cannot reach its credential in the login keychain, so
  the CLI surface on a remote machine needs a window there or an unlocked
  keychain. That is a real blocker, not a flake.

## The seam: the app's sources live here, the build happens in Forge

`forge-seam/modular/` holds the shell and its views; `forge-seam/patches/`
holds the chrome diff and the commit it applies to.

```bash
git -C ../forge worktree add /tmp/forge-cur "$(cat forge-seam/patches/BASE)"
forge-seam/populate.sh /tmp/forge-cur     # seam -> worktree, and verifies
forge-seam/sync.sh                        # worktree -> seam
```

Both derive their file lists — populate from what the seam carries, sync from
the `foreach(_forge_modular_src)` block in the worktree's CMakeLists. They used
to carry hand-written lists that drifted, and CMake skips a missing source with
`if(EXISTS)` rather than failing, so a file populate missed was a model that
quietly was not there.

**Run `sync.sh` before finishing any session that touched the shell.** The
worktree lives in /tmp and macOS clears it; a test written there and not synced
is simply gone.

## A window-root key hook cannot have the arrows while somebody is typing

AppKit offers every key-down to `performKeyEquivalent:` before `keyDown:`, and
that path asks the **focused view** before the root's `on_global_key`. The
composer is a multi-line `TextEditor`, so it claims Up and Down for line
movement and returns consumed. Anything under the composer that wants an arrow
— an @-mention list, a completion popup — therefore never sees one, because the
field has focus for exactly as long as the list is up.

Moving the hook between views on the root cannot fix this, and it was tried
twice. The chrome exposes `ForgeChrome::set_prompt_key_filter`, consulted inside
the field before its own handling; that is the only place that wins. Keep the
root hook as well — it carries the keys when focus is anywhere else.

The tell, if this recurs: log the events reaching the root hook. A key the field
has eaten arrives **only** as `is_down=false` (the key-up), while letters and
Tab arrive with `is_down=true`. And the give-away symptom is that clicking a row
makes the arrows start working, because that moves focus off the field — which
reads as the arrows being intermittent rather than as the field eating them.

## Showing a view is a layout change, and render_to_png hides it

`set_visible(true)` + `request_repaint()` draws the view at the size it last
had; for a view that has never been visible that is no size at all. It needs
`invalidate_layout()` on the view, its panel, and the ancestors.

The headless picture tests cannot catch this: `render_to_png` lays the tree out
from scratch on every call, so a notice that the running app could not draw
appears perfectly in the PNG. Anything about *appearing* has to be proven
against the running window.

## Two copies of the app, and the one you are not testing

macOS searches `/Applications` **and** `~/Applications`, and a copy in the home
one shadows the installed copy for Spotlight and the Dock. Both this machine and
m5 accumulated an older, unsigned build there. There is no way to tell from the
running window which one answered, and a fix tested against the wrong binary
reports as not working — it cost a session on each machine. `setup_m5.sh` now
removes both locations; `drive_app.py` identifies the app by the **executable
path** (via `ps -o comm=`, since macOS `pgrep` has no `-a`) and refuses to drive
when a copy from another build is running.

## Driving the real window

`tools/rack/prove_arrows.py` types `@br`, presses Down, presses Return, and
compares captured regions. Two things make it a proof rather than a picture:

* a **control frame** — two captures with nothing pressed between them, which
  must be identical, so a difference afterwards is the key press and not the
  blinking caret that is in every one of these frames;
* assertions that name what changed. Its first version called Return a PASS over
  a picture of a *rack*: "the region changed" is true of any navigation.

`uidriver key <name|code>` sends a key that produces no text — `type` goes
through `setUnicodeString`, which carries no key code, so before this there was
no way to send an arrow at all. Both need Accessibility and Screen Recording, so
they run from a Terminal on the machine, never over SSH.

## Signing: `stapler validate` never says "validated"

It prints `The validate action worked!`. A checker grepping for "validated"
reported every bundle as NOT stapled in the same run that had just stapled them.
Use the exit code. `tools/rack/sign_bundles.sh` signs (inner dylibs first),
notarizes, staples and re-reports all four bundles; `--check` reports without
changing anything, and names an ad-hoc signature as ad-hoc rather than printing
an empty authority that skims past as fine.
