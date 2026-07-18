---
name: faust
description: Create FAUST DSP plugins in Pulp using offline codegen, pre-generated C++ headers, and the FaustProcessor template wrapper.
requires:
  scripts:
    - tools/cmake/PulpFaust.cmake
  tools: []
---

# FAUST DSL

Use this skill when a task involves writing, generating, or testing FAUST-based processors in Pulp.

## Truthful Position

Pulp supports FAUST as an **offline codegen** DSL. The FAUST compiler is an optional external tool; pre-generated C++ is checked into the source tree so builds work without a FAUST installation.

Supported now:

- `.dsp` source files with offline codegen to C++ headers
- `FaustProcessor<T>` template that bridges any FAUST-generated `dsp` subclass into Pulp's `Processor` model
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
| `core/dsl/include/pulp/dsl/faust_base.hpp` | Self-contained FAUST base classes (`dsp`, `UI`, `Meta`) |
| `core/dsl/include/pulp/dsl/faust_processor.hpp` | `FaustProcessor<T>` template and `PulpFaustUI` |
| `core/dsl/include/pulp/dsl/dsl_processor.hpp` | `DslProcessor` base class shared across DSL lanes |
| `tools/cmake/PulpFaust.cmake` | CMake module: `pulp_faust_generate()`, `pulp_add_faust_test()` |
| `examples/faust-gain/` | Stereo gain — simplest FAUST example |
| `examples/faust-filter/` | Parametric filter example |
| `examples/faust-tremolo/` | Tremolo with rate/depth |

## How It Works

FAUST offline codegen produces a C++ class derived from `::dsp`. Pulp provides self-contained base classes in `faust_base.hpp` so the generated code compiles without the FAUST architecture headers. `FaustProcessor<T>` wraps the generated class:

1. **Constructor** calls `buildUserInterface()` to discover parameters and `metadata()` for plugin info
2. **`define_parameters()`** registers each FAUST zone as a `StateStore` parameter with correct range/unit/group
3. **`prepare()`** calls `dsp::init(sample_rate)`
4. **`process()`** syncs `StateStore` values into FAUST zone pointers, then calls `dsp::compute()`

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

### 2. Generate C++ (requires `faust` on PATH)

```bash
faust -lang cpp -cn MyPluginDsp -o examples/faust-myplugin/generated_myplugin.hpp \
    examples/faust-myplugin/myplugin.dsp
```

Or let CMake handle it (regenerates when .dsp changes):

```cmake
include(PulpFaust)
pulp_faust_generate(
    ${CMAKE_CURRENT_SOURCE_DIR}/generated_myplugin.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/myplugin.dsp
    MyPluginDsp
)
```

The generated header is committed to the repo so builds never require `faust`.

### 3. Write the processor wrapper

```cpp
// examples/faust-myplugin/faust_myplugin.hpp
#pragma once
#include "generated_myplugin.hpp"
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

- verify `descriptor()` metadata matches FAUST `declare` statements
- verify parameter count, names, ranges, and units
- verify audio processing at known parameter values (unity gain, boost, cut)
- verify state serialization round-trip
- verify DSL reflection (`dsl_name() == "faust"`, `dsl_params()`, `bus_layout()`)

## Build and Test

```bash
# Build everything (uses pre-generated C++ — no faust needed)
tools/ci/governed-build.sh cmake --build build

# Run all FAUST tests
ctest --test-dir build -R "faust" --output-on-failure

# Run one example
ctest --test-dir build -R "FaustGain" --output-on-failure

# Regenerate all FAUST C++ (requires faust on PATH)
cmake --build build --target faust-regenerate
```

## What To Check

When modifying the FAUST lane, verify:

- pre-generated C++ compiles without `faust` installed
- parameter names, ranges, and units round-trip correctly from `.dsp` metadata
- bus layout matches `getNumInputs()` / `getNumOutputs()`
- audio output is correct at known parameter values
- `DslProcessor` reflection (`dsl_name`, `dsl_params`, `bus_layout`) is accurate
- new examples are added to `examples/CMakeLists.txt`

## Boundaries

This skill covers the **offline codegen** FAUST lane only.

- The FAUST compiler is external, not vendored
- Generated C++ is committed so builds are self-contained
- No runtime JIT or FAUST interpreter embedding
- No FAUST GUI — Pulp provides its own UI layer
