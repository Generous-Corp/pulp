---
name: faust
description: Create Faust DSP plugins in Pulp using Pulp-owned reference DSPs, optional external code generation, and the FaustProcessor template wrapper.
requires:
  scripts:
    - tools/cmake/PulpFaust.cmake
  tools: []
---

# FAUST DSL

Use this skill when a task involves writing, generating, or testing FAUST-based processors in Pulp.

## Truthful Position

Pulp supports a narrow Faust lane built around `FaustProcessor<T>`, Pulp-owned
reference DSP implementations, and optional external code generation. The
Faust compiler is supplied and licensed by the developer; Pulp does not vendor,
download, redistribute, or require it for normal builds.

Supported now:

- optional `.dsp` authoring examples and external code generation to developer-selected artifacts
- `FaustProcessor<T>` template that bridges any compatible `dsp` subclass into Pulp's `Processor` model
- Pulp-owned gain, filter, and tremolo reference DSP implementations; no
  source equivalence to their adjacent `.dsp` examples is claimed
- automatic parameter reflection from FAUST `buildUserInterface()` into `StateStore`
- bus layout detection from `getNumInputs()` / `getNumOutputs()`
- metadata extraction (`name`, `author`, `version`) into `PluginDescriptor`
- CMake integration via `PulpFaust.cmake` (`pulp_faust_generate`, `pulp_add_faust_test`)
- three working examples: gain, filter, tremolo

Not supported by this skill:

- FAUST MIDI mapping
- FAUST polyphony / `declare options "[midi:on][nvoices:N]"`
- runtime JIT compilation
- `@gfx` / FAUST GUI — FaustProcessor reports `has_editor() = false`
- soundfile support

## Core Files

| File | Purpose |
|------|---------|
| `core/dsl/include/pulp/dsl/faust_base.hpp` | Pulp-owned Faust compatibility interfaces (`dsp`, `UI`, `Meta`) |
| `core/dsl/include/pulp/dsl/faust_processor.hpp` | `FaustProcessor<T>` template and `PulpFaustUI` |
| `core/dsl/include/pulp/dsl/dsl_processor.hpp` | `DslProcessor` base class shared across DSL lanes |
| `tools/cmake/PulpFaust.cmake` | CMake module: `pulp_faust_generate()`, `pulp_add_faust_test()` |
| `examples/faust-gain/` | Stereo gain — simplest FAUST example |
| `examples/faust-filter/` | Parametric filter example |
| `examples/faust-tremolo/` | Tremolo with rate/depth |

## How It Works

`FaustProcessor<T>` accepts a default-constructible class derived from `::dsp`.
Pulp provides the narrow compatibility interfaces in `faust_base.hpp`, which
are used by its reference DSPs and can also adapt a compatible artifact produced
by an externally supplied Faust compiler.

1. **Constructor** calls `buildUserInterface()` to discover parameters and `metadata()` for plugin info
2. **`define_parameters()`** registers each FAUST zone as a `StateStore` parameter with correct range/unit/group
3. **`prepare()`** calls `dsp::init(sample_rate)`
4. **`process()`** syncs `StateStore` values into FAUST zone pointers, adapts
   the block's channels to the compatibility interface, and calls
   `dsp::compute()`

## Creating a FAUST Plugin

### 1. Write the .dsp source

```faust
// examples/faust-myplugin/myplugin.dsp
declare name "MyPlugin";
declare author "YourName";
declare version "1.0.0";

import("stdfaust.lib");

gain = hslider("Gain [unit:dB]", 0, -60, 24, 0.1) : ba.db2linear;
process = _, _ : *(gain), *(gain);
```

### 2. Optionally generate a developer-owned C++ artifact

This requires an externally supplied `faust` executable. Record or pin the
compiler identity and verify its output before claiming that an artifact
corresponds to a source file.

```bash
faust -lang cpp -cn MyPluginDsp -o build/myplugin_faust.hpp \
    examples/faust-myplugin/myplugin.dsp
```

Or let CMake handle it (regenerates when .dsp changes):

```cmake
include(PulpFaust)
pulp_faust_generate(
    ${CMAKE_CURRENT_BINARY_DIR}/myplugin_faust.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/myplugin.dsp
    MyPluginDsp
)
```

The helper is inactive when `faust` is unavailable. Pulp's checked-in reference
DSPs remain buildable independently of the optional compiler.

### 3. Write the processor wrapper

```cpp
// examples/faust-myplugin/faust_myplugin.hpp
#pragma once
#include "myplugin_faust.hpp" // developer-owned, verified external artifact
#include <pulp/dsl/faust_processor.hpp>

namespace pulp::examples {
using MyPluginProcessor = dsl::FaustProcessor<MyPluginDsp>;

inline std::unique_ptr<format::Processor> create_my_plugin() {
    return std::make_unique<MyPluginProcessor>();
}
} // namespace pulp::examples
```

### 4. Add CMakeLists.txt

```cmake
if(PULP_BUILD_TESTS)
    add_executable(pulp-faust-myplugin-test test_faust_myplugin.cpp)
    target_link_libraries(pulp-faust-myplugin-test PRIVATE pulp::dsl pulp::format Catch2::Catch2WithMain)
    target_include_directories(pulp-faust-myplugin-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(pulp-faust-myplugin-test)
endif()
```

### 5. Write tests

Follow the pattern in `examples/faust-gain/test_faust_gain.cpp`:

- verify `descriptor()` metadata matches the values exposed by the selected
  `dsp` class
- verify parameter count, names, ranges, and units
- verify audio processing at known parameter values (unity gain, boost, cut)
- verify state serialization round-trip
- verify DSL reflection (`dsl_name() == "faust"`, `dsl_params()`, `bus_layout()`)

## Build and Test

```bash
# Build the Pulp reference DSP examples (no faust needed)
tools/ci/governed-build.sh cmake --build build

# Run all FAUST tests
ctest --test-dir build -R "faust" --output-on-failure

# Run one example
ctest --test-dir build -R "FaustGain" --output-on-failure

# Generate every registered optional artifact (target exists when faust is found)
cmake --build build --target faust-regenerate
```

## What To Check

When modifying the FAUST lane, verify:

- Pulp reference DSPs compile and their numerical tests pass without `faust`
  installed
- optional compiler output is treated as a distinct developer-owned artifact
  unless a pinned compiler and verification receipt prove a stronger claim
- parameter names, ranges, and units round-trip correctly from the selected
  `dsp` class's `buildUserInterface()` metadata
- bus layout matches `getNumInputs()` / `getNumOutputs()`
- audio output is correct at known parameter values
- `DslProcessor` reflection (`dsl_name`, `dsl_params`, `bus_layout`) is accurate
- new examples are added to `examples/CMakeLists.txt`

## Boundaries

This skill covers the Pulp reference DSP and optional external Faust codegen
lane only.

- The FAUST compiler is external, not vendored
- Pulp's reference DSP implementations are checked in and build without Faust
- External compiler artifacts are developer-owned inputs and are not Pulp
  reference implementations by default
- No runtime JIT or FAUST interpreter embedding
- No FAUST GUI — Pulp provides its own UI layer
