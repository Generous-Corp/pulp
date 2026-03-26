# Testing Deep-Dive

Advanced testing patterns: HeadlessHost API, golden-file testing, sanitizer builds, and format validation.

## HeadlessHost

`HeadlessHost` wraps a Processor so you can drive audio processing entirely from code. No audio device, no UI, no DAW required.

```cpp
#include <pulp/format/headless.hpp>

HeadlessHost host(MyPlugin::create);
host.prepare(48000, 512);

// Set parameters
host.state().set_value(kGainID, -6.0f);

// Process audio
audio::Buffer<float> in(2, 512), out(2, 512);
// ... fill input buffer ...
auto in_view = in.view();
auto out_view = out.view();
host.process(out_view, in_view);

// Check output
REQUIRE(out.channel(0)[0] != 0.0f);
```

### HeadlessHost with MIDI

```cpp
midi::MidiBuffer midi_in, midi_out;
midi_in.add(midi::MidiEvent::note_on(0, 60, 100), 0);  // Middle C at sample 0

host.process(out_view, in_view, midi_in, midi_out);
```

### State Save/Load Testing

```cpp
// Save state
auto state_data = host.save_state();

// Modify parameters
host.state().set_value(kGainID, 0.0f);

// Restore state
REQUIRE(host.load_state(state_data));
REQUIRE(host.state().get_value(kGainID) == Approx(-6.0f));
```

## Golden-File Audio Tests

Compare DSP output against a reference file to catch regressions.

### Pattern

```cpp
TEST_CASE("MyPlugin gain golden file") {
    HeadlessHost host(MyPlugin::create);
    host.prepare(48000, 512);
    host.state().set_value(kGainID, -6.0f);

    // Process known input
    audio::Buffer<float> in(2, 512), out(2, 512);
    fill_with_sine(in, 440.0f, 48000);
    auto iv = in.view(); auto ov = out.view();
    host.process(ov, iv);

    // Compare against reference
    auto ref = audio::read_wav("test/golden/myplugin-gain-6db.wav");
    for (size_t i = 0; i < 512; ++i) {
        REQUIRE(out.channel(0)[i] == Approx(ref.channel(0)[i]).margin(1e-6f));
    }
}
```

### Creating Golden Files

```cpp
// Run once to create the reference:
audio::write_wav("test/golden/myplugin-gain-6db.wav", out, 48000);
```

Store golden files in `test/golden/` and commit them. They're small (a few KB each).

### Tolerance

Use `Approx().margin()` for float comparison. Typical tolerances:
- Bit-exact (integer math): `margin(0)`
- Float DSP: `margin(1e-6f)` (single precision rounding)
- Lossy/stochastic: `margin(1e-3f)` (reverb tails, noise generators)

## Sanitizer Builds

Pulp supports ASan, TSan, and UBSan via CMake:

```bash
# AddressSanitizer (memory errors, buffer overflows)
cmake -B build -DPULP_SANITIZER=address
cmake --build build
ctest --test-dir build --output-on-failure

# ThreadSanitizer (data races)
cmake -B build -DPULP_SANITIZER=thread
cmake --build build
ctest --test-dir build --output-on-failure

# UndefinedBehaviorSanitizer
cmake -B build -DPULP_SANITIZER=undefined
cmake --build build
ctest --test-dir build --output-on-failure
```

### CI Integration

The `sanitizers.yml` workflow runs ASan, TSan, and UBSan on every PR to main and develop:

```yaml
# .github/workflows/sanitizers.yml
- name: Build with ASan
  run: |
    cmake -B build -DPULP_SANITIZER=address
    cmake --build build
- name: Test with ASan
  run: ctest --test-dir build --output-on-failure
```

### Common Issues

- **ASan false positives with SDL3**: SDL3 may trigger ASan warnings in its own code. Suppress with `ASAN_OPTIONS=suppressions=test/asan_suppressions.txt`.
- **TSan and atomics**: relaxed atomics on independent values are intentionally racy from TSan's perspective. Use annotations if needed.

## Format Validation

### CLAP

```bash
# Full validation (requires clap-validator installed)
clap-validator validate build/CLAP/MyPlugin.clap

# Fallback: dlopen test
ctest --test-dir build -R clap-dlopen-MyPlugin
```

### Audio Unit (auval)

```bash
# Requires plugin installation first
cp -r build/AU/MyPlugin.component ~/Library/Audio/Plug-Ins/Components/
auval -v aufx MyPl Mnfr
```

`auval` checks:
- Component can be instantiated
- Parameters are accessible
- Audio passes through without NaN/Inf
- State save/load round-trips correctly

### VST3

```bash
# Via ctest labels
ctest --test-dir build -L vst3 --output-on-failure
```

## Running Tests

```bash
# All tests
pulp test
# or
ctest --test-dir build --output-on-failure

# Filter by name
pulp test -R "Knob"
ctest --test-dir build -R "Compressor"

# Filter by label
ctest --test-dir build -L signal
ctest --test-dir build -L format

# Parallel
ctest --test-dir build -j$(sysctl -n hw.ncpu)
```

## Writing Tests

Use Catch2. Each subsystem has its test file in `test/`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/signal/biquad.hpp>

using Catch::Approx;

TEST_CASE("Biquad lowpass at 1kHz") {
    pulp::signal::Biquad filter;
    filter.set_type(pulp::signal::Biquad::Type::lowpass);
    filter.set_coefficients(1000.0f, 0.707f, 48000.0f);

    // Process a DC signal — should pass through
    float out = filter.process(1.0f);
    // ... after settling, output should be ~1.0
}
```
