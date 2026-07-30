---
name: contribute
description: Prepare an outside contribution to Pulp or Forge that a maintainer can land with minimal rework — routing (Core vs Forge), local build and test on a plain Mac, the checks that are worth running without Shipyard/Tart/VMs, and the handoff format. Use when contributing without write access or without the maintainer CI fleet.
---

# Contributing to Pulp / Forge without the maintainer setup

You do **not** need Shipyard, Tart, VMs, self-hosted runners, or write access.
Those are maintainer infrastructure. Your job is a clean, tested, well-scoped
change plus an honest account of what you could not verify. A maintainer takes
it from there.

Works the same under Claude Code and Codex — this file is the shared source of
truth (`.agents/skills/`), not a per-tool doc.

## The one rule that saves the most rework

**Say what you did not do.** A contribution that fixes one thing and clearly
lists three unverified things is far more useful than one that implies
everything was checked. The maintainer's expensive step is discovering an
unstated gap after merge. Write the gaps down and you are done; hide them and
you cost a debugging session.

---

## 1. Where does the change belong?

| Change | Repo |
|---|---|
| DSP, `core/**`, format adapters, runtime, view/state layers | **Pulp** (Core SDK) |
| Anything any Pulp plugin would want | **Pulp** |
| Generating plugins — Forge's UI, templates, catalog, workflow | **Forge** |
| Fixing a bug you hit *while building with Forge*, but the bug is in `core/**` | **Pulp** |

That last row is the common case and the easy one to get wrong. If you found it
from the plugin side but the defect lives in the shared layer, it belongs in
Pulp, and it should be split out from your product work.

**Split by concern, not by session.** If one branch contains two unrelated core
fixes plus a feature, prepare them as separate patches in dependency order and
say which are independent. Independent bug fixes land fast; anything that
changes how something *sounds* gets scrutinized, and shouldn't hold them up.

## 2. Never touch version files

Do **not** edit `CMakeLists.txt` `VERSION`, `.claude-plugin/plugin.json`,
`marketplace.json`, or `CHANGELOG.md`.

Pulp assigns versions **after merge** (`version-at-land`) from the diff. A
contributor-side bump only creates a conflict that makes the PR obsolete — it
does not help. Same for changelog entries: they are regenerated.

## 3. Build and test locally

```sh
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=ON \
  -DPULP_ENABLE_GPU=ON
cmake --build build-tests -j"$(( $(sysctl -n hw.ncpu) / 2 ))" --target <your-test-targets>
ctest --test-dir build-tests --output-on-failure -R "<pattern>"
```

Release, not Debug — a Debug build of a GPU/JS UI is dramatically slower and
will mislead you into thinking you caused a performance regression.

A share of the cores, not all of them: a full-core build starves everything else
on the machine, including whatever you are about to run the tests against. A
lint (`build_parallelism_guard.py`) enforces this across the repo, so a whole-machine
`-j$(sysctl -n hw.ncpu)` copied from anywhere will fail CI.

Add `-DPULP_ENABLE_GPU=ON` for anything touching view, canvas, render, or an
imported design — without it the GPU paths are not built and your tests may not
exist. A first configure also fetches external SDKs (VST3, Skia prebuilts), so
budget time for it and do not take the first run's duration as normal.

**Python 3.11+ is worth installing before you start**, not after a gate confuses
you — macOS ships 3.9, and several checks misreport on it:

```sh
brew install python@3.12 && python3.12 -m pip install --user diff-cover
# then run the check under it:
PYTHON=python3.12 tools/scripts/contributor_check.sh <targets>
```

**If your Mac exports `SDKROOT`** pointing at a CommandLineTools SDK, the build
can fail on missing `std::jthread`. Pass an explicit modern SDK:
`-DCMAKE_OSX_SYSROOT=macosx26.2`. This is a local environment workaround, not a
change to make in the repo.

## 4. Tests ship with the fix — and must fail without it

Non-negotiable in this repo. For every fix, add a test **and prove it fails
when the fix is reverted.** Revert the change, rebuild, watch it fail, restore.
Then say so in the handoff — "confirmed failing without the fix" is the single
most credible sentence you can write.

"It compiles" and "CI was green" are not tests.

**Register a new test in `test/cmake/<owner>_tests.cmake`, never in
`test/CMakeLists.txt`.** The top-level file is a frozen include hub and adding a
registration there trips the hotspot gate — the obvious place is the wrong one.

## 5. Run the contributor check

```sh
tools/scripts/contributor_check.sh                    # whole diff vs origin/main
tools/scripts/contributor_check.sh pulp-test-<name>   # also measure diff coverage
```

It runs only what is meaningful on a plain Mac: version-file hygiene, that tests
accompany source, a size/structure review, the sub-second repo gates, and diff
coverage. It never needs Shipyard, Tart, or a VM. Its own self-tests are
`tools/scripts/test_contributor_check.sh`.

**A check it cannot run is not a failure — it is a line in your handoff.** The
script prints those together at the end, ready to paste. Do not bypass a gate
silently.

Two environment facts worth knowing before you read its output:

- **Python 3.11+.** Parts of `gates.sh` need `tomllib` and `unittest`'s
  `enterContext`, neither of which exists in the 3.9 macOS ships. On stock
  Python you will see `deps-audit self-tests: failing` — that is the interpreter,
  not your change. The script says so rather than blaming your work, but install
  3.11+ if you want a real answer. Skill-sync and version-bump — the two gates
  that most often send a PR back — do run correctly on 3.9.
