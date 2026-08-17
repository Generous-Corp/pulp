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

## A run that fails still has to hand something over

`generate()` returns `(patch, why, shortfall)`. `shortfall` is `None` on a pass
and a `Shortfall` when the loop ran out of attempts and is handing over the
best thing it built. It only raises `SystemExit` when there is genuinely
nothing to hand over — no attempt ever got past the lint.

The rule this encodes: **an advisory verdict is not fatal, and out of attempts
is not the same as nothing to show.** The loop used to `raise SystemExit("gave
up after 5 attempts")` after generating five patches its own transcript had
called "structurally sound" and "keeping the patch anyway", and the user got
nothing — not the patches, not a copyable reason. "Not a sequenced-voice patch
**yet**" is a wording that means the loop should keep going, not that the patch
is worthless.

Consequences to keep in step when touching this:

- The build writes the handover as `<slug>-unfinished.vcv`, prints the "built N
  modules, M cables → path" line the app parses for an artifact, then the
  `handover_report()` block, and **exits 1**. Dropping either the exit code or
  the `gave up after` phrase breaks a different reader: the shell decides a run
  ended by classifying that phrase, so without it the app watches a dead build
  forever.
- A patch that failed the **lint** never becomes a `Shortfall`. It names modules
  Rack cannot create, so handing it over offers a file that will not open.
- `Shortfall.rank` is `(severity, misses, -attempt)`, lowest wins: a patch that
  plays beats one measured silent, fewer missed requirements beats more, and
  only a tie goes to the later attempt. Rank on requirements missed, never on
  the number of LINES it takes to say so — naming installed jacks adds lines,
  and ranking on those would rate a patch worse for every jack we managed to
  name.
- `keep_attempt` is **not** gated on `FORGE_ATTEMPT_DIR` any more. It was, and
  nothing but `prove_idioms.sh` ever set it, so every app build kept nothing
  while the log claimed otherwise. The default is a stamped per-pid directory
  under `~/Library/Application Support/Forge Modular/attempts/`.

## A retry has to name a jack, not restate the concept

`name_the_jacks()` turns each failed requirement into something the next
attempt can act on. The requirement itself is a sentence the model already
believed when it wrote the patch — "the sequencer's gate has to fire an
envelope, or every step runs together" — so handing it back as the correction
produced five near-identical attempts and nothing escalated.

It joins the missing describe-strings back to the idiom's `topology` entries
(the strings ARE `req["describe"]`), then adds up to three lines per side:

- `this patch's sequencer CANNOT send it, however it is wired:
  CVfunk/PentaSequencer (its outputs are A, B, C, D, E)` — the most actionable
  line there is, and the one that ends the loop of trying the same module.
- `installed jacks that can send it: CVfunk/StepWave out1 'Sequencer Gate'`

Rules it follows, each of which was a wrong answer first:

- **A module nobody cartographed appears in neither list.** An unrecorded jack
  is unknown, not absent, and claiming otherwise invents a fact about three
  quarters of the library.
- **Plugins already in the patch sort first.** Alphabetical order offered three
  strangers ahead of the maker's own module that answered the requirement
  exactly — a worse suggestion and a bigger change, for the same reason a retry
  is told not to drop the makers the prompt named.
- **An unconstrained port kind (`any_out`/`any_in`) names nothing**, because
  every jack matches and the list would be noise.

## A table is not a fact, and a retry cannot see the run

Two lessons from the run kept in `tools/rack/test_fixtures/silent-oscillator/`.
A correctly-wired patch was silent because one oscillator produced 0.000 V, and
the model kept that oscillator for **four consecutive attempts**, adjusting its
knobs. Its params were all in range and none at a silencing zero, so retuning
was never going to work.

**The information was already there and was not usable.** The retry context
carried the entire per-module table and told the model to "find the FIRST
module in the chain whose output is 0.000". It did not. Handing over data plus
an instruction to infer the conclusion is not the same as handing over the
conclusion. `dead_module()` names it: `CVfunkSands/Zephyr PRODUCED NOTHING:
out0=0.000, out1=0.000`.

Rules that finder follows, each of which was a wrong answer available:

- **Dead means EVERY output reads zero.** A module with one quiet jack among
  live ones is working, and accusing it sends the model to rebuild correct
  wiring.
- **Blame the cause, not the consequence.** In that report `PressedDuck` also
  reads 0.000 and is what the `FAIL` line points at — it is silent *because*
  Zephyr is. The cause is a dead module with no dead module feeding it, decided
  from the patch's own cables, never from the order the gate printed.
- **Match rows to modules by POSITION, then verify the model name.** The gate
  prints one row per module in the patch's order, which survives two modules of
  the same model where a name lookup cannot tell them apart. Verified, because
  a silently wrong pairing names an innocent module. Where verification fails
  it falls back to unambiguous names; where that fails it names nothing.
- **`(not instantiated)` reported nothing, which is not zero.** Core's audio
  interface is in that state in every patch, and calling it dead would name the
  one module that cannot be the cause.

**The more valuable half is escalation.** Each attempt arrives knowing only its
own rejection, so *repetition is a fact only the loop can see* — and it was
telling nobody. The fourth prompt read exactly like the first. `generate()`
keeps `silent_runs` and `missed_runs`; from the second consecutive attempt with
the same dead module (`silence_advice`) or the same unmet requirement
(`stuck_note`) it says to **replace the module**, not retune or rewire it, and
names installed candidates. On the real evidence this fires on attempt 2, three
attempts before the model found its own way out.

A **different** dead module, or a different requirement, is progress and must
never be called a repeat: escalating on a run that is improving tells the model
to throw away a fix that worked.

`render_inventory` has the same honesty rule: a module with no recorded jacks
prints `ports: UNKNOWN`, and inputs and outputs are independent. It used to
emit both only `if m.get("inputs")`, so an uncartographed module and one with
no ports at all rendered identically, and a module with outputs and no recorded
inputs lost its outputs.

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

## Audibility has three answers, and `sounds()` no longer exists

`patch.py::audibility(patch)` returns `(AUDIBLE | SILENT | UNMEASURED, report)`.
It was `sounds() -> (bool, str)`, and the rename is deliberate: a caller written
against the boolean now fails loudly rather than reading a non-empty string as
success.

**UNMEASURED is the whole point.** A missing SDK, an unbuilt gate, a gate that
died, a run that never finished, and a gate refusing its own `PATCH_GATE_SET`
are all "this patch was never judged" — not "it passed", and not "it is
silent". The boolean forced that choice and got it wrong: with no SDK it
returned `True` and a run printed audibility as passed having measured nothing.
That is the same defect as a gate that measures presence, one layer up — the
absence of a failure reading as the presence of a pass.

