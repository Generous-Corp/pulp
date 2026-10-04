# Dawn WaveNet depth gate audit (2026-10-04)

## Disposition

**FAIL / gate remains open.** The current Pulp source has a bounded production
multi-flight implementation, but no checked-in receipt proves authenticated
Dawn WaveNet depth of at least two together with ordered delivery, late
retirement, device-loss handling, and a throughput or deadline-margin result.
This audit is documentation-only; it does not change the E126 capability
transaction or any GPU-NAM source.

The audit was rebased onto protected `origin/main` at
[`df6f3076f909fee587d32a01cff3477617591785`](https://github.com/Generous-Corp/pulp/commit/df6f3076f909fee587d32a01cff3477617591785).

## Current production depth

`GpuWaveNetRealtimeNode::Impl::valid_config()` requires
`max_inflight <= session.slots` and `max_inflight <= capacity - lead_blocks`;
`initialize()` allocates a `pending_ring` of exactly `max_inflight` entries
([`core/gpu_audio/src/gpu_wavenet_realtime_node.cpp:196-229`](../../core/gpu_audio/src/gpu_wavenet_realtime_node.cpp#L196-L229)).
The worker admits while that ring has space, submits each channel, increments
`pending_count`, and records `in_flight_high_water`
([`core/gpu_audio/src/gpu_wavenet_realtime_node.cpp:545-589`](../../core/gpu_audio/src/gpu_wavenet_realtime_node.cpp#L545-L589)).
This establishes source capability and the bounded lifecycle; it does not
establish that a real provider actually has two physical GPU submissions in
flight.

The current synthetic depth test passed:

```text
pulp build --target pulp-test-gpu-wavenet-realtime-node
build/test/pulp-test-gpu-wavenet-realtime-node \
  '[gpu_audio][wavenet][realtime][depth]' --reporter compact
All tests passed (10 assertions in 1 test case)
```

The test runs capacities 2 and 8 through a fake `WaveNetRealtimeChannel` and
only checks `in_flight_high_water >= 1` plus monotonicity
([`test/test_gpu_wavenet_realtime_node.cpp:341-383`](../../test/test_gpu_wavenet_realtime_node.cpp#L341-L383)).
It does not require high-water depth 2, identify a provider, capture GPU
timestamps, or measure deadline margin. The full realtime suite also passed
463 assertions in 18 cases; that remains synthetic channel evidence.

## Shared-I/O slot ledger and provider receipts

The focused private lifecycle binaries passed on this checkout:

| Binary/filter | Result |
| --- | --- |
| `pulp-test-gpu-shared-io-slot-ledger [deadline]` | 42 assertions, 2 cases |
| `pulp-test-gpu-shared-io-slot-ledger [retirement]` | 112 assertions, 6 cases |
| `pulp-test-gpu-shared-io-slot-ledger [completion]` | 108 assertions, 6 cases |
| `pulp-test-gpu-shared-io-convolution-pipeline` | 171 assertions, 9 cases |
| `pulp-test-gpu-shared-io-convolution-session` | 232 assertions, 18 cases |

These tests prove generic fixed-slot chronology, late-retirement credit, and
provider-loss fencing with deterministic/private test providers. They do not
bind those results to the WaveNet realtime node or an authenticated adapter.
The historical direct-executable receipt records the same boundary and
explicitly says that a live adapter probe and hardware submission were not
claimed ([`2026-10-02-dawn-phase-e-direct-executables.md`](2026-10-02-dawn-phase-e-direct-executables.md)).

The public WaveNet session slot matrix also passed 123 assertions in 4 cases
(`pulp-test-gpu-wavenet-session`), but the test permits
`ProviderUnavailable`, emits no adapter identity or high-water receipt, and
does not exercise late completion or device loss. It is therefore a session
capability check, not this depth gate.

## Why no exact-provider WaveNet receipt exists

The ordinary Release configure used for the focused tests has
`PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO=TRUE` but
`PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF=OFF`. The CMake graph creates
`pulp-gpu-dawn-shared-io-provider-probe` only when the exact-provider option is
enabled ([`test/cmake/render_gpu_surface_tests.cmake:206-234`](../../test/cmake/render_gpu_surface_tests.cmake#L206-L234));
that target is absent from this build's Ninja graph. The option is explicitly
opt-in and requires GPU-enabled Apple Silicon ([`CMakeLists.txt:103-112`](../../CMakeLists.txt#L103-L112)).
The checked-in WaveNet depth receipt is still the historical synthetic
negative control: capacities 2 and 8 both recorded high-water depth 1 and no
GPU timestamps ([`2026-10-02-dawn-wavenet-depth-comparison.md`](2026-10-02-dawn-wavenet-depth-comparison.md)).
No checked-in M5 receipt closes this WaveNet depth gate.

## Smallest next packet

Run a dedicated Apple-Silicon exact-provider configure with
`PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF=ON`, then add one private WaveNet probe
under the GPU-audio owner that:

1. records the manifest-pinned Dawn revision and adapter identity;
2. submits at least two blocks before servicing and records provider slot
   tokens, submission count, and `in_flight_high_water >= 2`;
3. verifies contiguous ordered output and a direct CPU oracle;
4. injects or observes one late terminal and one device-loss terminal, proving
   physical slot retirement before reuse and fail-closed recovery; and
5. compares depth 1 and depth 2 on the same host/block size with provider
   timestamps or a paced deadline-margin measurement.

Until that packet exists, the production source capability and synthetic tests
must remain labeled partial evidence; they cannot be promoted to a Dawn GPU
performance or acceptance claim.
