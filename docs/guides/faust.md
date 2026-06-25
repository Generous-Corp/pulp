# FAUST External Toolchain Guide

Use this guide when you want to author DSP in FAUST and ship it through Pulp's
existing `Processor` model.

## Truthful Scope

Pulp supports a **narrow but real** FAUST lane today:

- offline code generation only
- external `faust` compiler when regeneration is needed
- checked-in generated C++ headers so builds work without a local FAUST install
- parameter reflection into `StateStore`
- headless validation through normal Pulp tests

This guide does **not** imply:

- live hot reload
- JIT compilation
- MIDI mapping from FAUST metadata
- automatic UI generation from FAUST
- a first-class `pulp dsp compile` CLI flow

The supported model is: FAUST is an external codegen tool, Pulp ships the
wrapper substrate and compiles the generated output.

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

Each example checks in:

- the `.dsp` source
- the generated C++ header
- a small adapter header that instantiates `dsl::FaustProcessor<T>`
- headless regression tests

## Recommended Workflow

### 1. Start from a checked-in generated header

Pulp's current build story assumes the generated FAUST C++ is committed into the
project so builds remain reproducible without requiring a local FAUST install.

That means your project should typically include both:

- `my_effect.dsp`
- `generated_my_effect.hpp`

### 2. Wrap the generated DSP class with `FaustProcessor<T>`

Example:

```cpp
#include <pulp/dsl/faust_processor.hpp>
#include "generated_my_effect.hpp"

namespace myapp {

using MyEffectProcessor = pulp::dsl::FaustProcessor<MyEffectDsp>;

inline std::unique_ptr<pulp::format::Processor> create_my_effect() {
    return std::make_unique<MyEffectProcessor>();
}

}  // namespace myapp
```

### 3. Regenerate only when the `.dsp` source changes

If `faust` is installed locally, regenerate with the checked-in CMake helpers.

Per-file codegen:

```cmake
include(tools/cmake/PulpFaust.cmake)

pulp_faust_generate(
    ${CMAKE_CURRENT_SOURCE_DIR}/generated_my_effect.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/my_effect.dsp
    MyEffectDsp
)
```

Repo-wide regeneration target:

```bash
cmake --build build --target faust-regenerate
```

If `faust` is not installed, Pulp falls back to the checked-in generated header
and the build still works.

### 4. Validate with headless tests

The real support lane is not just “it compiles.” A FAUST-backed processor should
prove:

- descriptor and parameter reflection
- fixed-buffer processing
- deterministic state round-trip
- expected output behavior

Reference commands:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target pulp-faust-gain-test pulp-faust-filter-test pulp-faust-tremolo-test -j8
ctest --test-dir build --output-on-failure -R Faust
```

## Example Layout

The current examples model the supported project shape:

```text
examples/faust-gain/
  gain.dsp
  generated_gain.hpp
  faust_gain.hpp
  test_faust_gain.cpp
  CMakeLists.txt
```

That is the pattern to copy into a new project today.

## CMake Helpers

See [CMake Reference](../reference/cmake.md#pulp_faust_generate) for the helper
function signatures.

Current helpers:

- `pulp_faust_generate(...)`
- `pulp_add_faust_test(...)`
- `faust-regenerate` target when a local `faust` compiler is present

## Validation Expectations

Treat a FAUST-backed processor as a normal Pulp processor once generated:

- use `StateStore` for parameters/state
- use the normal headless host/testing lane
- keep generated artifacts under version control
- expect builds to succeed without `faust` installed

## Known Boundaries

- FAUST remains **experimental** in the capabilities matrix because the support
  lane is still narrow
- the external-toolchain workflow is real and supported
- broader productization like templates, project scaffolding, or a dedicated DSP
  compile CLI remains follow-up work