`generate()` keeps an UNMEASURED patch and says the doubt out loud, because a
patch that lints clean and whose audibility is unknown is worth more than no
patch. It used to decide that by sniffing `GATE_CRASHED` out of the report's
wording, which covered the crash and nothing else — so the no-SDK case took the
other branch and printed nothing. The verdict carries it now; don't reintroduce
a string match.

**Audibility and behaviour are independent layers.** `held.vcv` — a bare VCO
into the interface — is genuinely `AUDIBLE` and genuinely fails `melodic`. Both
are correct. Presence was never the property; it is also not nothing.

## The gate measures behaviour; it never judges it

`patch-gate` runs 6 s of the real DSP and prints TWO things about every cable
into the audio interface: the old presence verdict (`mean_abs`, pass/warn/fail)
and a `behaviour` block that is measurement only — pitch variety, attacks,
level over time, brightness over time. **Nothing in the behaviour block can fail
the gate.** Whether a patch's numbers are the right numbers is a question about
what was ASKED for, and the request is not in that process.

- The verdict lives in `patch_behaviour.py`, which maps an idiom's `behaviour`
  flags to predicates over those numbers. Every threshold is data in
  `patch_behaviour_thresholds.json` and **every one is an untuned guess** —
  first cuts, not measured truths. When a threshold disagrees with somebody who
  listened, the listener is right; edit the JSON.
- `UNMEASURED` is not `fail`. A flag whose `needs` are unmet (a patch whose
  pitch was trackable for 8% of the run) comes back unmeasured, because
  rejecting on a number that was never really measured teaches the model to
  satisfy the number instead of the request.
- **Read `voiced_fraction` before believing `distinct_pitches`.** A staccato
  patch whose notes are shorter than `min_note_windows` (2 windows = 100 ms)
  reports far fewer pitches than it plays. That is the measurement being
  conservative, not the patch being wrong; `PATCH_GATE_SET=min_note_windows=1`
  re-runs it looser, and the report records which setting produced it.
- `periodicity` is **not reported at all** below two onsets. A held tone's
  detection function is flat to within arithmetic noise, and flat noise
  autocorrelates at 0.99 — so a drone used to announce a confident pulse at a
  meaningless lag.
- **`patch_behaviour.hpp` is standard-library only, and that is load-bearing.**
  The gate stays a one-file `clang++` compile against the Rack SDK and nothing
  else, which keeps the SDK the ONLY thing standing between it and a machine
  that can build it. It also detaches the analysis from the capture: the f0
  estimator (cumulative mean normalized difference) and the radix-2 FFT are
  written out here rather than pulled from `pulp/signal`, so
  `test_patch_behaviour.cpp` builds with `clang++ -I tools/rack` and no SDK,
  no Rack and no Pulp target. That half is gateable anywhere today. **Do not
  add an include path to this header** — a `-I` or a `-framework` in either
  build line means the property is gone, and `check_behaviour_is_measured`
  fails when it happens.
  (Linking `pulp/signal` in was tried and reverted; both estimators produced
  numbers identical to Pulp's `YinTrackerT` and `FftT` on all eight synthetic
  signals and both real patches, so nothing was traded for it. The Rack SDK is
  GPLv3 — see `tools/cmake/PulpRack.cmake` — and while the shipped `.vcvplugin`
  deliberately does combine MIT `pulp/signal` with it under Rack's plugin
  exception, this binary has no reason to need the boundary considered at all.)
- **The gate rebuilds on a header edit, not just a `.cpp` edit.** `GATE_HEADERS`
  feeds the staleness check; without it, editing `patch_behaviour.hpp` silently
  reran the previous binary and the change appeared to do nothing.
- `build_gate()` returns `(binary, reason)`. A compile failure used to reach
  the user as "no SDK", which sent the diagnosis to the wrong place.
- `PATCH_GATE_SERIES=1` adds the per-window tracks (semitones, RMS, centroid,
  onset times) to the JSON. They are the whole diagnosis when a number reads
  wrong and dead weight when it does not, so they are off by default.

`test_patch_behaviour.cpp` is the proof, and it needs no Rack: it synthesises a
held tone, a five-note line, steady/jittered/accelerating pulse trains, a filter
sweep, a decaying tone and silence, and asserts each measures as itself.
`--prove` additionally runs every signal's expectations against every other and
**fails when an expectation set describes nothing in particular** — which is how
a suite that measures nothing goes green. It caught one on its first run.

## What the model knows about a module

`render_inventory()` in `patch.py` is the catalogue the model receives. It
carries ports AND params (name, range, default). It did not carry params for a
long time, and the model wrote values blindly — which is how a kick drum came
back with its VCA level at zero. If you add a field to a manifest that the
model should reason about, it has to reach `render_inventory` or it may as well
not exist.

In a long-lived DAW host, never cache Rack's plugin directories at Python
import time. The directory may be created by the bundled-pack installer or a
Rack launch after the generator was loaded. Resolve it at the point of use;
when `RACK_PLUGIN_DIR` is set, treat that one sandbox-visible directory as the
authoritative install and scan destination.

“Only” or “exclusively” naming a maker is an output contract, not merely prompt
copy. Preflight must prove at least one named-maker module is installed, and
the retained patch may contain only those maker plugin slugs plus Rack's Core
infrastructure. Reporting a substitution does not satisfy an exclusive request.

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

## Intent anchors are constraints, not decoration

`@Maker` and plain role words are retrieval guidance unless the user says
`only` (or equivalent). An exclusive maker request is a closed final-patch
constraint: allow Core I/O, reject every other maker after generation, and do
not silently substitute one on a retry. `#Tag` is explicit: resolve it against
the current VCV index with forgiving case/plural matching, pass its candidates
to the model, and require at least one final module for each explicit tag.
Ordinary words such as `quantizer` and `sampler` remain useful cues, not
surprise preflight refusals. Keep these checks after parsing and linting so a
model cannot claim compliance in prose while returning the wrong patch.

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

## Ranges are measured by a headless Rack, and both halves are traps

Parameter ranges (`minValue` / `maxValue` / `defaultValue`) come off a
`ParamQuantity`, which exists only on a widget Rack has built. So they can only
be measured from inside a running Rack — and for the whole life of the feature
they never were, because measuring them meant a person opening Rack and
dragging in the modules they cared about. `tools/rack/measure_ranges.py`
removes the person: it writes a patch, opens it headless, and reads the map
back. Two things make that work, and neither is guessable.

**A headless Rack never runs a frame, so `step()` never fires.** CARTOG's
rescan-on-count-change lives in `step()`, which is the per-frame callback:
`Rack -h <patch>` loads the patch, builds every widget, and tears the scene
down without stepping once. The widget existed and its panel loaded; nothing
ever asked it to measure. The scan therefore ALSO hangs off `onAdd()`, which
Rack fires as each module joins the scene, during patch load. Keep both —
`onAdd` is the only path a headless run has, `step()` is what keeps an
interactive session current.

