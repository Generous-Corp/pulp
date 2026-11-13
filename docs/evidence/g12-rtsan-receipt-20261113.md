# G-12 RTSan routed/direct receipt, 2026-11-13

Evidence only. No runtime or test change. Issue 12177, item 10.

## Subject

- Source head: `d486ff70c6b02c59acdeac504af747724d1fb906` (origin/main, `Merge pull request #12175`).
- Fixture: `test/fixtures/g5_graph_event_rt_cases.inc`, test case
  `G12 prepared gain activation has one exact audio transition under paced callbacks`
  (CMake group `pulp-test-group-trigger-graph`, built from `test/test_trigger_graph.cpp`).
- Selection assertions in that case: routed `blocks_processed == kCallbacks`, direct `== 0`,
  `walk_fallbacks == 0`, `execution_failures == 0`, per-callback allocations/locks/blocking `== 0`.
- Callback boundary: `graph_callback_boundary` carries `[[clang::nonblocking]]` when
  `__has_feature(realtime_sanitizer)`; backend selectable with `G12_RTSAN_BACKEND=routed|direct`;
  planted negatives with `G12_RTSAN_CONTROL=malloc|mutex|sleep|write|clean`.

## Toolchain

- Host: arm64 macOS 27.2 (Darwin 27.2.0), shared and loaded (load about 27 on 28 cores).
- Compiler: Homebrew `clang version 21.1.8`, target `arm64-apple-darwin27.2.0`, at
  `/opt/homebrew/opt/llvm@21/bin`. Apple clang does not support RTSan on this host
  (see `docs/reports/g12-baseline-20261105/README.md`), so upstream LLVM 21 was used.
- SDK: `/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk`. With the default
  `MacOSX27.2.sdk`, Homebrew libc++ fails in `<complex>` (`INFINITY`/`NAN` undeclared, via
  Accelerate `lapack_types.h`) in `core/simd/src/accelerate.cpp`; the 26.5 SDK compiles clean.
  This is a host/SDK interaction, not a repo change.

## Build

```
cmake -S . -B build-rtsan -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm@21/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm@21/bin/clang++ \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk \
  -DPULP_SANITIZER=realtime -DPULP_BUILD_TESTS=ON -DPULP_BUILD_EXAMPLES=OFF \
  -DPULP_ENABLE_GPU=OFF -DPULP_TEST_PCH=OFF
PULP_BUILD_JOBS=4 nice tools/ci/governed-build.sh cmake --build build-rtsan \
  --target pulp-test-group-trigger-graph
```

Compile line carries `-O3 -DNDEBUG -fsanitize=realtime -fno-omit-frame-pointer -g`.
`pulp-test-trigger-graph` is not a target name; the buildable target is the group above.
Build rc 0. The tartci lease was denied (capacity), so the build ran leaseless at `-j2`.

## Results

Run from `build-rtsan/test`, `T="G12 prepared gain activation has one exact audio transition under paced callbacks"`:

| Case | Command | Exit | Result |
| --- | --- | --- | --- |
| Routed backend | `G12_RTSAN_BACKEND=routed ./pulp-test-group-trigger-graph "$T"` | 0 | All tests passed (421 assertions); 0 RTSan reports |
| Direct/reference backend | `G12_RTSAN_BACKEND=direct ./pulp-test-group-trigger-graph "$T"` | 0 | All tests passed (421 assertions); 0 RTSan reports |
| Both backends | `./pulp-test-group-trigger-graph "$T"` | 0 | All tests passed (838 assertions) |
| Clean control | `G12_RTSAN_CONTROL=clean G12_RTSAN_BACKEND=routed ...` | 0 | passes |
| Planted malloc | `G12_RTSAN_CONTROL=malloc G12_RTSAN_BACKEND=routed ...` | 134 | DETECTED |
| Planted mutex | `G12_RTSAN_CONTROL=mutex ...` | 134 | DETECTED |
| Planted sleep | `G12_RTSAN_CONTROL=sleep ...` | 134 | DETECTED |
| Planted write | `G12_RTSAN_CONTROL=write ...` | 134 | DETECTED |

Also run under the same binary (RTSan runtime linked, no report, exit 0):
`G5 graph event callbacks keep fixed storage across both serial backends` (65822 assertions) and
`G5 callback reset retires a queued graph event without allocating` (168 assertions). Note these
two cases do not use the G12 boundary annotation, so they are not RTSan-bounded callbacks.

No RTSan violation occurred in any positive case.

### Control detection excerpt (`G12_RTSAN_CONTROL=malloc`)

```
==76362==ERROR: RealtimeSanitizer: unsafe-library-call
Intercepted call to real-time unsafe function `malloc` in real-time context!
    #0 ... in malloc+0x20 (libclang_rt.rtsan_osx_dynamic.dylib:arm64+0x34c4)
    #1 ... in g12_activation_fixture::inject_analyzer_control(...) g5_graph_event_rt_cases.inc:506
    #2 ... in g12_activation_fixture::graph_callback_boundary(...)
    #3 ... in std::__1::__thread_proxy ...
SUMMARY: RealtimeSanitizer: unsafe-library-call g5_graph_event_rt_cases.inc:506 in ...inject_analyzer_control(...)
```

The mutex control reports `std::__1::mutex::lock()`, the sleep control reports
`std::__1::this_thread::sleep_for`, and the write control reports a call at
`g5_graph_event_rt_cases.inc:520`; each exits 134 with the same `unsafe-library-call` class.

## Boundary of the claim

Local arm64 macOS, Release, N=1 run per case, one source head. The positive cases prove the
tested callbacks (the G12 gain activation fixture, both serial backends) are RTSan-clean and
that the analyzer is sensitive to malloc, mutex, sleep and write inside the same annotated
boundary. It does not cover Linux/LLVM 20, other geometries, parallel execution, or timing
(deadline) qualification. The hosted `rtsan` workflow job remains dispatch-only.
