# Faust External Toolchain Guide

Use this guide when you want to author DSP in Faust and ship it through Pulp's
existing `Processor` model.

## Truthful Scope

Pulp supports a narrow but real Faust lane today:

- `FaustProcessor<T>` adapts a compatible `dsp` class to Pulp's `Processor`
- Pulp-owned reference DSP implementations exercise gain, filter, and tremolo
- an external `faust` compiler can generate developer-owned C++ artifacts
- parameter reflection into `StateStore`
- headless numerical validation through normal Pulp tests

Faust is optional, externally supplied, and absent from Pulp's normal build
dependency chain. Pulp does not vendor, download, or redistribute the compiler.
The checked-in reference implementations are not claimed to be compiler output
or source-equivalent to the adjacent `.dsp` examples.

This guide does not imply live hot reload, JIT compilation, MIDI mapping from
Faust metadata, automatic UI generation, a first-class `pulp dsp compile` flow,
or registration in the authored SignalGraph node model.

## What Pulp Ships

Core substrate:

- `core/dsl/include/pulp/dsl/faust_base.hpp`
- `core/dsl/include/pulp/dsl/faust_processor.hpp`
- `core/dsl/include/pulp/dsl/dsl_processor.hpp`
- `tools/cmake/PulpFaust.cmake`

Reference examples:

- `examples/faust-gain/`
- `examples/faust-filter/`
- `examples/faust-tremolo/`

Each example checks in an optional `.dsp` authoring example, a Pulp-owned
`reference_*.hpp` implementation, a compatibility `generated_*.hpp` forwarding
include, a `FaustProcessor<T>` adapter, and headless regression tests.

## Recommended Workflow

### 1. Choose the artifact you are validating

For the checked-in examples, include the Pulp-owned reference implementation:

```cpp
#include <pulp/dsl/faust_processor.hpp>
#include "reference_my_effect.hpp"

using MyEffectProcessor = pulp::dsl::FaustProcessor<MyEffectDsp>;
```

For your own Faust-authored DSP, generate a distinct artifact with your own
external compiler. Record the compiler version or immutable package identity
and validate the output before claiming that it corresponds to a source file.

### 2. Generate an optional external artifact

```cmake
include(${PULP_ROOT}/tools/cmake/PulpFaust.cmake)

pulp_faust_generate(
    ${CMAKE_CURRENT_BINARY_DIR}/my_effect_faust.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/my_effect.dsp
    MyEffectDsp
)
```

The helper is inactive when `faust` is unavailable. It never replaces Pulp's
reference implementations and does not make the compiler a required build
dependency. The `faust-regenerate` convenience target exists only when the
external compiler is found.

### 3. Validate through the normal processor lane

A Faust-backed processor should prove descriptor and parameter reflection,
fixed-buffer processing, deterministic state round trips, and its expected
numerical behavior.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target pulp-faust-gain-test pulp-faust-filter-test pulp-faust-tremolo-test -j8
ctest --test-dir build --output-on-failure -R Faust
```

## Example Layout

```text
examples/faust-gain/
  gain.dsp
  reference_gain.hpp
  generated_gain.hpp       # compatibility forwarding include
  faust_gain.hpp
  test_faust_gain.cpp
  CMakeLists.txt
```

## CMake Helpers

See [CMake Reference](../reference/cmake.md#pulp_faust_generate).

- `pulp_faust_generate(...)` invokes a discovered external compiler for an
  explicitly selected output
- `pulp_add_faust_test(...)` registers a normal Pulp processor test
- `faust-regenerate` is present only when a local `faust` executable is found

## Known Boundaries

Faust remains experimental because the support lane is narrow. The external
code-generation helper and Pulp reference processors are available today;
authored-graph registration, templates, scaffolding, a real-compiler
compatibility gate, and a dedicated compiler CLI remain follow-up work.
