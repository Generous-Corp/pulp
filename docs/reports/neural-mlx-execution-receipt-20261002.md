# MLX execution probe receipt

Date: 2026-10-02
Work package: D (MLX Apple Silicon provider)
Status: private harness proven; public ABI unchanged

## Host and pinability

Host probes:

- macOS 27.0.1 (build 26A434), arm64
- Xcode 27.0 (27A266a)
- default `/opt/homebrew/bin/python3`: MLX unavailable (`ModuleNotFoundError`)
- PyPI arm64 wheels are available and installable in an isolated Python 3.14
  virtualenv:
  - `mlx==0.32.3`, wheel SHA-256
    `452c621862684e8769be93c1517420c8ba2fd6e8b01a1c970c8ba8069022f0ac`
  - `mlx-metal==0.32.3`, wheel SHA-256
    `34ae9b83ad2f0ccdd3e5d48ec35176e7119f57069eef187122916dc941a4ae1f`

Installing only `mlx` failed because its required `mlx-metal` runtime was
absent. Installing both wheels in `/tmp/pulp-mlx-venv-20261002` imported
successfully and reported `Device(gpu, 0)`. No dependency or manifest was added
to Pulp.

## Private harness

`tools/validation/mlx_worker_harness.py` is now present on `origin/main` as a
tools-only, default-off synthetic probe (landed through
[PR 9332](https://github.com/Generous-Corp/pulp/pull/9332)). It
creates/evaluates/releases synthetic arrays on one dedicated Python thread per
instance and reports owner-thread identity, service-time percentiles, deadline
misses, and a conservative synthetic weight size. It does not expose MLX types,
handles, streams, or paths through Pulp's public ABI and is not wired into a
plugin build. It is a landed developer/validation tool, not a shipped MLX
provider or product feature; the provenance audit records the source snapshot
and merged-path status.

The host-default invocation correctly reports MLX unavailable. In the isolated
venv, the following run completed:

```text
mlx 0.32.3, Device(gpu, 0), arm64 macOS 27.0.1
instances=2, paced=true, frames=32, sample_rate=48000, blocks=100000
elapsed_seconds=66.749967542
worker 0: thread=6155300864, release_thread=6155300864,
  p50=231.917 us, p95=296.084 us, p99=345.083 us,
  max=21400.708 us, deadline_misses=84, eval_count=100001
worker 1: thread=6172127232, release_thread=6172127232,
  p50=224.708 us, p95=295.708 us, p99=350.126 us,
  max=21474.917 us, deadline_misses=79, eval_count=100001
```

A bounded single-block check also completed with two instances (`eval_count=2`
per worker and `owner_thread_consistent=true`). The host-default Python path
still returned the expected unavailable result, so the failure path remains
explicit rather than being mistaken for a CPU or MLX success.

The watchdog controls were exercised against the synthetic (fake-provider)
worker. With `--blocks 100000 --instances 2 --worker-timeout-seconds 0.001
--json`, the command exited 1 with `status=error`,
`reason=worker_timeout`, both workers listed alive, and the explicit
`phase3_gate=not_claimed` marker. No partial worker result was emitted. A short
default-paced run with `--blocks 8 --instances 2 --json` derived a 30.0-second
timeout, exited 0 in 0.015292959 seconds, and returned `eval_count=9` for each
worker. These are watchdog/failure-path observations only; they do not add
transport, fallback, parity, thermal, or product-model evidence.

The run proves that two MLX owners can execute concurrently while retaining
per-instance thread ownership in this synthetic tool. It does **not** prove a
Pulp audio deadline or an acceleration win: the workload is a tiny synthetic
matrix operation, there is no CPU shadow, no model-quality oracle, no Pulp
transport, no callback, no thermal/contended-load capture, and the weight-size
field is only the synthetic array allocation. The landed tool therefore does
not change the current CPU-only processor facade or constitute shipped-product
evidence.

## Gate disposition

The run covers the requested N=2/100,000-block pacing attempt, but it cannot
pass the planning Phase 3 gate. The synthetic deadline misses (84 and 79) are
not transport misses, and there is no GPU-delivery/parity/fallback accounting.
A named TCN/SSM model, CPU oracle, continuously primed fallback, Pulp transport,
thermal state, host contention, memory residency/allocator delta, and forced
late-completion test remain required. The result therefore records **evidence
available, gate not claimed**.

Next safe measurement is to adapt this private worker behind a model-specific
adapter and feed it the existing Pulp paced harness, while retaining one owner
thread per prepared runtime. Only then can shared-worker versus per-instance
weights be decided from real N=2 residency and deadline data.
