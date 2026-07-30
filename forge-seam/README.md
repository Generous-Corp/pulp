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
