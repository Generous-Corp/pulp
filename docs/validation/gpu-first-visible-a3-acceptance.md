# GPU first-visible acceptance receipts

`gpu-first-visible-a3-acceptance.json` is the closed evidence receipt for the
GPU first-visible acceptance gate. Its schema deliberately keeps incomplete
work representable without allowing it to become a pass.

## Verify a receipt

```bash
python3 tools/scripts/gpu_first_visible_a3_acceptance.py verify \
  docs/validation/gpu-first-visible-a3-acceptance.json \
  --evidence-root docs/validation
```

The verifier returns `0` only for a terminal pass, `2` for a valid nonterminal
receipt, and `1` for malformed, inconsistent, missing, or digest-mismatched
evidence. A nonterminal result is never an acceptance pass.

## Generate a receipt

Author a template matching
`docs/contracts/gpu-first-visible-a3-acceptance-v1.schema.json`, using
`"sha256": "auto"` for artifact references. Then run:

```bash
python3 tools/scripts/gpu_first_visible_a3_acceptance.py generate template.json \
  --output acceptance.json --evidence-root /path/to/evidence
```

Artifact paths are relative to the evidence root and may not contain `..`.
Generation hashes the referenced files, applies the full schema and semantic
checks, and writes atomically only after validation.

## Terminal evidence

A complete receipt must bind the exact planning revision and digest, Pulp and
Forge revisions, build, product, plugin format, machine, product instance, and
campaign. It must also contain:

- A ratified `pulp.gpu-first-visible-budget.v1` receipt with its exact raw
  10-cold and 10-warm reference-host artifacts. Version 1 derives the threshold
  deterministically as the larger cache-state p95 plus one bound-host refresh
  interval, rounded up to whole milliseconds; a hand-selected threshold or an
  unbound/implausible reference-host refresh rate is rejected.
- Exactly one passing `pulp.gpu-health-read-result.v1` campaign for standalone,
  headless-constrained, a real DAW/plugin format, and the exact Forge shell.
  Each campaign carries its own raw 10-cold and 10-warm artifacts plus the
  measured product and host artifacts. Every trial must include bounded
  compile, upload, hidden-frame, and native-present timing; a
  `gpu.startup.pass` code cannot substitute for present corroboration. Every
  campaign also carries a nonempty trace and typed digest-bound campaign trace
  analysis. The selected causal campaign additionally receives the full pinned
  analyzer replay described below.
- An exact-digest A2T receipt, pinned analyzer executable, raw Perfetto trace,
  analyzed trace, and binding receipt that agree with one campaign's build,
  instance, campaign, GPU evidence, and trace evidence identifiers. The verifier
  anchors the analyzer digest in the A2T receipt and replays that snapshotted
  analyzer over the snapshotted trace; submitted analysis JSON is not trusted.
- A caught transparent-frame negative and an audio-thread exclusion receipt
  from an external instrumented harness. The receipt must cover the exact known
  `ControlGpuHealthProvider` entry points, record zero events on explicitly
  registered audio threads, and include a non-audio positive control. This is
  external harness evidence; it is deliberately not described as product-runtime
  thread proof.
- Exactly one disposition: `queue-B4`, `queue-B4-investigation`, or
  `no-change`. The verifier derives it from the validated causal campaign and
  replayed A2T result under `pulp.b4-disposition-policy.v1`; neither the health
  provider's advisory pipeline fields nor a hand-authored disposition can select
  the result. The disposition receipt binds the derived inputs and artifact
  digests.

The verifier recomputes nearest-rank p95 values, requires a complete lossless
capture, rejects unavailable or unverified health campaigns, cross-checks raw
samples against the health results, and verifies every declared artifact
digest. The A2T no-producer disposition must also be accepted by the exact
planning revision/digest bound by A3; a stale `requires-approval` receipt cannot
close the causal gate. A product provider's locally complete snapshot is therefore input
evidence, never self-sufficient acceptance. Artifact traversal rejects symlinks
and parses the same immutable byte snapshot it hashes. Partial observations
belong in `observations`; every remaining gap must be named in
`missing_evidence` while `status` remains `incomplete`.