- **Coverage is opt-in.** Pass your test target(s) to measure it. With no target
  the whole-tree coverage build takes ~30 minutes, so the script skips it and
  tells you rather than appearing to hang. It also skips automatically when your
  diff contains no C/C++ — there is nothing to cover.

### Path-based gates can fire on files you barely touched

Some gates key off *paths*, not semantics. Touching `widget_bridge.hpp` demands
a `test/test_widget_bridge*.cpp` change and compat-doc updates even if you only
added a private declaration. If your tests genuinely belong elsewhere (beside
existing coverage for the same behavior), leave them there and **say so** — the
maintainer decides whether the gate is meant literally. Do not scatter tests to
appease a path match.

Adding a file under `tools/scripts/` or a new skill also has an inventory to
regenerate — `python3 tools/scripts/pulp_tooling_disposition.py --write`, which
needs PyYAML and then an explicit disposition for the new entry. `contributor_check.sh`
does not catch this one; the `Vellum freeze` CI check does.

### When a gate is genuinely not meant for your change

Some gates are satisfied by a **commit trailer** stating why, not by contorting
the change. This is a sanctioned, audited escape hatch — it lives in git history
where a reviewer sees it — not a bypass:

```
Skill-Update: skip skill=<name> reason="..."
Version-Bump: skip reason="..."
Config-Doc: skip reason="..."
```

Put one on the tip commit, with a real reason. If you believe a path-based gate
is firing on a file you barely touched, this is how you say so — and then the
maintainer can agree or disagree with a specific claim rather than guessing.

Do not reach for a trailer to silence a gate you simply have not addressed. "I
added the test somewhere the gate does not look, here is where" is a reason. "It
was failing" is not.

### Comments: no issue numbers, no phase or PR breadcrumbs

`docs_noise_lint` runs in the maintainer's pre-push and in CI, but **not** in
`gates.sh` — so nothing you run locally will catch this, and agents write exactly
what it forbids by default. In source comments and test tags, do not write
`(Phase 2)`, `slice 3 of`, `fixes #1234`, or `[issue-NNN]`-style Catch2 tags.
Write what the code does; the narrative belongs in the commit message.

```sh
git diff origin/main | grep -nE '^\+.*(//|#|\*).*(#[0-9]{3,}|[Pp]hase [0-9]|slice [0-9])'
```

### If you touch the check script itself

It runs on two very different shells: bash 3.2 on a contributor's macOS, and
bash 5 on Linux CI. They fail in opposite directions, so passing locally proves
little.

The concrete trap: `${#arr[@]:-0}` is a **bad substitution in bash 5** but is
accepted silently by 3.2. Arrays here are all explicitly initialized, so plain
`${#arr[@]}` is right on both. `mapfile` is the mirror image — bash 4+ only, so
it breaks on macOS instead.

`bash -n` catches neither; both are runtime. The self-tests grep for the known
bad form, because macOS cannot execute its way into the failure.

## 6. Structure — keep it landable

Before handing off:

- No file you touched should cross **~1000 LOC** without a reason you can state.
- A new `core/**` source file with no matching test is a red flag.
- Extract duplicated logic rather than copying it — if you fixed the same bug in
  two files, that is a sign the helper should be shared, and reviewers will ask.
- Prefer several small commits with real messages over one large one.

## 7. Hand it off

### If you have write access
Push the branch, then `gh pr create`. Do **not** run `shipyard pr`, and do not
arm auto-merge — that is the maintainer's call.

If the PR opens but required checks show `MISSING` rather than pending, the
workflows did not dispatch; say so rather than waiting it out. (A PR opened by an
app token does not auto-trigger `pull_request` workflows — the maintainer can
dispatch them.)

### If you have READ access only (the common case)
You cannot push a branch. Two good options:

```sh
# format-patch series — reviewable, and `git am`-able in order
git format-patch --stdout <base>..HEAD > 01-my-change.patch

# or a bundle carrying the real commits
git bundle create my-work.bundle <base>..HEAD
```

Record the **exact base commit** you built on, verify your patches apply
cleanly to it, and say so. A maintainer can then reproduce your branch exactly.

Alternatively fork the repo and open a PR from the fork — a normal PR, but note
that some workflows behave differently for forks.

### The handoff document

Ship a short `README.md`/`HANDOFF.md` alongside the patches with:

1. **Base commit** and how to apply
2. **Suggested split** — which patches are independent, which order
3. **What each change fixes**, and how the defect was found
4. **Verification** — test suites run, with results; and for each test, that it
   was confirmed failing without the fix
5. **What you could not do** — access, gates not run, platforms not built,
   hosts not verified, anything judged by ear rather than measured
6. **Recommendations** — anything you consider a starting point rather than a
   finished surface, and known rough edges in what you added
7. **Provenance** — one line: that the work is yours and you are contributing it
   under the repo's MIT license, and the origin of anything that is not yours
   (adapted from a reference, generated, copied from another project — with its
   license). Pulp is MIT and public; a maintainer cannot land code whose
   licensing is unstated. `git commit -s` (`Signed-off-by`) is a fine way to say
   the first half.

That structure is what makes a contribution cheap to land. Sections 4 and 5
matter most.

## What you are NOT expected to do

Linux or Windows builds · DAW/host verification · sanitizers · multi-platform
validation · `shipyard pr` · merge-queue interaction · version bumps ·
changelog edits · running the full ~16k-test suite.

State plainly that you did not do them. That is the correct outcome, not a
shortfall.
