# MLX named-model product-gate disposition

Date: 2026-10-04
Source baseline: `origin/main` at `e260a88701eb85d1a22e11952826f98a4a36671c`
Scope: read-only named-model gate investigation; no public ABI, E126 capability
transaction, Dawn ownership, or GPU-NAM source changes

## Disposition

**FAIL / product gate remains open.** A redistributable named fixture is
available, and its CPU bridge is green, but the checked-in MLX worker accepts
only a synthetic matrix. There is no adapter that can load the named fixture,
run it through the model-neutral `StreamingModel` contract, or compare MLX
output with the existing CPU shadow. The synthetic worker receipt must remain
non-product evidence.

This is an evidence receipt, not a capability claim. In particular, it does
not claim MLX execution for `NeuralProvider::Mlx`, an acceleration win, or a
passing deadline/fallback gate.

## Named artifact and license

The smallest named artifact found in the protected Pulp tree is
`test/fixtures/neural/example.nam`:

| Field | Value |
| --- | --- |
| Bytes | 3,941 |
| SHA-256 | `66bda2b379289eff079c0755588bc9a92760654d9cc9af1b97cf30d0e92b167d` |
| Architecture | Neural Amp Modeler `WaveNet` A1 |
| Sample rate | 48,000 Hz |
| Serialized weights | 131 |
| Metadata name | `Test Model` |
| Model shape | 2 arrays; channels 3 then 2; receptive field 22 samples |

The fixture is the MIT-attributed Neural Amp Modeler example capture. The
attribution was checked against the sibling `pulp-gpu-nam` checkout at
revision `014f24325a7f4211ee5052993badd9c90151b414`; Pulp's existing
`docs/status/neural-nam-tcn-artifact-bridge-receipt-20261003.md` records that
provenance and the attribution-file hashes. The license is therefore suitable
for a fixture-only measurement, subject to retaining that attribution in any
future MLX receipt or bundle.

## Existing CPU proof

The private `NamTcnArtifactAdapter` in
`core/gpu_audio/src/detail/nam_tcn_artifact.hpp` parses this format on the
control path and advances preallocated causal state in `process_cpu()`. The
fresh baseline was configured and built with the governed command
`pulp build --target pulp-test-nam-tcn-adapter`.

The exact discovered CTest names were run with:

```text
ctest --test-dir build --output-on-failure -R '^(NAM/TCN adapter|serialized NAM)'
6/6 passed, 0.11 s
```

The artifact case covers 64- and 128-frame processing, reset replay, and the
documented CPU-oracle samples. The negative cases reject unsupported state
offsets and a divergent serialized head scale. The direct executable
`build/test/pulp-test-nam-tcn-adapter` also passed 32 assertions in 6 cases.

These tests prove the CPU shadow and artifact admission only. They do not
create an MLX representation.

## MLX harness boundary and observed probe

`tools/validation/mlx_worker_harness.py` has no model/artifact argument. Its
worker allocates `mx.ones((frames, frames))`, evaluates `tanh(matmul(...))`,
and reports a declared synthetic weight size. It never reads `.nam`, calls a
`StreamingModel`, or emits CPU-shadow output, provider-selection state,
transport counters, fallback blocks, or parity residuals.

The existing isolated environment (`mlx==0.32.3`, `mlx-metal==0.32.3`) was
available on this arm64 macOS host. A short two-instance probe was run only to
verify the harness boundary:

```text
mlx_version=0.32.3
device=Device(gpu, 0)
host=macOS-27.0.1-arm64-arm-64bit-Mach-O
instances=2, paced=true, frames=32, sample_rate=48000, blocks=8
worker 0: owner/release consistent, deadline_misses=5, resident_weight_bytes=4224
worker 1: owner/release consistent, deadline_misses=5, resident_weight_bytes=4224
phase3_gate=not_claimed: synthetic workload and no audio callback/model oracle
```

The synthetic probe is useful evidence for one owning thread per worker and
the worker watchdog. Its `deadline_misses` are matrix-service misses, not
audio transport misses. The `resident_weight_bytes` field is a formula for the
synthetic arrays, not measured residency of a named model.

MLX's allocator counters are available, but they do not close this gate. For
the same 32-frame synthetic arrays, `mx.get_active_memory()` changed from
`0` to `4,232` bytes against a declared `4,224` bytes, and remained `4,232`
after deleting arrays and clearing the cache. This demonstrates why a future
receipt must record both active and peak memory, instance count, and release
behavior; it is not named-model residency evidence.

## Why the existing bridge cannot be invoked from this worker

The CPU bridge and MLX worker are separate private surfaces:

* `NamTcnArtifactAdapter` is a C++ `StreamingModel` implementation whose
  `process_cpu()` receives Pulp `BufferView` objects and owns C++ causal state.
* The MLX probe is a Python tools-only process. It imports `mlx.core` on its
  worker thread and has no FFI, IPC protocol, or callback table for a Pulp
  `StreamingModel`.
* The private `NeuralProcessor` currently records a requested MLX preference as
  a CPU fallback until a prepared backend exists. No MLX class, dependency,
  checkpoint loader, or model-neutral backend implementation is present in the
  build graph.

Bridging these surfaces by copying GPU-NAM code, adding a public provider API,
or treating synthetic arrays as the named model would violate the requested
scope and would make the receipt fail closed less reliably. No such source was
added.

## Smallest next implementation packet

The next packet can remain private and default-off:

1. Add a tools-only named-fixture runner that parses the immutable `.nam`
   fixture and constructs an MLX worker model from the same serialized weights.
   Keep loading, array creation, evaluation, reset, and release on one owning
   worker thread.
2. Invoke the model-neutral streaming contract through a private adapter and
   run the same deterministic input through the existing CPU artifact bridge
   as a continuously primed shadow. Emit a max residual and a nonzero
   `parity_failures` counter rather than a boolean capability claim.
3. Run N=1 and N=2 instances with stable instance IDs and owner/release thread
   IDs. Emit `selected_provider`, actual MLX execution count, `gpu_delivered`,
   `transport_misses`, `deadline_misses`, `fallback_blocks`, and a forced-late
   `LateRejected` disposition.
4. Measure `mx.get_active_memory()` and peak memory before/after preparation,
   after N=2, and after release. Record allocator/cache caveats and the exact
   fixture/runtime hashes. Do not call declared bytes “residency.”
5. Only after those receipts exist should the product gate be reconsidered;
   synthetic timing and CPU-only fallback remain the safe disposition until
   then.

## Checks

- `git rev-parse HEAD` = `e260a88701eb85d1a22e11952826f98a4a36671c`
- `pulp build --target pulp-test-nam-tcn-adapter` completed in Release mode
- Exact NAM CTest filter: 6/6 passed
- `python3 -m py_compile tools/validation/mlx_worker_harness.py`
- MLX probe completed with `status=ok` and explicit `phase3_gate=not_claimed`
- No source, public ABI, capability transaction, or GPU-NAM ownership files
  changed by this packet
