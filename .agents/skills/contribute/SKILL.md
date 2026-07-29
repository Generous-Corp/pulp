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
changes how something *sounds* gets scrutinised, and shouldn't hold them up.

## 2. Never touch version files

Do **not** edit `CMakeLists.txt` `VERSION`, `.claude-plugin/plugin.json`,
`marketplace.json`, or `CHANGELOG.md`.

Pulp assigns versions **after merge** (`version-at-land`) from the diff. A
contributor-side bump only creates a conflict that makes the PR obsolete — it
does not help. Same for changelog entries: they are regenerated.

## 3. Build and test locally

```sh
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=ON
cmake --build build-tests -j"$(sysctl -n hw.ncpu)" --target <your-test-targets>
ctest --test-dir build-tests --output-on-failure -R "<pattern>"
```

Release, not Debug — a Debug build of a GPU/JS UI is dramatically slower and
will mislead you into thinking you caused a performance regression.

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
existing coverage for the same behaviour), leave them there and **say so** — the
maintainer decides whether the gate is meant literally. Do not scatter tests to
appease a path match.

Adding a file under `tools/scripts/` or a new skill also has an inventory to
regenerate — `python3 tools/scripts/pulp_tooling_disposition.py --write`, which
needs PyYAML and then an explicit disposition for the new entry. `contributor_check.sh`
does not catch this one; the `Vellum freeze` CI check does.

## 6. Structure — keep it landable

Before handing off:

- No file you touched should cross **~1000 LOC** without a reason you can state.
- A new `core/**` source file with no matching test is a red flag.
- Extract duplicated logic rather than copying it — if you fixed the same bug in
  two files, that is a sign the helper should be shared, and reviewers will ask.
- Prefer several small commits with real messages over one large one.

## 7. Hand it off

### If you have write access
Open the PR **first**, then push. `pull_request` workflows fire on
`synchronize`, so pushing before the PR exists leaves required checks `MISSING`
and the PR cannot merge. Use `gh pr create`. Do **not** run `shipyard pr` and do
not arm auto-merge — that is the maintainer's call.

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

That structure is what makes a contribution cheap to land. Sections 4 and 5
matter most.

## What you are NOT expected to do

Linux or Windows builds · DAW/host verification · sanitizers · multi-platform
validation · `shipyard pr` · merge-queue interaction · version bumps ·
changelog edits · running the full ~16k-test suite.

State plainly that you did not do them. That is the correct outcome, not a
shortfall.
