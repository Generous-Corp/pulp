# Phase 13 Readiness Snapshot

**Last updated:** 2026-04-02
**Branch:** `feature/v3-phase13-readiness`
**Base truth:** `origin/main` after Phase 7 (`#91`) and local-CI safety (`#93`)

This note hardens the exact readiness floor for Phase 13 so the Three.js / WebGPU bridge starts from proven facts instead of future-state assumptions.

## What Is Already True On `main`

- Phase 10 is merged.
- The `JsEngine` abstraction exists with `quickjs`, `jsc`, and `v8` backends.
- Phase 11 is merged, so the WebGPU exploration work is already available.
- Phase 7 is merged, but the shipped native WebView host is currently macOS-only.

## Configure / Build Proof

### QuickJS

Command:

```bash
cmake -S . -B build-phase13-quickjs -DCMAKE_BUILD_TYPE=Debug -DPULP_JS_ENGINE=quickjs
cmake --build build-phase13-quickjs --target pulp-test-js-engine -j8
ctest --test-dir build-phase13-quickjs -R "JsEngine" --output-on-failure
```

Result:
- configure: pass
- build: pass
- shared `JsEngine` tests: `21/21` passed

### JavaScriptCore

Command:

```bash
cmake -S . -B build-phase13-jsc -DCMAKE_BUILD_TYPE=Debug -DPULP_JS_ENGINE=jsc
cmake --build build-phase13-jsc --target pulp-test-js-engine -j8
ctest --test-dir build-phase13-jsc -R "JsEngine" --output-on-failure
```

Result:
- configure: pass
- build: pass
- shared `JsEngine` tests: `21/21` passed

### V8

Command:

```bash
cmake -S . -B build-phase13-v8 -DCMAKE_BUILD_TYPE=Debug -DPULP_JS_ENGINE=v8 \
  -DV8_INCLUDE_DIR=/opt/homebrew/Cellar/node/25.8.2/include/node \
  -DV8_LIB_DIR=/opt/homebrew/Cellar/node/25.8.2/lib \
  -DV8_LIBRARY_PATH=/opt/homebrew/Cellar/node/25.8.2/lib/libnode.141.dylib
cmake --build build-phase13-v8 --target pulp-test-js-engine -j8
ctest --test-dir build-phase13-v8 -R "JsEngine" --output-on-failure
```

Result:
- configure: pass with explicit external V8 inputs
- build: pass with the Node-provided embedder library
- shared `JsEngine` tests: `24/24` passed

Truth:
- the V8 backend exists in code
- V8 is not a turnkey stock-checkout backend today
- V8 is dependency-conditional until explicit external V8 inputs are supplied
- a real V8-enabled proof path now exists on this machine via Homebrew Node's embedder layout
- the current provider contract is:
  - `V8_INCLUDE_DIR` -> header root containing `v8.h`
  - `V8_LIB_DIR` -> library directory
  - `V8_LIBRARY_PATH` -> actual provider library when it is not named `v8_monolith`

## Capability Floor Status For Phase 13

The merged Phase 10 abstraction is real, but the stronger Phase 13-forward capability floor is only partially implemented today.

Current code truth:
- `JsEngine::supports_host_objects()` defaults to `false`
- `JsEngine::supports_typed_arrays()` defaults to `false`, but backend overrides now matter
- `JsEngine::supports_promises()` defaults to `false`
- `QuickJS` still reports typed arrays unsupported through the current Pulp seam
- `JavaScriptCore` now surfaces TypedArray / ArrayBuffer values through the current Pulp seam and is covered by shared `JsEngine` tests
- the `V8` backend now truthfully reports typed-array support in code and is proven in a real V8-enabled configure/build/test path when explicit external inputs are supplied
- HostObjects and Promise-returning native APIs are still not implemented on any backend

That means the current engine layer is sufficient for:
- expression evaluation
- module execution
- function registration
- basic bridge-style JS integration
- typed-array argument / return-value flows on `jsc`

It is not yet proven sufficient for:
- HostObject-style native GPU wrapper objects
- zero-copy TypedArray / ArrayBuffer exchange
- Promise-returning native async APIs such as `mapAsync()`

## First Honest Phase 13 Gate

Before calling Phase 13 implementation "in progress", the following should be true:

- [x] prove `quickjs` configure/build/test on current `main`
- [x] prove `jsc` configure/build/test on current `main`
- [x] record the real `v8` configure contract on current `main`
- [x] raise the first truthful capability slice: typed arrays where the backend already supports them
- [x] prove one real V8-enabled configure/build with external V8 inputs
- [ ] decide whether Phase 13 starts as `v8-first` or `quickjs/jsc-first with V8 follow-up`
- [ ] extend the `JsEngine` contract with the remaining minimum host-object / promise surface
- [ ] add tests for that remaining surface before binding Dawn objects
- [ ] define the first truthful Phase 13 smoke slice

## Recommended Next Steps

1. Add the next capability slice for HostObjects and Promise-returning native async paths instead of leaving them as undocumented stubs.
2. Decide whether the first bridge slice targets:
   - a `v8-first` path for Three.js realism, or
   - a thinner engine-agnostic proof slice first, with V8 performance proof immediately after.
3. Only then start the actual Three.js / WebGPU bridge branch.

## Related Tracking

- [#89](https://github.com/danielraffel/pulp/issues/89) — parked licensing disposition for Phases 4 and 5
- [#92](https://github.com/danielraffel/pulp/issues/92) — Windows/Linux native WebView parity follow-up
