# How to reuse Forge's chrome without committing to it

Status: recommendation. Answers a specific hesitation — until Forge Modular is
proven, nothing Rack-shaped should live in Forge, and backing out must be clean.

## The constraint that decides it

`ForgeShell` has 14 virtuals and **every one is DSP**: `process_audio`,
`has_build`, `macro_descriptors`, `install_generated_bundle`,
`current_sample_rate`, `generation_recipe_bank`. Not one is UI.

`ForgeChrome` has **no virtuals at all** and takes `ForgeShell&` by reference.

So an external subclass of `ForgeShell` gets Forge's chrome for free — and cannot
change a single word, add a tab, or mount an overlay. UI variation runs through
`ShellKind` *inside* `chrome.cpp`.

That kills the hope of pure outside-in extension, and it means the four obvious
strategies are not equivalent.

## The four options, honestly

### A — Fork Forge, track upstream

Forge Modular's chrome edits live on a long-lived branch of a fork.

- Back out: delete the fork. Clean.
- **Cost: a permanent rebase burden against a 9,154-line file we are editing.**
  Local `main` here was 431 commits behind origin in a matter of weeks, so
  `chrome.cpp` churns hard. Every upstream chrome change is a merge into our
  edits, in the largest file in the repo.
- Failure mode: the fork silently falls behind, we A/B against a stale Forge and
  "match" something that no longer exists. **This already happened once in this
  project** — two passes of visual work were built on a 431-commit-stale
  `chrome.cpp`, and the user caught it, not me.

### B — Long-lived unmerged branch in Forge's repo

Same as A without a second remote.

- Back out: delete the branch. Clean.
- Same rebase burden, same staleness failure mode.
- Slightly worse: a Rack-shaped branch sits in Forge's repo, so "nothing
  committed to Forge" is only true of `main`.

### C — Forge as a pinned submodule here, plus a patch series

Forge stays pristine and read-only; our chrome deltas are patches applied at
build time.

- Back out: delete the submodule and the patch directory. Clean.
- Cost: patches conflict on upstream churn — the same burden as A and B, but the
  deltas stay **explicit and small**, so a conflict is legible instead of buried
  in a 9k-line merge.
- Failure mode: patch rot. Better than A/B because the diff is the artifact and
  its size is visible.

### D — Upstream only the *seam*, keep the product out ✅ recommended

Land a small, **product-neutral** change in Forge that makes chrome extensible,
and nothing about VCV Rack or modular. Then Forge Modular lives entirely here as
an external subclass.

What goes into Forge — roughly 60 lines, all of it a refactor Forge benefits
from on its own:

1. **Wording moves off hard-coded switches.** `default_build_title`,
   `prompt_placeholder`, `followup_placeholder` and `shell_kind_badge` currently
   switch on `ShellKind` inside `chrome.cpp`. Replace with one virtual on
   `ForgeShell` returning a small `ChromeCopy` struct; the three existing shells
   return exactly what the switches return today. Forge gets its product strings
   out of its layout code, which is a plain improvement.
2. **The composer's action row becomes a description.** A `ComposerRow` struct —
   left items, right items, each a label, icon and callback. Forge's three kinds
   supply what they hard-code now (`+`/`Random`, `Select model`/`Create`).
3. **Two optional view slots.** A virtual returning an extra view for above the
   composer, and one for the workspace, both defaulting to `nullptr`. Forge's
   shells return `nullptr` and are unaffected.

What stays here: `ForgeModularShell : ForgeShell` overriding those hooks, the
tabs, the mention overlay, the rack preview, the wiring lines, the generator, the
`.vcvplugin`. **Nothing in Forge knows Rack exists.**

- Back out: delete this repo's directory. Forge keeps a generic extension point
  it wanted anyway — or reverts one small commit if not.
- **No rebase burden**, because we are not editing `chrome.cpp` on an ongoing
  basis. We consume a released SDK and a stable seam.
- Failure mode: the seam turns out too narrow for something later — the rack
  preview needs a hook chrome does not offer. Then it is one more small,
  product-neutral virtual, not a merge.

## My confidence, and where it is not high

**High on D.** The seam is small and its shape is already implied by the code —
chrome *already* varies wording per kind; D just moves the variation from a
switch to a virtual, which is the ordinary refactor for exactly this problem. It
is reversible because the only thing in Forge is a mechanism, not a product.

**Specifically not confident about, and worth saying:**

- **The composer-row description is the one genuinely shared change.** Getting it
  wrong changes Forge Instrument. This is why the no-leak render test is item 1
  and not item 5: three baseline renders, one per `ShellKind`, asserted
  byte-identical. Without that test I would not touch this file, and I would say
  so rather than be careful.
- **The mention overlay may want more than a view slot** — it has to sit over the
  composer and follow the caret, and I have not proven a plain child view can. If
  it needs chrome to know about an overlay layer, that is a second seam change
  and I would come back before making it.
- **I cannot promise the number of Forge-side changes is exactly three.** It is
  three for what is planned. A fourth is possible; a tenth would mean D was the
  wrong call and we should switch to C.

## What I would do first, before writing any of it

Prove the no-leak test can exist and fail correctly:

1. Build all three Forge shells' standalones from `origin/main`.
2. Screenshot each headlessly; commit the three as baselines.
3. Make one deliberate one-pixel change to shared chrome and confirm all three
   go red.
4. Revert; confirm green.

If step 3 does not go red, D is unsafe and C is the answer — because then
nothing protects the other products, and the whole argument for touching shared
code collapses.

That is a day's work and it decides the strategy on evidence rather than on my
reading. I would rather spend it than ask you to trust the reading, given this
project's record: about twenty gates were wrong the first time they met real
material, and every one was found by running it.

## Recommendation

**D, gated on the no-leak test passing step 3.** If it does not, **C** — pinned
submodule plus an explicit patch series — because visible, small patches are the
next best thing to no patches, and they fail loudly rather than silently.

**Not A or B.** Both put us back on a hand-maintained fork of the fastest-moving
file in Forge, and this project has already shipped visual work built on a stale
copy of exactly that file.