**`onAdd` fires per module, in patch order, so the scanner goes LAST.** Verified
against Rack, not assumed: named first, CARTOG logs `placed alongside 1
modules` and writes a map holding none of its subjects; named last, `alongside
44`. `getModules()` already includes CARTOG itself when its `onAdd` runs, so
the count is subjects + 1. `scan()` records the count it measured, so `step()`
compares against what was actually seen rather than against a number a caller
remembered to set.

**Every automated run must be killed, and Rack holds that against the next
one.** Headless Rack has no way to exit on its own, so the harness kills it —
and the next launch decides it crashed last session and blocks in
`osdialog_message` → `NSAlert runModal` on *"VCV Rack crashed during the last
session … Clear your patch and start over?"*. There is no window and nobody at
the keyboard, so it waits forever; the log stops dead after `Creating patch
manager` and the run reports a library with no ranges in it. `--safe` is not an
escape — it skips the plugins too. Against the real user directory the failures
accumulate: measured on a working machine, 2 of 5 launches loaded the patch,
then 0 of 8.

The fix is a **throwaway Rack user directory per launch** (`make_scratch`),
with the module library symlinked in and `settings.json` + `licenses` copied
so a Pro launch stays licensed. A directory Rack has never seen has no last
session to have crashed in: 5 of 5. It also stops the scan clobbering the
user's autosave, log and open rack.

**The symlink must preserve Rack's architecture-specific layout.** In an
isolated proof, link `ForgeModular` at
`plugins-mac-arm64/ForgeModular`, not at the scratch user-directory root.
Rack ignores a root-level link, then a missing-module dialog can make a launch
look like a plugin proof unless the log also proves the expected module was
created. Keep a regression stub that rejects the wrong layout.

**Three ways a launch dies before it reaches the patch**, all of which look
from the map like a library with nothing in it:

- *Nobody closed stdin.* Headless Rack prints "Press enter to exit." and waits
  on whatever terminal it inherited — forever. Every run leaves a live Rack
  holding an audio device, and somebody has to force-quit it. Launch with
  `stdin=DEVNULL` and kill the process yourself once the map is written.
- *Rack aborts inside its MIDI init.* `rack::rtmidiInit` → `MidiInCore` →
  `getCoreMidiClientSingleton` throws, the exception crosses a `noexcept`
  boundary, and the process aborts about 170ms in — before any patch is
  parsed and before any of our code is reachable. **It is sporadic, and the
  retry is the entire remedy.** Two confident explanations for it have now
  been measured and both are wrong, so do not reach for a third without
  numbers:

  | explanation | prediction | measured |
  |---|---|---|
  | no GUI login session | fails over SSH, fine locally | a bare `MIDIClientCreate` probe: **1/400** failures in a GUI session, **0/50** over SSH with no `SECURITYSESSIONID` |
  | client exhaustion from rapid relaunch | failures cluster under hammering | **0/14** launches back to back; the single abort came from the run spaced 3s apart |

  **Do NOT wrap the launch in `launchctl asuser`.** It is the obvious fix and
  it is worse than nothing: it needs root, and without it the command is never
  executed at all — `launchctl asuser $(id -u) /bin/echo HELLO` prints nothing
  and exits 1 with "Could not switch to audit session … Operation not
  permitted". Over SSH, **50 of 50** wrapped probes failed where **0 of 50**
  unwrapped ones did. It would break the one case it gets added for.

  Claim this mechanism on the stack Rack prints (`rtmidiInit`, `RtMidiDriver`,
  `MidiInCore`) and never on "the launch failed" — a diagnosis that fires for
  every death stops meaning anything the moment it is right. Bound the retry
  separately and tightly: each abort is a crash report on somebody's desk.
- *The crash prompt*, above.

**`install_pack.sh` reporting success does not mean Rack loads what you built.**
It places the `.vcvplugin` ARCHIVE and never touches an existing UNPACKED
`plugins-mac-arm64/ForgeModular/` directory — and the unpacked directory is
what Rack loads. Same version in both places is not the documented refusal
("keeping X, which is newer"); it is a clean exit 0 with the old binary still
live. **Verify by hash, not by exit code**, and if the directory is stale move
it aside and unpack the archive over it.

**A range without a unit cannot place a number from a book.** The same idea
carries four unit systems across an installed library — measured on its
filters: `ALM018` runs −1..0, `XFMN01` 20..12000 Hz, `Rain` 0..1 normalised,
`BattalionTone` −5..5 volts. Against bounds alone, "cutoff 40 Hz" is past one
module's maximum, near another's floor, and meaningless on a third.

Scan 5 records what Rack keeps beside the bounds: `unit`, and the
`displayBase` / `displayMultiplier` / `displayOffset` that convert both ways.

    displayValue = f(value) * displayMultiplier + displayOffset
        f(value) = value                      base == 0   linear
        f(value) = log_{-displayBase}(value)  base < 0    logarithmic
        f(value) = displayBase ** value       base > 0    exponential

`tools/rack/param_units.py` is the conversion; **use `place()` and pass the
unit the number came with.** Converting without checking the unit is the
failure this exists to stop: 40 on a dimensionless 0..1 knob is out of range,
clamps to 1.0, and yields a filter wide open in a patch claiming to follow the
book. So a unit the control cannot read is a **refusal**, while a value it
understands but cannot reach is a **clamp with a reason** — different
situations, reported differently.

Written only where it is not the identity, so at scan 5 an absent
`displayBase` means linear and an absent `unit` means dimensionless; below 5
it means nobody looked. Same rule `kind` lives by. Emitting the identity for
every linear knob would add most of a megabyte of `0.000000` and say nothing.

**Bump `kScanVersion` and CARTOG's emitted `"scan"` together** — they live in
different products and `forge-seam/test_seam_patch.sh` greps both because
nothing else links them.

**A measured range can be inverted, and a default can sit outside it.** Rack's
`configParam` does not require `min < max` — a reversed knob is a legal
configuration — and a vendor may declare a default outside its own bounds (0
usually meaning "auto"). On this machine's library: **10 params across 7
modules are inverted, and 8 across 7 have an out-of-range default**, about
0.1% each of 9,307 ranged params. They are faithful, not corrupt: re-measuring
reproduces them exactly. Anything consuming the map must therefore use
`lo, hi = sorted((minValue, maxValue))` before normalising or sampling —
`(v - min) / (max - min)` divides by a negative on an inverted range and
silently mirrors the control.

**A partial sweep is the outcome that looks finished and is not.** An aborted
launch leaves its subjects exactly as they were, so a batch can end with some
modules still carrying an entry from an older scanner: present, parsing, and
quietly less than the rest of the map. `stale_scans` names them, measured
against the newest scan version anywhere in the map rather than a constant —
a constant duplicated from `kScanVersion` would go stale in the one place
whose job is noticing staleness.

