# Neural-audio evidence closeout (2026-10-07)

Source baseline: [`b879a14481f013d1367a343dd358c478642a9475`](https://github.com/Generous-Corp/pulp/commit/b879a14481f013d1367a343dd358c478642a9475), the protected `origin/main` head used for this review.

This is a bounded evidence closeout. It records what is proven, what remains
blocked, and the adversarial checks applied to prevent an unsupported product
claim.

## Proven in this packet

The private, default-off MLX named-model harness was run against
`test/fixtures/neural/example.nam`:

- fixture SHA-256: `66bda2b379289eff079c0755588bc9a92760654d9cc9af1b97cf30d0e92b167d`
- MLX: `0.32.3`; device: `Device(gpu, 0)`; host: arm64 macOS 27.0.1
- two workers, 8 blocks each, 256 evaluations per worker
- six harness tests passed, including CPU-oracle samples, reset replay,
  malformed-input rejection, MLX parity, and bounded timing storage
- both workers reported `selected_provider=mlx`, zero parity failures, and
  matching owner/release thread IDs
- maximum CPU-shadow residual: `1.2601506155229814e-07`
- observed peak memory: `1980` bytes per worker

Commands:

```text
/tmp/pulp-mlx-venv-20261002/bin/python -m unittest tools/validation/test_mlx_named_model_harness.py -v
/tmp/pulp-mlx-venv-20261002/bin/python tools/validation/mlx_named_model_harness.py --model test/fixtures/neural/example.nam --blocks 8 --instances 2 --json
```

## Explicit disposition

This packet proves named-fixture loading, MLX execution, worker ownership, and
CPU parity for the private harness. It does **not** prove a Pulp transport,
callback, plugin, or shipped provider. Each worker recorded eight deadline
misses; the harness reports `phase3_gate=not_claimed`, and its fallback contract
states that Pulp transport fallback was not exercised. The result therefore
remains private feasibility evidence, not product acceptance or a performance
claim.

The following gates remain open and require external prerequisites:

| Gate | Required proof | Disposition |
| --- | --- | --- |
| MLX product path | reviewed Pulp adapter, callback/transport fallback, late-result rejection, and a bounded paced campaign | Open; harness is ready, product adapter is absent |
| Dawn | exact provider and adapter identity, depth >=2, ordered delivery, device-loss recovery, fresh host receipt | Blocked pending authenticated provider/device run |
| Forge/Magenta | named licensed artifact, paced generation, named consumer, cancellation and underrun receipt | Blocked; current state is metadata-only |
| Apple host promotion | host-bound cold/steady campaign, signed model/package hashes, reset/device-loss evidence | Open; existing host observations are experimental |
| Plugin/package | actual consumer path, packaged signed artifact, host receipt, CPU fallback | Open; no public neural consumer is claimed |
| Competitive performance | matched model/corpus, 100,000 paced blocks, complete counters, bootstrap intervals, quality parity | Open; no ranking claim is made |

## Adversarial closeout checks

The packet was rejected as acceptance if any of the following occurred: a
synthetic workload was substituted for the named fixture; CPU fallback was
labelled as GPU execution; provider or executable identity was absent; hashes
were stale; deadline counters or denominators were omitted; allocator bytes
were described as residency; a queued or skipped run was treated as green; a
plugin or package path was bypassed; or a result was generalized across Apple
hosts. None of those shortcuts is present in this receipt.

The next implementation packet is therefore dependency-ordered: first add a
reviewed private adapter with real transport fallback and late-result controls;
then collect exact-provider Dawn or MLX product receipts; only after those
pass, attempt Forge consumer/packaging and the full competitive campaign.
