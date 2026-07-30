# Resuming this work

The Forge worktree lives at `/tmp/forge-cur` and **`/tmp` does not survive a
reboot.** Everything durable is here in `forge-seam/`. To rebuild the working
state:

```bash
cd /Volumes/Workshop/Code/forge
git fetch origin
git worktree add /tmp/forge-cur origin/main        # 7f0999a when this was written
cd /tmp/forge-cur
git apply /Volumes/Workshop/Code/pulp-modular-rack/forge-seam/patches/0001-*.patch
cp -r /Volumes/Workshop/Code/pulp-modular-rack/forge-seam/test/* test/
# register the no-leak test in CMakeLists.txt (see ../README.md)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/Code/pulp-sdk-forge-aumi"
cmake --build build --target forge-test-chrome-no-leak forge-test-chrome -j 8
./build/forge-test-chrome-no-leak     # expect 3/3
./build/forge-test-chrome             # expect 4711 assertions
```

**Do not work in `/Volumes/Workshop/Code/forge` directly.** Its working tree has
unresolved conflict markers in `src/chrome.cpp` (`UU`), and its local `main` is
431 commits behind `origin/main` — building from it produced two passes of wrong
visual work before that was noticed.

## Facts that cost time to find

- **`ForgeFxShell` is `final`.** No test double by subclassing it; that is why the
  live-path proofs here were manual. `ForgeModularShell` will be the first real
  `ForgeShell` subclass and makes them permanent.
- **`ForgeChrome` has no virtuals** and takes `ForgeShell&`. The extension point
  is the shell, never the chrome.
- **`ForgeShell`'s 14 virtuals are all DSP** — `process_audio`, `has_build`,
  `macro_descriptors`, `install_generated_bundle`, `current_sample_rate`. None
  are UI. That is why the seam had to be added rather than found.
- **`design_tokens.hpp` is the palette's source of truth**, not `chrome.cpp`.
  Lines are translucent (`#DCE8FA1F`), the hero is 42, `surface_sunken` exists.
- **Forge MIDI has no Standalone target** (CLAP + AU only), which is why the
  no-leak guard renders chrome directly instead of screenshotting apps.
- **`icon_kind` is not declared** where the composer row builder sits.
- **The product name is already config-driven**: `FORGE_IDENTITY_PRODUCT_NAME`,
  `FORGE_IDENTITY_MARK` and friends are `if(NOT DEFINED)` overrides in
  `cmake/ForgeIdentity.cmake`.

## Where the phases stand

| Phase | State |
|---|---|
| 0 — no-leak guard | **done**, fails on a 1px shared change, passes when reverted |
| 1 — the seam | **done**, 3 changes, 3 products byte-identical, each proven live |
| 2 — Forge Modular runs Forge's UI | **done**, 7.2/255 vs Forge Instrument, verified windowed |
| 3 — the tabs | **in progress** — see below |
| 4–8 | not started; see `../SPEC-forge-modular.md` |

## Phase 3, exactly where it stands

Working, and visible in `design/prototype/modular-tabs-wip.png`:

- Two `TextButton` tabs come through `home_accessory()` and render.
- The styles are right: the selected tab is the loud one (`ghost`, which paints
  the accent) and the unselected is the quiet box (`secondary`). That is the
  opposite of the first guess and only a render showed it.
- `ForgeChrome::refresh_copy()` re-reads `chrome_copy()` and updates the hero,
  the badge and the placeholder in place, so switching artifact does not rebuild
  the tree and drop a half-typed prompt. It leaves a non-empty prompt alone.
- Clicking the already-active tab is a no-op rather than a redundant rebuild.

**Not working: the row will not centre.** The two tabs sit at opposite edges of
the hero. Tried, in order: `align_self = center` on the accessory in chrome;
`dim_width = 100%` plus `justify_content = center` on the row; `flex_grow = 0`
and `flex_shrink = 0` on each button. None moved them.

**Do not guess at this a fourth time — and the measurement itself crashed.**

Attempting to measure it did this:

```cpp
auto view = shell.create_view();
view->set_bounds({0, 0, kDesignWidth, kDesignHeight});
view->layout_children();      // then walk the tree
```

→ **SIGSEGV.** Removed; it is not a valid test and it left the suite red.

That is the **second** crash touching this shell's view tree, and the first one
a human hit. Together they are a lead, not two coincidences:

| | Where |
|---|---|
| Reported crash | `rebuild_marketplace_cards` destroying a card's `Label`, wild pointer |
| This crash | walking the tree after `create_view()` + a manual `layout_children()` |

`render_to_file()` on the same tree is fine, and the seam tests are fine, so the
tree is not simply broken — something about laying it out or tearing it down
by hand is. **Chase this before the centring**, which is cosmetic by comparison.
Two candidates worth ruling out first: whether `layout_children()` may be called
directly on a chrome root at all (`render_to_file` may do a different pass), and
whether `home_accessory()`'s view, allocated in a different translation unit
from chrome's, is being destroyed under a mismatched view layout.

