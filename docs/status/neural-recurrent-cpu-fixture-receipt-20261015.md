# Synthetic recurrent CPU fixture receipt

Date: 2026-10-15  
Baseline: `e86ed199bad9eabe55a22fe6fa877342115613b8`  
Worktree: `/Users/danielraffel/Code/pulp-neural-cpu-fixture-20261015`  
Scope: private CPU contract evidence only

The protected baseline already contains the smallest bounded recurrent seam in
[`core/gpu_audio/src/detail/recurrent_cpu_adapter.hpp`](../../core/gpu_audio/src/detail/recurrent_cpu_adapter.hpp)
and its isolated test target in
[`test/test_recurrent_cpu_adapter.cpp`](../../test/test_recurrent_cpu_adapter.cpp).
This receipt records the evidence packet without adding a public ABI, provider
registration, model catalog entry, GPU execution path, or product claim.

## Fixture and contract

The fixture is synthetic, in-memory LSTM and GRU callback kernels (one layer,
one causal direction, one input and output, hidden size 2). It has no external
weights, license record, serialized artifact, or independent model oracle. The
test oracle is a separately written scalar implementation using the same
declared equations and input corpus. The adapter admits only bounded shapes,
requires the state schema and byte count, and performs all preparation and
release through the private `StreamingModel` seam.

The tests cover 32/64/128-frame partitioning, reset replay, lifecycle retry and
reprepare, alias/partial-overlap and oversized-buffer rejection, and a
`NeuralProcessor` generation transition. The RT path is checked with
`ScopedRtProcessProbe`; a planted negative callback records one allocation,
one lock, and one blocking event through `RtContractProbe`.

## Pinned provenance

| Field | Value |
| --- | --- |
| source commit | `e86ed199bad9eabe55a22fe6fa877342115613b8` |
| build | Release, CMake/Ninja through `pulp build --target pulp-test-recurrent-cpu-adapter` |
| compiler | Apple clang 21.0.0.21000334 (`/usr/bin/c++`) |
| host | macOS Darwin 27.0.0, arm64, `Daniels-M5-Studio` |
| Python | 3.15.0 (toolchain check warns pinned manifest expects 3.14.4) |
| CMake | 4.4.3 (toolchain check warns pinned manifest expects 4.3.3) |
| neural artifact | none; synthetic callback state only |

The toolchain warnings are recorded rather than silently treated as pinned
toolchain proof. No physical Dawn/WebGPU/provider admission was attempted.

## Result and disposition

The governed Release build completed and the direct focused binary passed:

```
./build/test/pulp-test-recurrent-cpu-adapter --reporter compact
All tests passed (332 assertions in 11 test cases)
binary SHA-256: 845bcb260e9e456e2aa3e1bfd82a23effabb760d51702703e69fbf2f792003f1
```

The generated CTest registration was present, but this checkout's top-level
CTest discovery did not select the target; the direct binary is the authoritative
focused result. A green result is CPU contract evidence only. It does not establish
TCN/SSM support, a licensed model artifact, GPU execution, or shipped product
support. The next packet requires an immutable licensed artifact, tensor/state
layout and hash, independent oracle vectors, and a host/product integration
receipt before any model-support claim.
