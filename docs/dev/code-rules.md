# Code Rules

One-page card for writing C++ in Pulp. It states the rules; the linked pages give the reasons. Process (branches, pull requests, landing) lives in `CLAUDE.md` and `AGENTS.md`, not here.

## Design rules

- A plugin is a `pulp::format::Processor`. Format adapters (VST3, AU, CLAP, standalone) wrap it, so plugin code never names a format SDK.
- Subsystems live in `core/<subsystem>/`. Dependencies point downward only: `format` may use `state`, `audio` and `midi`, never `view` or `render`. Peer modules do not include each other unless the dependency is declared in CMake.
- Consumers include only `core/<subsystem>/include/pulp/<subsystem>/`. Docs and examples never include from `src/`.
- Public headers use `#pragma once` and include what they use. Keep internals out of them unless the type is deliberately public API.
- Parameters live in `state::StateStore` with stable numeric IDs. Never renumber a shipped ID.
- Verify behavior in source and prove it with a test. A comment is not evidence.

## Style

- Namespaces `pulp::<subsystem>`. Classes `PascalCase`. Functions and methods `snake_case`. Constants and enum values `kPascalCase`. Files `snake_case.hpp` and `snake_case.cpp`.
- C++20. Use designated initializers in declaration order, because GCC rejects reordered ones.
- Comments say why: a constraint, a non-obvious decision, a safety argument. Do not restate the code. Document the memory order on every atomic. Public API comments describe the contract, not the implementation.

## File layout

| Kind | Location |
|---|---|
| Public header | `core/<subsystem>/include/pulp/<subsystem>/<name>.hpp` |
| Implementation | `core/<subsystem>/src/<name>.cpp` |
| Platform code | `core/<subsystem>/platform/<os>/<name>.cpp` or `.mm` |
| Umbrella header | `core/<subsystem>/include/pulp/<subsystem>/<subsystem>.hpp` |
| Tests | `test/test_<name>.cpp`, or `test_<example>.cpp` beside an example |
| Examples | `examples/<name>/`, one directory per example |

Configure and build must not modify tracked files. Generated files go to build output.

## Class template

```cpp
#pragma once
#include <pulp/format/processor.hpp>

namespace pulp::examples {

enum MyParams : state::ParamID { kLevel = 1 };

class MyProcessor : public format::Processor {
  public:
    format::PluginDescriptor descriptor() const override;          // metadata and buses
    void define_parameters(state::StateStore& store) override;     // register parameters
    void prepare(const format::PrepareContext& ctx) override;      // allocate here
    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override;          // real-time, no allocation
};

}  // namespace pulp::examples
```

`examples/pulp-gain/pulp_gain.hpp` is the full working version.

## Test template

```cpp
#include <catch2/catch_test_macros.hpp>
#include <pulp/signal/adsr.hpp>

TEST_CASE("Adsr reaches sustain after attack and decay", "[signal][adsr]") {
    pulp::signal::Adsr env;
    env.set_sample_rate(44100.0f);
    env.note_on();
    for (int i = 0; i < 44100; ++i) (void) env.next();
    REQUIRE(env.stage() == pulp::signal::Adsr::Stage::sustain);
}
```

- Register the file in CMake with `pulp_add_test_suite(pulp-test-<name> LIBRARIES pulp::<subsystem>)`. The default source is `test_<name>.cpp`. A test file no CMake target names fails the dead-test guard.
- Every fix and every new public behavior lands with its test in the same change. Assert on the real path, not on "it compiles".
- A negative check needs a control that proves the check can pass. See "Confirm the failure" in `CLAUDE.md`.

## Real-time safety

Code reachable from `Processor::process()` must not:

- allocate (`new`, `malloc`, growing a `std::vector`, building a `std::string`),
- lock (`std::mutex`, condition variables, OS locks),
- throw or catch exceptions,
- do I/O, including logging,
- do work with unbounded run time.

Cross-thread data uses the primitives in `core/runtime/`:

| Need | Primitive |
|---|---|
| One independent value, latest wins | `std::atomic<T>` with relaxed order |
| Several fields read coherently | `SeqLock<T>` |
| Large state swap or meter data | `TripleBuffer<T>` |
| Ordered events | `SpscQueue<T>` |

Allocate in `prepare()`. Read parameters once per block unless the plugin needs per-sample automation.

## Platform guards

- Platform code goes under `core/platform/` or a `platform/<os>/` directory, selected in CMake.
- Where a source file must branch, use `#if defined(__APPLE__)`, `_WIN32`, `__ANDROID__` or `__linux__`, and test `TARGET_OS_IPHONE` after `__APPLE__`. `pulp::platform::current_os` gives the same answer at compile time.
- Keep the portable path complete. A feature that exists on one platform still gets a test that runs on that platform in CI.

## Error handling

- Expected failures return `pulp::runtime::Result<T, E>` (`Ok` and `Err`) or `std::optional`. Do not signal them with exceptions.
- No exceptions on the audio thread. Elsewhere, catch at the boundary where a third-party API can throw and convert to `Result`.
- Never swallow an error silently. Return it, log it off the audio thread, or assert an invariant.
- Validate input at trust boundaries: files, network, host calls, scripts. Bounds-check before indexing.

## Documentation expectations

- Public behavior changes update the matching page under `docs/`, and `docs/status/*.yaml` when a support level changes.
- A new CLI command needs an entry in `docs/reference/cli.md` and `docs/status/cli-commands.yaml`.
- A new CMake function needs an entry in `docs/reference/cmake.md` and `docs/status/cmake-functions.yaml`.
- A new third-party dependency needs a row in `DEPENDENCIES.md`, an attribution entry in `NOTICE.md`, and a license check. Edits to vendored sources are recorded in `tools/deps/patches.json`.
- Write for a reader who knows audio plugins but not this repository's history. Point at headers, tests and guide pages. Run `tools/check-docs.sh` before pushing.