Also still owed for Phase 3: the tabs must drive which generator Build reaches,
with **both** sides asserted -- checking one side of a boolean is what let
"Build always made a patch" ship.

## THE IMPORTANT ONE: Forge Modular writes into Forge's storage

`FORGE_IDENTITY_STORAGE_DIRECTORY` defaults to `"Forge"`, and Forge Modular
inherited it. Running the Forge Modular standalone wrote **121 project
directories** into `~/Library/Application Support/Forge/projects`, which is the
same store Forge Instrument, MIDI and FX read.

Forge Instrument's home shelf now renders Forge Modular's projects. The no-leak
guard caught it as a changed baseline — 209,124 bytes against 201,127 — which is
the guard doing exactly its job, on the one kind of leak that matters most.

**This is a product bug, not a test bug.** A separate SKU must not put its
artifacts in another product's shelf.

**Fix:** set `FORGE_IDENTITY_STORAGE_DIRECTORY` for the Forge Modular target.
It is already an `if(NOT DEFINED)` override in `cmake/ForgeIdentity.cmake`, so
this is a one-line change in `modular/CMakeLists.txt` — the same mechanism that
makes the product name configurable.

**And the guard needs to be hermetic.** It currently renders against whatever is
in the shared store, so its baselines are only reproducible on a machine whose
store has not changed. Point it at a temp storage directory for the duration of
the run, or the next person will chase a "leak" that is really yesterday's
projects.

**Clean up before re-baselining:** those 121 directories are Forge Modular's, in
Forge's store. Decide whether to move or delete them; do not just refresh the
baseline over them, or Forge Instrument's baseline permanently encodes another
product's data.

## The two crashes have one root cause

`ForgeShell::create_view()` calls `ensure_default_build()`, with the comment
"so the editor always maps to a live graph". `ForgeModularShell` overrides it as
a no-op, so the chrome builds its views against a graph that does not exist.

Narrowed by elimination, one suspect per run:

| Test | Result |
|---|---|
| Walk Forge Modular's tree after `create_view()` | **SIGSEGV** |
| ...with `home_accessory()` returning nullptr | **SIGSEGV** — not the accessory |
| ...with the stock composer row | **SIGSEGV** — not the row |
| Walk **stock Forge FX**'s tree, same code | 2,578 views, 203 buttons, **fine** |

So it is the shell, and `ensure_default_build()` is the only remaining
difference that chrome depends on. It also explains the crash a human hit in
`rebuild_marketplace_cards`: the same missing graph, reached through a different
path.

**Fix:** either install a minimal valid build so the contract the base class
documents is honoured, or make the chrome tolerate a shell with no graph. The
first is smaller and matches what the other three products do.

## Open: a crash in rebuild_marketplace_cards

Reported from a windowed launch of the worktree build on 2026-07-29 23:43,
`SIGBUS` on a wild pointer (`0x700000408`, inside the GPU carveout):

```
View::~View  ←  Label::~Label  ←  View::~View (recursive)
ForgeChrome::rebuild_marketplace_cards + 152
ForgeChrome::refresh_marketplace_screen + 2352
ForgeChrome::build + 2200
ForgeShell::create_view + 1320
```

**Not reproducible on the current binary** — 25 s windowed, no crash. The report
is from an intermediate build taken mid-iteration, most likely the one where the
described-row branch still returned early from `build_home()` and left the home
half-constructed. `rebuild_marketplace_cards` starts by destroying every existing
card, which is exactly where a half-built tree would fail.

**Do not treat that as closed.** What it establishes:

- Forge Modular is the first shell to reach this path with a *described*
  composer row, and a half-built home tree kills it in the destructor rather
  than at the point of the mistake — which is a bad place to learn about it.
- The no-leak baselines, the seam tests and the compiler were all green while
  that build was broken. Only running it windowed showed anything.
- `--screenshot` sets `headless = true` and is **not** the same path as a
  windowed launch. Everything here had only ever been screenshotted.

**Owed:** a windowed launch as part of the routine check, not just a screenshot;
and a look at whether `rebuild_marketplace_cards` should tolerate a partial tree
rather than trusting it.

## Also open: Skia is dropping draws

Present on every run here, including the current good one:

```
[skia] WARNING - Couldn't convert SkImage to a Graphite-backed representation
[skia] WARNING - Key context creation failed in Device::drawGeometry, draw dropped!
```

Draws are being **discarded**, which is worth chasing before judging any render's
fidelity — a missing element may be a dropped draw rather than a layout bug. Seen
in Forge Instrument's run too, so it is not something Forge Modular introduced.
