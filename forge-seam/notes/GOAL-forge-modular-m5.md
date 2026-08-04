# GOAL — Forge Modular, finished

Hand this file back to start the work. It is the whole goal; the step detail
lives in `PLAN-forge-modular-e2e-delivery.md` and `PLAN-prototype-parity.md`.

**Do not stop until every part is done.** If something is blocked, finish
everything that is not, and say plainly which part is blocked and why.

---

## The goal, in one paragraph

On the M5, open Forge Modular — as a standalone app and as an AU, VST3 and CLAP
plugin in a DAW — type a prompt, and get a working VCV Rack **module** whose
panel you can see. Switch to Patch and get a **patch** that opens, makes sound,
**is recognisably the kind of patch that was asked for**, and **explains how it
is wired at three real depths**. Everything the prototype showed is there.

---

## Four parts

### 1. It works, everywhere — verified by you, not inferred

Steps 1–7 of the delivery plan. Steps 5 (re-validate, sign, notarize), 6 (set up
the M5 from scratch) and 7 (prove all three surfaces there) are outstanding.

**Before Step 5, fix the instruments that produce the evidence** — an
independent review found both broken, which means some existing green is not
green:

- `drive_app.py`'s patch PASS branch is dead code and `generating()` cannot see
  `patch.py`, so app-driven patch proofs report INCONCLUSIVE *on success*. Match
  the real success line; add a negative control (a failed run must not PASS).
- `install_toolchain.sh` has never passed on a fresh machine — which is exactly
  what Step 6 is. Seed a fresh home from the committed manifests, never
  overwriting generated state, and make a `mktemp -d` run a required rehearsal.

**The bar.** Do not say it is ready until you have personally verified, on the
M5, on the binaries that will actually be there: **the CLI**, **the standalone
app by clicking Build**, and **REAPER** with all three formats. "It shares the
seam" is not verification. Neither is a green suite.

### 2. The patches are worth distributing

Today a prompt produces a plausible module graph, not a good patch: a drone came
back a tone, a krell played one note, a bouncing ball had no trigger source —
and all three passed every gate, because the gates check that a patch *sounds*,
never that it is *the thing asked for*.

A module never had this problem, because `dsp_vocabulary.py` tells the model what
a module may be built from. Patches have no equivalent. Build one:

```
tools/rack/patch_idioms/*.json     ~60 idioms across voice, generative, rhythm,
                                   modulation, texture, utility
tools/rack/patch_vocabulary.py     renders them into the patch contract
tools/rack/idiom_check.py          asserts a patch against its claimed idiom
```

Each record carries what the idiom **is**, its required **topology**, expected
**behaviour**, what "sounds right" means, and the **mistakes** that make it fail.
One record does four jobs: teaches the model, checks the result, drives the
behavioural gate, and explains a rejection in words a person recognises.

Then verify behaviour, not just audibility: does it keep going, does it move, is
it rhythmic when asked, is it in range. Topology first (instant, specific),
audio second (costs seconds).

**Three things the review said this design gets wrong, and they are right:**

- **One writer for the claim set.** Resolve prompt → idiom deterministically,
  outside the model. Behaviour flags come from the idiom record. If the model
  also names an idiom, assert agreement and fail on disagreement — otherwise the
  model grades its own homework and every rejection teaches it to claim less.
- **Make `common_mistakes` executable.** Apply each mistake to a passing patch
  and assert the checker fails *naming it*. Sixty prose fields become sixty
  negative controls for almost nothing. Today ~57 of 60 topology specs are never
  proven able to fail at all.
- **Guard the vocabulary after substitution**, not just at install: the
  assembled contract must contain zero literal `<!--` markers and ≥50 idiom
  slugs. Checking the renderer instead of what the model receives re-arms the
  silent failure the DSP side already had.

**Prove it with a dozen prompts across families**, at least three of which
*imply* an idiom without naming it — otherwise only the naming half is ever
tested. Fix thresholds *before* running them, or the gate is fitted to the demo.

### 3. It looks like the prototype

`ForgeModular.dc.html` showed things that were designed and then not landed.

**Done today:** a built module now draws its own panel — knobs, silkscreen,
jacks — instead of an empty rectangle. The emitter had been writing that artwork
all along, beside a preview that never read it.

**Still missing:** the module spec table (WIDTH / CONTROLS / I/O / DSP / PANEL,
every field derived from the manifest, never retyped); role grouping in the
patch explanation (AUDIO / PITCH & GATE / CLOCK / MODULATION with counts and
dots); the stage cable legend and stage-side hover; the Rack presence and launch
model; one icon system for artifact, role and availability used everywhere; the
toolbar meta pill; and honest degradation states — a module whose panel is
missing must *say so*, not render an indistinguishable grey box.

