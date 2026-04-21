# Testing

## Running Tests

```bash
pulp test                    # Run all tests
ctest --test-dir build       # Equivalent, direct CTest
./validate-build.sh          # Detached clean worktree configure/build/test
./validate-build.sh --smoke  # Fast clean install/export preflight, no tests
```

Filter tests by name:

```bash
pulp test -R Gain            # Tests matching "Gain"
pulp test -R "golden"        # Golden-file audio tests
ctest --test-dir build -R clap-dlopen   # CLAP load tests
```

Use `./validate-build.sh --quiet` when you want a detached clean-build outer loop
that ignores incremental build state and only prints logs on failure. It validates
committed `HEAD` in a detached worktree, then runs a fresh configure, build, install,
an installed SDK `find_package(Pulp)` smoke configure, and an optional test pass.
This is the quickest way to catch clean-build drift, basic install/export breakage,
and missing setup dependencies before opening a PR. Run `./tools/check-docs.sh`
separately for manifest and README/docs consistency checks.

Use `./validate-build.sh --smoke` when you want the fastest truthful preflight for
build-system and SDK export work. Smoke mode still bootstraps dependencies, does a
clean detached configure/build/install, and runs the installed-SDK `find_package(Pulp)`
smoke configure, but disables tests, examples, and GPU in that clean build so you can
catch install/export regressions before paying for a full validation run.

## Repeat Until Fail

When a test or command only fails intermittently, use the repeat helper instead of
copy-pasting shell loops:

```bash
tools/scripts/repeat-until-fail.sh 100 -- ctest --test-dir build -R "SeqLock concurrent stress test" --output-on-failure
tools/scripts/repeat-until-fail.sh -- ctest --test-dir build -R "OSC sender/receiver loopback" --output-on-failure
```

The helper prints the current iteration, exports it as
`REPEAT_UNTIL_FAIL_ITERATION`, and stops on the first failure. This is useful for
debugging flaky concurrency, timing, path, and validator issues without inventing a
new one-off loop every time.

## Test Surface

What the suite exercises:

- Unit tests for subsystems (runtime, state, audio, midi, signal, format, events, canvas, render, view, osc)
- Golden-file audio comparison tests
- Plugin format validation (CLAP dlopen, VST3 pluginval, AU auval, optional local AAX validation)
- Example plugin processing tests

For coverage numbers and per-subsystem / per-platform / per-surface breakdowns, see [Test Coverage](coverage.md).

## Test Layers

| Layer | What | When |
|-------|------|------|
| Unit tests | Individual functions and classes | Every build |
| Golden-file tests | DSP output matches reference audio files | Every build |
| Format validation | Plugins load and pass format validators | `pulp validate` |
| Example tests | Each example plugin has a processing test | Every build |

## Plugin Validation

```bash
pulp validate
```

This runs:

- **CLAP**: `clap-validator` if installed, otherwise falls back to dlopen checks
- **VST3**: `pluginval` if installed, otherwise skips cleanly
- **AU**: `auval` on macOS if the component is installed
- **AAX**: DigiShell + AAX Validator on macOS/Windows if installed; otherwise the CLI reports a guided skip

Validation is separate from unit tests. Unit tests verify internal behavior; validation verifies that the built plugin binary loads and behaves correctly from the host's perspective.
Public CI does not build or validate AAX because the SDK and validator are
developer-supplied and not bundled by Pulp.

## Design Tool Debugging

The design tool has its own headless harness:

```bash
pulp design-debug --prompt "make the gain knob look like macOS 7" --target k1
```

Use this when you want reproducible before/after/diff screenshots plus JSON metadata
for AI-driven restyling runs. The report captures provider/model/reasoning-effort,
request text, token changes, widget look ids, screenshot diff metrics, and the
render backend used for the capture.

The default `--capture-backend skia` path renders widget SkSL offscreen, which makes
it useful for shader-aware pipeline verification. The remaining limitation is that
it still does not prove final live GPU presentation parity in the interactive app.

## Writing Tests

Tests use Catch2. Test files live in `test/` or alongside examples.

Example test structure:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <pulp/format/headless.hpp>
#include "my_processor.hpp"

TEST_CASE("MyPlugin processes audio") {
    pulp::format::HeadlessHost host(create_my_processor);
    host.prepare(48000.0, 512);

    pulp::audio::Buffer<float> in(2, 512), out(2, 512);
    // Fill input, process, check output...
}
```

## Golden-File Tests

Golden-file tests render known input through a processor and compare the output against a reference file. If the output differs beyond a tolerance, the test fails.

This catches unintended DSP regressions.

## Test Expectations

- New public behavior needs tests
- Bug fixes should add or adjust a test when practical
- Example plugins each include a test file (`test_pulp_*.cpp`)
