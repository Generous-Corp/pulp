# Testing

## Running Tests

```bash
pulp test                    # Run all tests
ctest --test-dir build       # Equivalent, direct CTest
```

Filter tests by name:

```bash
pulp test -R Gain            # Tests matching "Gain"
pulp test -R "golden"        # Golden-file audio tests
ctest --test-dir build -R clap-dlopen   # CLAP load tests
```

## Test Count

The current branch has 646 registered tests covering:

- Unit tests for subsystems (runtime, state, audio, midi, signal, format, events, canvas, render, view, osc)
- Golden-file audio comparison tests
- Plugin format validation (CLAP dlopen, AU auval)
- Example plugin processing tests

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
- **AU**: `auval` on macOS if the component is installed

Validation is separate from unit tests. Unit tests verify internal behavior; validation verifies that the built plugin binary loads and behaves correctly from the host's perspective.

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
