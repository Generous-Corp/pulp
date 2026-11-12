# P0 layered inspection and replay receipt

Status: **PARTIAL / tooling experiment**.

The checked-in fixture receipt is
`p0-layered-inspection-replay-20261112/receipt.json` (SHA-256
`b66229e3d6986d4015fae89939a8ab6f63bdf1c4bbaf7e67b6577d1c0c91c9a6`). Its
consumer gate is **NOT_QUALIFIED** because this checkout could not supply the
installed public binaries: the source build's generated package refers to
relocatable support modules that are materialized only by a completed SDK
install. The missing installed-consumer lane is recorded explicitly rather
than being inferred from the source build.

This evidence-only slice keeps the five inspection layers independent:

1. authored semantic graph bytes and schema;
2. resolved node, implementation, and resource identities;
3. prepared generation, capacity, latency, and fallback facts;
4. event/control inputs with clock epoch and absolute sample positions; and
5. canonical PCM output metadata and digest.

`tools/scripts/p0_layered_replay_receipt.py` composes the existing G3 resolved
identity consumer and the separate spectral frame consumer. The receipt is
versioned as `pulp.graph.layered-replay-receipt.v1` and records each consumer's
command, executable digest, output digest, and return code. The consumer gate
is fail-closed: absent commands exit 2 unless `--allow-unavailable` is used;
that mode writes `NOT_QUALIFIED` and never reports a completed experiment.

The fixture oracle renders sample-level canonical signed 32-bit PCM and records
two partition labels (64 and 128 frames). It does not execute distinct block
boundaries, so the partition control is **NOT_CHECKED**. It plants one mutation
at a time in authored, resolved, plan, event, and output layers. These controls
are explicitly synthetic tooling controls; they do not claim
runtime scheduling, realtime safety, or audio-provider parity.

## Focused controls

```sh
python3 -m unittest tools/scripts/test_p0_layered_replay_receipt.py -v
```

The actual installed-consumer run uses separate commands, for example:

```sh
python3 tools/scripts/p0_layered_replay_receipt.py \
  --output docs/evidence/p0-layered-inspection-replay-20261112/receipt.json \
  --source-head "$(git rev-parse HEAD)" \
  --g3-command /path/to/pulp-g3-resolved-identity-consumer \
  --g3-identity pulp-g3-resolved-identity-consumer \
  --g3-protocol pulp.graph.g3-resolved-identity-consumer.v1 \
  --spectral-command /path/to/spectral-frame-codec-consumer \
  --spectral-identity spectral-frame-codec-consumer \
  --spectral-protocol pulp.signal.spectral-frame-codec-consumer.v1
```

The resulting receipt is qualified only when both independent public consumers
exit zero. Runtime/event scheduler, realtime, cross-platform, provider parity,
and broad graph identity lanes remain unavailable by design.
