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
- `JsEngine::register_host_object()` now exposes a first truthful host-object slice on every proven backend:
  - snapshot properties
  - native method callbacks
  - explicit `_objectName` tagging for later bridge code
- `JsEngine::supports_typed_arrays()` defaults to `false`, but backend overrides now matter
- `JsEngine::register_promise_function()` now exposes a first truthful promise slice on every proven backend:
  - native callback result is surfaced as a real JS `Promise`
  - resolution happens on the JS microtask queue through a shared wrapper
  - this is not yet a held native resolver for later completion
- `QuickJS` now reports host-object support through the shared descriptor seam, but still reports typed arrays unsupported through the current Pulp seam
- `JavaScriptCore` now reports host-object support through that same shared descriptor seam
- `V8` now reports host-object support through that same shared descriptor seam when built with the explicit provider contract
- `JavaScriptCore` now surfaces TypedArray / ArrayBuffer values through the current Pulp seam and is covered by shared `JsEngine` tests
- the `V8` backend now truthfully reports typed-array support in code and is proven in a real V8-enabled configure/build/test path when explicit external inputs are supplied
- full held-resolver native async APIs are still not implemented on any backend
- full opaque native wrapper / proxy-style HostObjects are still not implemented on any backend

That means the current engine layer is sufficient for:
- expression evaluation
- module execution
- function registration
- basic bridge-style JS integration
- native-backed object descriptors with properties + method callbacks
- Promise-returning native functions that resolve via the JS microtask queue
- typed-array argument / return-value flows on `jsc`

It is not yet proven sufficient for:
- zero-copy TypedArray / ArrayBuffer exchange
- held-resolver native async APIs such as `mapAsync()`
- opaque native GPU wrapper objects with live property interception / lifecycle hooks

## First Honest Phase 13 Gate

Before calling Phase 13 implementation "in progress", the following should be true:

- [x] prove `quickjs` configure/build/test on current `main`
- [x] prove `jsc` configure/build/test on current `main`
- [x] record the real `v8` configure contract on current `main`
- [x] raise the first truthful capability slice: typed arrays where the backend already supports them
- [x] prove one real V8-enabled configure/build with external V8 inputs
- [x] raise the first truthful host-object slice: native-backed object descriptors with properties + methods
- [x] raise the first truthful promise slice: native callbacks surfaced as JS `Promise` objects
- [ ] decide whether Phase 13 starts as `v8-first` or `quickjs/jsc-first with V8 follow-up`
- [ ] extend the `JsEngine` contract with the remaining minimum deferred async surface
- [ ] add tests for that remaining surface before binding Dawn objects
- [ ] define the first truthful Phase 13 smoke slice

## Recommended Next Steps

1. Add the next capability slice for deferred native async paths instead of leaving that gap as an undocumented stub.
2. Decide whether the first bridge slice targets:
   - a `v8-first` path for Three.js realism, or
   - a thinner engine-agnostic proof slice first, with V8 performance proof immediately after.
3. Define the first truthful bridge smoke slice on top of the new host-object floor.
4. Only then start the actual Three.js / WebGPU bridge branch.

## Related Tracking

- [#89](https://github.com/danielraffel/pulp/issues/89) — parked licensing disposition for Phases 4 and 5
- [#92](https://github.com/danielraffel/pulp/issues/92) — Windows/Linux native WebView parity follow-up
