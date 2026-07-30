# The Forge seam — staged here until it is proposed

Changes destined for the **Forge** repo, kept here while Forge Modular is
unproven. Nothing in this directory is Rack-shaped: it is the product-neutral
extension point described in `../PLAN-repo-strategy.md`, plus the guard that
makes touching shared code safe.

Staged rather than committed to Forge because the user's condition is that
nothing goes into Forge until Forge Modular earns it. If Forge Modular is
dropped, delete this directory and Forge never knew.

## What is here

### `test/test_chrome_no_leak.cpp` + `test/baselines/chrome-home/*.png`

**Phase 0, and it is done.** Renders each Forge product's Home frame and holds it
against a committed baseline. Three products, three baselines.

Proven to work, in both directions:

| | Result |
|---|---|
| `shell_rail_width + 1.0f` in shared chrome | **3 of 3 failed** |
| reverted | **3 of 3 passed** |

Two things about it worth keeping:

- **It renders the chrome directly rather than driving standalones.** Forge MIDI
  ships CLAP and AU only, so it has no window to screenshot. Rendering chrome
  covers every product whether or not it has one.
- **It compares digests, not byte vectors.** Comparing vectors is correct and
  unreadable — Catch2 printed both PNGs on failure, burying the real message
  under thousands of characters. Two hashes and two paths is what a person can
  act on.

A blank render cannot pass: the frame is asserted over 20 KB first, so a render
that produced nothing could not match an equally empty baseline.

Refresh a baseline deliberately, never casually:

```
FORGE_NO_LEAK_UPDATE=1 ./forge-test-chrome-no-leak
```

and commit the changed PNGs with the reason.

### `patches/0001-chrome-copy-from-the-shell.patch`

**Phase 1, step 1 of 3, and it is done.** The four `switch (kind)` functions in
`chrome.cpp` — badge, prompt placeholder, follow-up placeholder, default build
title — become one `ChromeCopy` the shell returns. The chrome asks instead of
deciding.

84 insertions, 40 deletions, across 5 files. Net effect on the three existing
products: **none**.

| Check | Result |
|---|---|
| `forge-test-chrome-no-leak` | 3 of 3 byte-identical |
| `forge-test-chrome` | 4,711 assertions, 126 cases, all pass |

Two things this cost that are worth knowing:

- **The helpers had to return `std::string`, not `const char*`.** The copy now
  lives in a value the shell returns, so handing back a pointer into that
  temporary would dangle. The compiler does not catch it; the switch statements
  returned string literals and were safe by accident.
- **`chrome_copy()` is pure virtual on purpose.** A new product that forgets to
  answer fails to compile rather than silently inheriting another product's
  words.

### Phase 1, step 2 — the composer action row

**Done, and it is the one genuinely shared change.** `ComposerRow` describes what
the row contains — left items, right items, each a label, an icon, a primary
flag and a callback — and `ForgeShell::composer_row()` returns it. An **empty**
row means "the standard one", so the three original products are untouched and
nothing that does not care has to change.

The chrome still owns the treatment: size, radius, border, icon colour, label
type. A product says what its buttons ARE; the chrome decides how they look, so a
described row cannot accidentally style itself out of the family.

Proven live and per-product in one run. Temporarily describing Forge FX's row:

| Product | Result |
|---|---|
| FX (described) | **failed** — the render changed, so the path is live |
| Instrument | passed — untouched |
| MIDI | passed — untouched |

Reverted: 3 of 3 pass, and Forge's chrome suite stays at 4,711 assertions.

That is the guarantee demonstrated rather than asserted: a product describing its
own row changes only itself.

**One honest gap.** That demonstration was manual, because `ForgeFxShell` is
`final` and cannot be subclassed for a test double. A permanent test needs a
`ForgeShell` subclass — which is exactly what `ForgeModularShell` will be in
Phase 2, so the test becomes natural there rather than requiring a throwaway.

`icon_kind` was dropped from the icon enum: it is not declared where the row
builder sits, and an icon that cannot be wired is worse than one that does not
exist.

Still to do in Phase 1: the two optional view hooks.

## Applying it to a Forge checkout

The test needs one registration in Forge's `CMakeLists.txt`, beside
`forge-test-chrome`:

```cmake
add_executable(forge-test-chrome-no-leak test/test_chrome_no_leak.cpp)
target_include_directories(forge-test-chrome-no-leak PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/test)
target_link_libraries(forge-test-chrome-no-leak PRIVATE
    forge_core Catch2::Catch2WithMain)
catch_discover_tests(forge-test-chrome-no-leak)
```

Built and run against Forge `origin/main` at `7f0999a`.
