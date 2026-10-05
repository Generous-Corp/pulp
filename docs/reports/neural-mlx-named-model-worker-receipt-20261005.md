# MLX named-model worker receipt

Date: 2026-10-05
Work package: D (MLX Apple Silicon provider)
Source baseline: `origin/main` at `f85a50352a46b7e2a6a302f1b4208fdc4f8ee0fa`
Status: bounded private evidence; Phase 3/product gate **not claimed**

## Scope and safety boundary

`tools/validation/mlx_named_model_harness.py` is a tools-only, default-off
probe. It does not change Pulp's public ABI, capability manifest, CMake graph,
plugin build, callback path, or provider selection. Each worker loads the
checked-in named fixture, creates and evaluates MLX arrays, and releases them
on its own Python thread. A separate CPU implementation mirrors the private
`NamTcnArtifactAdapter` arithmetic and is used as a continuously primed shadow.

The JSON fields `model_id`, `selected_provider`, `fallback_contract`,
`owner_thread_id`, `release_thread_id`, `max_residual`, `parity_failures`,
`deadline_misses`, and allocator snapshots are receipt semantics for this
private probe. They are not a unified public control or receipt ABI. A plugin
or SDK client has no way to observe this probe through Pulp; it would need a
future reviewed adapter and projection before provider, latency, fallback, or
receipt state could be exposed.

## Named artifact and runtime

- Fixture: `test/fixtures/neural/example.nam`, metadata name `Test Model`
- `model_id`: `nam.example`
- Fixture SHA-256: `66bda2b379289eff079c0755588bc9a92760654d9cc9af1b97cf30d0e92b167d`
- Architecture: serialized NAM WaveNet A1; 48 kHz; 131 weights; receptive field 22
- MLX environment: `mlx==0.32.3`, `mlx-metal==0.32.3`
- Wheel hashes are unchanged from [`neural-mlx-execution-receipt-20261002.md`](neural-mlx-execution-receipt-20261002.md):
  `mlx` `452c621862684e8769be93c1517420c8ba2fd6e8b01a1c970c8ba8069022f0ac`;
  `mlx-metal` `34ae9b83ad2f0ccdd3e5d48ec35176e7119f57069eef187122916dc941a4ae1f`

## Measured run

Command:

```text
/tmp/pulp-mlx-venv-20261002/bin/python \
  tools/validation/mlx_named_model_harness.py \
  --model test/fixtures/neural/example.nam --blocks 8 --instances 2 --json
```

Host: arm64 macOS 27.0.1; MLX reported `Device(gpu, 0)`.

| worker | owner/release thread | evals | max residual | parity failures | deadline misses | p50 / p95 service (us) | active before / prepared / release (bytes) | peak (bytes) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 6150795264 / 6150795264 | 256 | 1.2601506e-07 | 0 | 8 | 112456.6 / 119473.5 | 0 / 1412 / 20 | 1972 |
| 1 | 6167621632 / 6167621632 | 256 | 1.2601506e-07 | 0 | 8 | 112806.1 / 119101.6 | 84 / 1444 / 976 | 1972 |

The CPU shadow also reproduces the existing C++ oracle samples:
`-0.01314363`, `-0.012158165`, `-0.014329166`, and `-0.016419834`.
The MLX run reported `selected_provider=mlx`,
`fallback_contract=cpu_control_available; Pulp transport fallback not
exercised`, and `phase3_gate=not_claimed`.

The nonzero post-release active-memory values are allocator/cache observations,
not proof of retained model residency. The service times and deadline misses
are worker-loop measurements, not audio callback deadlines.

## Gate disposition and next evidence

This closes the named-fixture load, worker ownership, MLX execution, and CPU
parity portions of the private measurement slice. It does **not** close Phase 3
or product acceptance. The remaining required evidence is:

1. A reviewed private adapter into Pulp's model-neutral streaming/transport
   contract, with actual provider identity projected through unified controls.
2. CPU fallback under forced provider failure and late completion, including
   fallback block counts and stale-result rejection in the real transport.
3. N=2 isolation and residency/thermal/contended-load measurements at the
   declared paced workload, including a bounded 100,000-block campaign.
4. A plugin/SDK-visible receipt projection whose provider, latency, fallback,
   and provenance fields are backed by the actual product path.

Until those controls exist, this receipt remains private evidence and makes no
GPU acceleration, deadline, transport, or shipped-provider claim.

## Tests

- `python3 -m py_compile tools/validation/mlx_named_model_harness.py tools/validation/test_mlx_named_model_harness.py`
- Default Python: `python3 -m unittest tools/validation/test_mlx_named_model_harness.py -v` — 1 pass, 1 expected skip (MLX absent).
- Isolated MLX Python: `/tmp/pulp-mlx-venv-20261002/bin/python -m unittest tools/validation/test_mlx_named_model_harness.py -v` — 2 passes.
- Named N=2 run above — exit 0, two owner/release pairs, zero parity failures.