Write down what is deliberately deferred, so "not landed" and "not wanted" stop
looking the same.

### 4. The explanations actually teach

The product's promise is that a patch explains itself. The review found it did
not, and the cause is now fixed but not finished.

**Done today:** the generator worked out why each cable existed, printed it to
stdout, and wrote only the netlist; the loader never looked for prose. Standard
depth adds exactly that string — so **Standard rendered byte-identical to Terse
for every real patch**, and the three-depth promise held only in tests that built
connections by hand. Reasons now travel as a sidecar and are read back: 4 of 8
cables on a really generated patch came back explained.

**Still to do:**

- **Derive cable role from structure, in one place.** The app currently infers
  role — grouping, dot colour, and the Learning primer, i.e. the taught content
  — from the cable's hex colour, against a convention never stated to the model.
  Learning depth can therefore teach a false concept for a correct cable.
- **Resolve real port names** from the inventory the CLI already uses, and
  disambiguate duplicate models. Today the app shows `out0 → in1`.
- **Give Learning a real delta.** Its whole addition is a static primer repeated
  per cable, which the existing `learning_lines > standard_lines` test rewards.
- **Explain patches nobody generated.** Ship sidecars with the example patches,
  and give imported human-designed patches a structure-derived overview plus the
  verified idiom's `is` line — the only explanation such a patch can ever have.
- **Lint the prose against the wiring.** A why key with no matching cable, or a
  module named in a clause that is not in the patch, should be caught. A correct
  patch with a wrong sentence teaches the wrong mechanism, invisibly.

---

## Where the patch content comes from

The repertoire these instruments actually have: Allen Strange's *Electronic
Music: Systems, Techniques and Controls*; Bjørn & Meyer's *Patch & Tweak*; the
manuals that document their own idioms best — Make Noise Maths, Mutable's design
notes, Buchla's LPG-centred west-coast approach; Rob Hordijk's Rungler; Todd
Barton's Krell.

**Techniques, not text.** A Krell patch *is* "a random voltage sampled to set an
envelope's decay, with end-of-cycle retriggering it." That is a method, and
describing it in our own words is fair. Copying a book's prose or ingesting a
community patch library is neither necessary nor permitted.

**Explicitly not in scope:** judging whether a patch is *good*. Taste is not
measurable and a gate claiming to measure it would be lying. What is measurable
is whether a patch is the KIND of thing it claims to be and behaves that way.

---

## What proof to bring back

The thing itself, not a description of it:

- the generator log's last lines (`gate passed`, `installed →`)
- Rack's own log naming the plugin, and a capture of the module or patch in Rack
- for a patch, the measured audio from the gate (mean and peak volts)
- for REAPER, the smoke's verdict line per format
- `auval` / `clap-validator` / VST3 probe results **from the shipped binaries**,
  re-run after signing
- for the idioms, the dozen prompts and what each check said
- for the UI, rendered captures beside the prototype screens

State anything you could not verify, and why. A skipped check reported as
skipped is fine. A skipped check reported as a pass is not.

---

## Standing constraints

- Work in `/Volumes/Workshop/Code/pulp-modular-rack`, branch
  `explore/modular-rack`. Nothing goes to `main` without being asked.
- The shell is built in a Forge worktree under `/tmp`, which macOS clears and
  another agent may rebuild. **Run `forge-seam/sync.sh` before finishing any
  session that touched it**; `--check` says whether anything is at risk now.
- Nothing Rack-shaped leaks into Forge Instrument / MIDI / FX. The no-leak guard
  is the check; if it fires, investigate before refreshing a baseline.
- The Rack SDK is GPLv3 and is not committed.
- Signing credentials live in `~/.config/pulp/secrets/`, never in the repo.
- Never install a plugin to a system folder without validating it first.
- Launching Rack, REAPER or the standalone takes an audio device: say so first,
  cap the run, quit gracefully. Never hard-kill Rack.
- Do not install anything on the M5 until it is ready to be tested.

---

## The two patterns behind nearly every bug here

**One list, two writers.** A manifest naming 29 modules against a header
declaring 24; two generators wording success differently; a gate reading
defaults while Rack loaded stored values; two `tools_dir()` implementations; an
emitter writing panels beside a preview that never read them. When something is
described in two places, derive it in one.

**The claim was never the thing asserted.** A green suite beside a screen saying
"Built" and "file is not there" at once. A test asserting which artifact was set
rather than which tab looked selected. Depth tests that built their connections
by hand, hiding that Standard and Terse were identical in the shipped app. When
a test passes, ask what would have to break for it to fail — then break it.