**A zero here is never evidence on its own.** A wedged launch, a scanner placed
first, and a genuinely empty library all produce the same "measured nothing".
So the harness reads Rack's own log for `forge: CARTOG placed alongside N` and
fails loudly with the reason instead of reporting a total, and its verdict is
which models still lack ranges rather than whether a number went up. Modules
with no params at all (CV funk's blanks, our MULT) are not a shortfall.

## A headless Rack RUNS THE ENGINE, so a patch can be listened to

A patch had only ever been checked as a file. It can be heard, and the way in
is not obvious: `Rack -h <patch>` closes stdin and exits at once, which is why
`measure_ranges.py` gets a scan and no audio. Hold stdin OPEN as a pipe and
Rack blocks on its "Press enter to exit." — and while it blocks, the engine
runs in real time on its fallback thread. Four seconds of stdin held is four
seconds of DSP, measured: 176,401 frames at 44.1 kHz, every time.

`tools/rack/fidelity.py` is the harness built on that, and
`tools/rack/fidelity_probe.cpp` the module it drives. Three things about the
shape are worth keeping:

**The probe is a SEPARATE plugin, compiled into a scratch directory.** Not a
module added to Forge Modular: a diagnostic in the shipped set would show up in
the user's browser and outlive the question. `make_scratch` symlinks the user's
plugins one directory at a time rather than linking the arch directory whole,
which is what leaves room to add a plugin they do not have. Nothing is
installed and the user's Rack is never written to.

**Every audio interface is DELETED before the run, and the probe takes its
place.** The probe is patched with exactly what the interface was being fed, so
what gets recorded is what a listener would have heard — and nothing in the
patch can reach an output device. That is what makes the harness safe to run
unattended on somebody's desk.

**Ask Rack to leave; never kill it.** The probe flushes its capture from the
audio callback when the window fills, and from its destructor otherwise. The
destructor only runs on a clean shutdown, so the harness writes a newline to
stdin and waits.

### Acid needs eight semantic witnesses, not one convenient CV tap

`tools/rack/acid_taps.py` plans the synchronized acid capture in fixed order:
clock, raw pitch, post-slew pitch, accent, slide, effective external cutoff,
filter audio, final audio. It reads the `acid-voice` topology and the same
inventory/Cartog port roles as `idiom_check.py`; an unknown jack is
`UNMEASURED`, never "probably input zero". Feed its returned `taps` directly
to `fidelity.instrument`, whose one eight-input probe keeps every series on
the same sample clock.

There are two identities in a routed control signal and both matter. The
physical origin survives transparent multiples, attenuators, mixers, switches,
quantizers and slew modules, so pitch, accent and slide can be proven to be
three distinct sequencer output jacks. The terminal output advances through
that chain, so effective cutoff is the last external signal the filter really
receives (for example the output of a mixer combining accent and envelope),
not merely the raw accent lane. Shared physical lanes are a measured `FAIL`;
missing, uncartographed or ambiguous witnesses are `UNMEASURED`.

`evaluate_capture` is intentionally only a capture-contract evaluator. Its
`PASS` says all eight finite, equal-length series were recorded and explicitly
does not say the patch sounds like a 303. `acid_behavior.evaluate` is the next
layer: it derives eight-step windows from clock edges, requires two agreeing
loops and three pitches, proves accent and slide are selective, compares
post-slew motion on matched selected/unselected transitions, and uses repeated
equal-pitch accented/unaccented steps to require cutoff plus filter/final-audio
brightness or transient differences. Missing, unequal, short or loop-mismatched
series are `UNMEASURED`; observed silence, flat/zero cutoff, all-on selectors,
missing glide or bypassed audible response are `FAIL`. Structural tap-plan
failures stay separate from behavioral failures in the result. Do not weaken
this verdict into cable presence or subjective taste.

## A value outside a knob's range is silently clamped on load

`Module::paramsFromJson` puts a written value through the parameter's own
bounds, so a patch that writes `0.0` into a knob whose minimum is `0.001`
loads holding `0.001`. Nothing reports it: the file keeps the number it was
written with, the module holds a different one, and the only symptom is the
patch not doing what it says. Found on a real generated patch —
`bouncing-ball-never-bounces.vcv` writes ENV Attack `0.0` — and reproduced from
both ends, by reading the engine back and by predicting it from the port map's
declared range. `fidelity.will_be_clamped()` names it before Rack is launched
at all, which costs nothing; the port map has to have ranges for that plugin
first (`measure_ranges.py`).

## A pitch reader will invent notes unless it is made to answer for itself

Four readings out of the first analyser were the instrument rather than the
patch, and each one read as a finding:

* **A confident 8.9 kHz on two unrelated patches, identical to four decimals.**
  Autocorrelation is near one at no lag and falls away, so the largest value in
  the curve is the SHORTEST lag the search can see, not the period. Start the
  search after the correlation first turns negative.
* **A sawtooth an octave low above a kilohertz.** Taking every fifth sample
  folds everything above the new Nyquist back into the search. Average down,
  do not stride down.
* **Zero tracking error on a drone.** One held note tracks any tuning
  perfectly, because there is no interval to get wrong — so a CV that never
  moves has to report that nothing was tested, not that everything passed.
* **An envelope-gated patch reading as no notes at all.** Requiring every note
  to hold two windows throws away plucks. Silence AROUND a reading is what
  tells a short note from the boundary between two long ones.

Two numbers that look identical are the tell for a reading that came from the
instrument. And "silent" and "unreadable" are different findings: report the
recording's level, or a patch that plainly sounds reads as one that makes
nothing.

## A zero is usually your instrument, not the world

The single most expensive habit on this project is believing a measurement
that returned nothing. **Absence is what a true negative and a broken
instrument look like from the outside**, so a zero is the one reading that
must be re-taken before it is reported. Every one of these was confident,
plausible, and wrong:

| the reading | the instrument | what was true |
|---|---|---|
| `0 modules carry ranges` | probed `min`/`max` | the keys are `minValue`/`maxValue` |
| `scan version: None` | read a top-level field | `scan` is per module |
| `pack has no dylib` | `unzip` | `.vcvplugin` is Zstandard (`tar --zstd`) |
| `the EPUB is 0 bytes` | `ls -s` (blocks) | it is a directory; 2.3M chars of text |
| `no scan version in CARTOG.cpp` | grepped `"scan"` | the quotes are escaped in C++ |
| `EMITS_CLOCK_TAGS is absent` | grepped the wrong file | it is in `patch.py` |
| `the sweep died` | `pgrep -fl` | `ps` showed it running; 8 minutes in |
| `no prior installers` | a zsh glob | one non-matching pattern aborts the **whole** command |
| `no .component built` | the same zsh glob | all four bundles existed |
| `render_for returns nothing` | — | that one was real |

Nine of ten were the tool. The tenth — the instrument catalogue rendering
zero characters — was a genuine defect, and it was only believable **because
the other nine had been checked and eliminated first**.

The habits that catch these, in order of cost:

- **When two instruments disagree, resolve it — do not pick the convenient
  one.** `find` reported SDK directories empty while `ls -A` showed contents,
  because `find` does not follow symlinks. `pgrep` said the sweep was dead
  while `ps` said it was running. The disagreement is the information.
- **A zero from a query you just wrote is a bug in the query** until you have
  pointed the same query at data known to be non-empty. The port-map probe
  above would have been caught in one step by asking it for any key at all.
- **`setopt NULL_GLOB` or use `find`.** zsh aborts an entire command when any
  glob matches nothing, so a single stray pattern silently discards the output
  of everything beside it. This happened four times in one session.

## When a check disagrees with your evidence, ask what CHANGED between them

A run log said `EnvelopeArray` could not receive a gate. Ten minutes later the
live inventory said it could. **The right question was "what landed in
between"; the question actually asked was "was my evidence stale".** A fix had
been committed between the two, so a real defect was declared imaginary and
another agent was told to stop working on it.

`git log --oneline -5` would have settled it in one command, and on a tree
several agents are committing to it is never a wasted one.

Both readings were true of their moment. **A log records what happened once; a
query reports what is true now.** Neither is a statement about the other, and
reconciling them means finding the commit, not picking the more recent
instrument.

## "Does any kind accept it" cannot detect a defect when one kind is a catch-all

The corpus audit asked, for every jack a real cable lands on, whether **any**
port kind would accept it — and reported 0 failures in 319 cables, which read
as "the matcher is healthy".

It is not a health measurement. `cv_in` accepts nearly anything: a jack named
`Wobble` classifies as `cv_in` quite happily. So the question could barely fail,
and the zero it produced said almost nothing.

**The defect being hunted was never "no kind accepts this" — it was "the WRONG
kind accepts this".** `Gate 1 CV` was not unplaceable; it was placed as `Cv`,
confidently, by a vocabulary that checks `Cv` first. A test that asks whether
something matched cannot see a thing that matched incorrectly.

When designing a detector, **ask what a defect would look like in its output**
before running it. If a defect and a clean result produce the same reading, the
detector measures nothing however impressive the sample size.

## Where the patch corpus lives, and what it was actually worth

```
~/Library/Application Support/Forge Modular/corpus/patchstorage/
    index.json     per patch: id, title, author, url, licence, licence slug,
                   sha256, tags, categories, fetched_at
    patches/*.vcv  the bodies — Zstandard, `tar --zstd -xf … patch.json`
```

**Outside the repo entirely, and deliberately so** — `ditto` copies a directory
rather than git's view of it, which is how 113 MB of reference books once
reached a signed installer.

Fetched via the public beta API. Patchstorage publishes **no Terms of Service**;
`robots.txt` carries `Crawl-delay: 10` and no `Disallow`, and **every patch
carries its own licence in the API response**, which is a stronger permission
signal than a site-wide document because it comes from the rights holder. The
crawl delay is honoured and the licence is recorded per patch, so anything
non-permissive can be excluded from storage later without re-fetching.

**What it was predicted to find:** port-matching defects in bulk, over real
usage.
**What it found:** nothing, for the reason in the section above.
**What it was actually worth:** it exposed a wrong diagnosis. Building the
audit is what surfaced that a "defect" had been read off a superseded log —
which stopped a permanent loosening of the gate that would have been made for
a reason that never existed.

That is worth recording honestly rather than as a success. The corpus is more
promising as **usage-inference for unmapped modules** — if many patches wire a
module's `out0` into a `V/Oct` input, that is a pitch output, and that is
knowledge about the ~40% of the library CARTOG cannot reach because nobody has
it installed. It is a prior, never a measurement, and must never override a
real scan.

## The reading corpus has an index, and retrieval is not admission

The source shelf is large enough that opening books one by one is no longer a
credible research workflow. Build its machine-local SQLite FTS5 index with:

```bash
python3 tools/rack/corpus.py --index
python3 tools/rack/corpus.py --query "sample and hold evolving random voltage"
# Structured results for an agent:
python3 tools/rack/source_index.py query "acid accent slide" --json
```

`source_index.py` indexes only `.md`, `.txt`, and `.sc` documents already under
`~/Library/Application Support/Forge Modular/.corpus` (or
`PULP_RACK_CORPUS`). It preserves a stable source key, work identity,
extraction identity, page/section locator, source hash, and passage hash.
Re-running the build hashes every source, updates changed documents, removes
vanished ones, and leaves unchanged rows alone. The SQLite schema is versioned
and fails closed if a newer tool has already migrated the database.

Recovered subset-font text that labels itself `machine output, imperfect` is
kept in the corpus but gets **no searchable passages**. Its fragments can look
like synthesis vocabulary and manufacture excellent-looking false hits.
`source_index.py status` lists every such zero-passage source explicitly. That
means "we possess this source but need page-image review/OCR," not "the work has
nothing useful in it." Allen Strange currently falls into this category; use
its pinned page images and candidate-evidence workflow until recovery is clean.

The database lives **inside that external corpus**, never under `tools/rack`.
It contains the copyrighted source text needed by FTS and therefore follows
the same non-shipping rule as the books; it is forced to user-only file
permissions on every open. A query returns bounded snippets plus locators and
hashes; it has no dump command.

Most important: **a search hit is evidence to inspect, not guidance to use.**
Retrieval may suggest a candidate technique, repair, negative control, or
parameter relationship. It does not bypass `knowledge.py`'s candidate
quarantine, canonical-claim dedupe, source anchor, or guided validation. The
generator still receives only admitted records. This boundary is why the
index can be broad without turning unreviewed prose into prompt cargo.

## One matching rule produced five defects

`_port_matches` compares a jack's label to a role's label list **whole**:
`label.upper() in ok_labels`. Vendors name jacks descriptively, so this fails
on every jack whose name says more than the bare word:

```
"PITCH CV (1V/OCT)"  vs  cv_out labels ["CV", ...]        -> no match
"GATE 1 CV"          vs  gate_in labels ["GATE", ...]     -> no match
"SQUARE"             vs  clock_out labels ["SQR", ...]    -> no match
```

Each was found, diagnosed and fixed **separately**, as though they were
unrelated bugs, and each fix was a new entry in a list. The fifth was found
only because a run log printed the rejected jacks beside the complaint:

```
this patch's envelope CANNOT receive it, however it is wired:
  CVfunk/EnvelopeArray (its inputs are ..., Gate 1 CV, Gate 2 CV, ...)
```

**When a fix is an entry in a list, ask what the list is compensating for.**
A per-label fix costs an hour and buys one module; changing the comparison
costs the same hour and buys the class. The reason to be careful rather than
bold is real — a wider match is how a gate becomes a rubber stamp, and
`clock_out`'s `not_ports: ["Audio"]` exists because an oscillator's audio-rate
pulse once read as a clock. But "careful" means **measure the blast radius and
sweep the corpus**, not "add one more label".

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

### A guard nothing runs goes stale silently, and this one did

That test was written, was correct, and exited 1 — and **nothing ever ran it**.
It carried no ctest registration, so it drifted by **24 unmatched endings**
while reporting them accurately to an empty room. The app hung on the plainest
possible case: the curation gates end a run before anything reaches the model,
so a mistyped prompt produced a spinner that never resolved, with the reason
sitting in the log.

The list and the checker are therefore one change, never two. Fixing the
endings without registering the check just resets the clock to the next drift.
It now runs as ctest `rack-generator-endings` (labels `rack;contract`, ~0.04 s,
pure Python — no Rack SDK, so it must never be gated on `PULP_HAS_RACK`).

Two things worth knowing before trusting a green run of it:

- **Prove it can go red.** Delete one ending from `build_monitor.cpp` and
  confirm the ctest fails, then restore. `tools/scripts/confirm_failure.sh`
  automates that loop and handles the same-second-mtime trap that makes a
  hand-rolled version report a false verdict in either direction.
- **Classify a curation gate as a `refusal`, not an `error`.** Nothing broke:
  the request was understood and declined, and the wording is the whole answer
  because no model call was made. `outcome_of` ranks refused and failed alike
  as terminal, so either ends the spinner — but only one of them tells the
  truth on screen.

Registration is the general point, not a detail of this test: 42 of the 48
`tools/rack/test_*.py` harnesses have no ctest entry. Some gate on Rack or the
network deliberately; a pure-source checker never should.

### Headless runs use the app's saved model, or do not run

`tools/rack/with_app_model_selection.py -- <command>` snapshots Forge Modular's
saved engineering provider, model and reasoning effort into the same `FORGE_*`
environment the app gives `generate.py` and `patch.py`. Use `--settings <path>`
for an isolated settings file. It clears the unselected provider's model and
effort variables before replacing itself with the command, so switching from
Claude to Codex (or back) cannot inherit stale values.

This wrapper deliberately has no built-in model or effort default. It follows
only saved role-to-default fallbacks, migrates the legacy `gpt-5.6` display
alias to `gpt-5.6-sol`, and refuses a missing, corrupt, incomplete or unsupported
selection before the command starts. A headless benchmark that silently picks a
different agent than the app is not reproducible evidence.

Codex generation must also be a sealed execution: pass `--disable apps`,
`--disable plugins`, and `--disable skill_search` before `exec`. Those
interactive catalogues consume the bounded skills context and can reject a
generation before its contract reaches the selected model. Keep the exact
argv covered by both `test_generate_model.py` and `test_patch.py`.

### Seeing an ending is not the same as SHOWING it

The monitor learned to classify every ending and the shell still reported one
line of it: `BuildOutcome::failed` narrated `headline()`, which by construction
returns the ending line and nothing after it. When the generator gives up it
says considerably more — what was asked for, what the patch it is handing over
does not meet, where that patch is, where the attempts behind it went — and all
of it was discarded a second time, one layer above the bug that discarded the
patches.

- `BuildMonitor::closing_block()` returns the ending line **and everything
  after it**. It scans BACKWARDS for the last ending, not forwards for the
  first: a model call that failed and was retried is an ending-shaped line in
  the middle of a healthy run, and starting there hands back most of the
  transcript as though it were the verdict.
- The `failed` branch also **opens the handed-over patch** (`open_patch_file` +
  `save_project_for`, as `done` does). Without it the patch is on disk, the
  skeleton is still animating, and nothing on screen can reach it.
- **The run log had never been named anywhere.** It has always existed at
  `~/Library/Application Support/Forge Modular/runs/<stamp>.log`, and the only
  way to read a failure was to already know that path. The failed branch says
  it.
- `format_failure_report()` is a free function taking a `RunFailure`, so a test
  can build the copyable block with no shell, no chrome and no clipboard. It
  reads the log **back from disk** rather than rebuilding it from the lines the
  monitor kept: the monitor drops blank lines and holds only what it polled, and
  a report that quietly differs from the file it names wastes an hour when
  somebody compares the two.
- **A Label cannot be selected with a mouse.** That argument already lives in
  the comment beside the About report's Copy action, for a report nobody
  urgently needs; a failed run is the case where it cost somebody their work
  ("i didn't even get a way to copy the prompt output"). The "Copy report"
  action appears in the composer row only after a failure, and is cleared when
  the next build starts — a control offering the PREVIOUS run's failure while a
  new one is in flight hands over the wrong report at the worst moment.

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

## How long a maker's name is is not something to guess at

A mention has to be able to span spaces, because a maker's name usually has one
in it. Both halves of this originally capped the span with a literal — the `@`
list allowed two spaces, and `brand_mentions()` in `patch.py` tried phrases of
three words, then two, then one — and both literals are wrong for the same six
of the 375 makers in the real library: **Studio Six Plus One**, **The All
Electric Smart Grid**, **Path Set x Omri Cohen**, **Mathematics and Music Lab
(MML)**, **Jasmine & Olive Trees**, **Autodafe - REDs FREE**. Naming any of them
resolved to nothing at all, in silence, from either surface.

The two sides fix it differently on purpose, and neither adds a bigger number:

- **`patch.py` reads the span from the library.** `brand_phrase_span()` is the
  longest maker name in the directory, in words, so the scan is exactly as wide
  as the data it is scanning for and stays correct when a maker with a longer
  name publishes.
- **The `@` list has no cap at all.** Only a newline ends the token. What ends
  a mention is that it **stopped matching**, which the caller already asks and
  which is strictly better evidence than a word count: right for
  `@The All Electric Smart Grid`, and right for `@vco into a filter` where a
  word count is only right by luck.

Two more makers were unreachable for a different reason, and both surfaces had
their own version of it:

- **`patch.py` dropped every token containing a slash**, because
  `@CVfunk/Sphinx` names a module. **Catro/Blanco** (8 modules) and **p.s.F/X**
  (7) have one in their names, so neither could be named at all. The exemption
  is checked against the brand directory, not guessed: a slashed token is a
  module unless it *is* a maker's name.
- **`fold()` in `module_catalog.cpp` removed a space, a hyphen and an
  underscore and nothing else**, so those same two could be reached only by
  typing their punctuation exactly. It now drops every ASCII non-alphanumeric,
  which is the rule `fold_name` was already applying on the other side.
  **Keep the non-ASCII code points**: `std::isalnum` in the C locale answers no
  to a UTF-8 continuation byte, so a byte-wise test shortens `Instruō` to
  `instru` and makes it collide with anything else starting that way.

**The two folds are one behaviour, and "ASCII case-folding" is not enough to
make them so.** `fold_name` is `str.lower()` plus `str.isalnum()`, both
Unicode-aware. `fold` case-folded only ASCII, so `ÄSK` — a real maker — folded
to `äsk` in a prompt and `Äsk` in the list, and **`@äsk` found nothing**. The
other three non-ASCII makers (`Instruō`, `Hügelton Instruments`, `nozoïd`)
agreed only by luck: all three are already lowercase, the one quadrant where
the two rules cannot differ. A sweep that types every name the way its maker
writes it will never see this — vary the case.

`fold` now walks **code points, not bytes** (half of `Ä` is not a character) and
case-folds Latin-1 Supplement, Latin Extended-A, Greek and Cyrillic. Every range
was checked against Python's `str.lower()` code point by code point; the
exclusions are the whole of the disagreement (`U+00D7` ×, `U+03A2` unassigned,
`U+0130` whose lowercase is two code points). Outside those ranges a code point
passes through in the case it was written, and **the guard for that is on the
corpus, not the code**: `check_maker_names_as_written` sweeps the real index for
an uppercase character outside the covered scripts, or any non-alphanumeric
non-ASCII one (which Python drops and `fold` keeps). Widening the C++ side to
full Unicode would mean a category table no maker needs yet.

**It is case-folding, not transliteration.** `Instruo` and `Instruō` must stay
two makers; merging them is worse than the bug being fixed because it is silent
and it changes what an already-stored token resolves to. Assert that as a
**count of maker rows**, never as which row leads — with the diacritic folded
away both names match exactly and the winner is whichever the catalogue lists
first, which an assertion about `hits[0]` passes straight through. That version
was written, mutated, and did not fail.

Pinned identically on both sides: `SEAM_FOLDS` in `test_patch.py` and the table
in `test_chrome_no_leak.cpp`. Two implementations in two languages can only be
held together by the same literals on both, never by a third implementation that
could itself be wrong.

Sweep the real index before believing a matcher change is complete. Every one of
these was found by asking whether all 375 makers can be named, not by reading
the code:

```python
brands = sorted({v["brand"] for v in cat.values()})
[b for b in brands if b not in P.brand_mentions(f"a patch with @{b} in it", cat)]
```

What pays for the uncapped span is that **matching is monotone** — a query
matches when its folded form is a substring of an entry's folded name, slug or
maker, and every longer query contains the shorter one, so a token that matches
nothing cannot grow into one that matches something. The overlay remembers the
token it abandoned; anything that token grows into is dismissed with one string
compare instead of a search. That is worth having: measured over a library the
size of the real one, a 500-character dead token costs **24 ms a keystroke** to
re-answer (2 chars: 1.1 ms, 120 chars: 6.6 ms), so deleting the cache is not an
option and neither is capping the span by guessing.

Rely on that property if you touch `matches()`: widening it to a fuzzy or
subsequence match would break the proof and put an unbounded search on every
keystroke.

**And the proof is about a FIXED corpus, which this one is not.** `all()`
reloads itself when the index changes, by design, because on a machine that has
never had an index the file arrives about a minute after launch. A cached
"nothing matched" therefore expires: somebody types `@Catro Blanco` into an
empty library, the index lands carrying that very maker, and a guard with no
invalidation keeps the list shut against it — silently, and recoverable only by
deleting back past the abandonment, which nobody will guess. `abandoned_` is
paired with `catalog_generation()`, which `all()` itself bumps on reload;
deriving that number from a second stat of the same file would let the counter
and the list disagree about when the change happened.

The general shape, worth carrying past this file: **a cache of a negative
answer needs the same invalidation as a cache of a positive one**, and an
optimisation justified by a proof is only as durable as the proof's unstated
assumptions. Write the assumption into the comment so the next reader can see
what would break it.

## The `@` list ranks two kinds of row on one scale, and installedness is not one of them

Module rows and maker rows are merged into a single list, so one ordering
decides both. The tiers are named in `module_catalog.cpp` (`kExactName`,
`kAlias`, `kMakerNamed`, `kMakerPartial`, …) with `static_assert`s, because as
bare integers "a partial maker name sits under the alias tier" was a
relationship between a `3` in one function and a `2` in another that only
somebody reading both would see.

The trap is the **front tier**, not the ranks. It was "installed, an exact name,
or a maker", and a maker row is always in it — so on a machine **without**
Audible Instruments, `@br` put any maker starting with those two letters above
Braids, whose alias match is a whole tier stronger. Being uninstalled is
precisely the state a mention exists to remedy, so it cannot be the thing that
decides which row leads. The front tier is now `is_identity_match` — the whole
name typed out, **or the alias** — plus installed rows and maker rows.

The old test for this rule installed Audible Instruments first, which is why the
hole stayed open. **If a mention test flips `installed = true` on the fixture to
make the assertion pass, that is the bug, not the setup.**

**Widen that tier only against the real index, never by reasoning.** Adding the
name-PREFIX tier is the obvious next step and it is wrong: measured over the
4,735-module index, `@br` then answers with thirty modules that merely begin
with those letters (Breakout, Branes, Broadcast) and Braids *and* every maker
row fall out of the six visible rows. A prefix is too common to lead. Both
directions are pinned by tests, so the decision fails loudly rather than drifts.

The front tier deliberately **overrides rank** — an installed `kMakerPrefix` row
leads an uninstalled `kNamePrefix` one — because it answers "what may lead the
list", not "what matched best". That is by design and predates this work.

The visible consequence, worth knowing before somebody reports it as a bug: an
alias-tier module now leads a **partial** maker row, so at `@CV` the modules
whose slugs begin CV (`Core/CV-Gate`, shown as "Gate to MIDI";
`Autinn/CVConverter`, shown as "Conv") sit above the "CV funk, 43 modules" row.
That is `kMakerPartial` sitting under `kAlias`, which is what the tiers have
always said; it was simply never true for an uninstalled module before.

## A maker token has to survive a reload, so it can only be a name

Projects store the prompt (`prompts` in `project.json`), so whatever the `@`
list inserts has to go on meaning the same maker against a library index rebuilt
since. It does, and only because what is inserted is the maker's **own display
name** and nothing else: no plugin slug, no id, no module count, no syntax we
invented. `brand_mentions()` resolves it again from scratch each time, so the
same token picks up plugins the maker has added since and drops ones that are
gone. Anything that embeds a snapshot of the library in the token — the count,
the slug it happened to come from — silently stops resolving the first time the
index is rebuilt.

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

Beside those sat 513 MB and 628 MB of hand-made `.prev` / `.prev-1301` /
`.backup-<timestamp>` / `.signed-backup` bundle copies. Nothing in the repo
creates them — they are interactive "let me keep the old one" copies, and none
was ever the only record of anything, because the build directory is the
previous build and git is the history. Do not make them.
`tools/rack/clean_installs.sh` sweeps both kinds (dry-run by default, `--yes` to
remove) and `setup_m5.sh` sends the same script over rather than reimplementing
the rule, because two copies of a *removal* rule are two chances to disagree
about what is safe to delete.

## Spotlight does not notice a bundle that arrives by rsync

An install can be complete, signed, notarized, stapled and known to
LaunchServices while Cmd-Space finds nothing — which is indistinguishable from
"it never installed", and is what one "I don't see it on m5" turned out to be.
**`touch` the bundle, then `mdimport`** — and the touch is the part that
matters. `rsync -a` preserves the SOURCE directory's mtime, and a rebuild only
changes the binary inside the bundle, so the installed .app arrives looking
older than the last index pass and Spotlight skips it as unchanged. `mdimport`
on its own does nothing; the tell that you are in this state is `mdls` reporting
**null** for every attribute while `mdfind` finds 2,300 other app bundles fine.
Indexing is **asynchronous**: checking immediately reported CANNOT FIND IT for
an app indexed seconds later, so the check in `setup_m5.sh` retries for 20s.
Query it as a structured search — `kMDItemContentType ==
"com.apple.application-bundle" && kMDItemDisplayName == "Forge Modular"` —
because `mdfind -name "Forge Modular.app"` finds it even when it is indexed.

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

## One generation at a time, and one log per run

`patch.py` / `generate.py` are launched with `nohup` + `setsid` so a build
survives the window closing. Every run used to redirect into one
`last-run.log`, and nothing stopped a second Build — so two generations
overlapping wrote over each other from offset zero. That file is not just a
transcript: the shell reads the **outcome**, the **stage** and the **artifact
path** out of it, so a collision puts one patch's explanation on screen beside
another patch's filename and hands Rack the wrong file. Seen on m5, two patches
finishing six seconds apart, with the log showing an acid explanation stopping
mid-word and an ambient one continuing.

Both halves are needed. The lock (`start_build_with`) is *a build this shell
started, on a log it is watching, that has not reported an end* — **not**
`busy()`, which is `watching_ && outcome == running` and therefore true for a
shell merely watching a log nothing has written yet; using it refused the FIRST
build of every session. The lock cannot see a run left over from a previous
launch, so the engine is also asked whether a generator process exists, and each
run claims its own log under `runs/` with `O_CREAT|O_EXCL` — an `exists()` check
is not enough, because the log does not exist until the run writes it, in
another process, so two submits in the same second pick the same name.

## Signing: `stapler validate` never says "validated"

It prints `The validate action worked!`. A checker grepping for "validated"
reported every bundle as NOT stapled in the same run that had just stapled them.
Use the exit code. `tools/rack/sign_bundles.sh` signs (inner dylibs first),
notarizes, staples and re-reports all four bundles; `--check` reports without
changing anything, and names an ad-hoc signature as ad-hoc rather than printing
an empty authority that skims past as fine.

Forge Modular packaging must run Pulp's unattended signing doctor before its
first `codesign` and treat any nonzero result as terminal. Do not restore the
old `PULP_SKIP_SIGNING_PREFLIGHT` escape hatch or warn-and-continue: either can
fall through to the login-keychain copy and open an unanswerable GUI password
dialog. The prompt-safe path is the dedicated keychain, full partition list,
identity hash, and real timestamped probe maintained by
`tools/scripts/ensure_signing_ready.sh`.

## A module's WIDTH is knowable from its artwork, and from nowhere else

Four things could say how wide a third-party module is and, for a plugin
fetched five minutes ago, none of them does: a `.vcv` records no width, a
`plugin.json` carries none, the port map has an entry only for modules CARTOG
has scanned, and the generated `.why.json` sidecar copies its `hp` from the
inventory, which copies it from the port map. So a fresh maker's modules all
arrive at the fallback of 8 HP, and the preview drew a 30 HP sequencer squeezed
into a quarter of its width. That does not read as "we do not know how wide this
is"; it reads as panels stretched to the ceiling, which is how it was reported.

The panel SVG is a picture of the module at its true size, and every 3U panel is
128.5 mm tall, so the root tag's own aspect IS the width — in whatever units the
vendor drew it. `panel_hp_from_artwork()` reads it; `RackModule::width_measured`
is what distinguishes a measurement from the fallback, so the preview knows when
to go and look.

Two things follow:

- **True widths overlap.** The positions in a patch were written by something
  that did not know the widths either — five modules spaced 8 HP apart turning
  out to be 15, 30, 14, 15 and 24 — so drawing them at their real size stacks
  them. `layout_rack` walks each row left to right and moves anything that would
  start inside its neighbour to just after it.
- **Assert the drawn pixels, not the layout.** The arithmetic in `rack_layout`
  was right the whole time and the render was still wrong, because the numbers
  fed into it were a guess. A geometry test that reads `layout_rack`'s output
  cannot see that. Render with the Skia backend, find the panel's ink, and check
  `width / height` against `hp * 5.08 / 128.5`.

## The settings reader is string-only, and a number reads as the next key

`modular_setting()` finds the next QUOTED string after a key's colon. A JSON
number is not quoted, so asking it for one returns the FOLLOWING key's value —
confidently, in the right shape, with nothing to suggest anything went wrong.
`modular_setting_int()` exists for numbers. Anything numeric added to
`SETTINGS_DEFAULTS` needs it, and `patch.py setting KEY VALUE` has to convert
the argument, or the writer refuses its own choices for not being in the list.

## Streaming the model call

`--output-format=stream-json` is refused without `--verbose`. Events arrive one
JSON object per line: `stream_event` carries the partial deltas (only with
`--include-partial-messages`), `assistant` carries whole blocks, and `result`
carries the finished answer — prefer it, so there is one authority for what was
said. A blocking read on a pipe cannot time itself out and a wedged call emits
no lines at all, so the deadline has to be a timer that kills the process.

`ask_model()` still accepts plain text on stdout: most checks here stub the CLI
with a script that prints an answer, and making each one imitate a stream to
test the code AROUND the model would be work for nothing.

Codex failures are not confined to `item.completed` error items. Current Codex
emits a top-level `{"type":"error","message":...}` followed by a
`turn.failed` event whose nested `error.message` repeats the same diagnosis.
Capture all three forms, deduplicate identical messages, and only promote them
to stderr when the process failed or produced no answer. Otherwise a real quota
or authentication refusal becomes the useless `exited non-zero and said
nothing`, while naively appending both current events prints the same failure
twice. Pin both the current paired-event shape and the older item form in the
stream regression.
