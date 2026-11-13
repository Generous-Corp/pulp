# P0 layered inspection and replay receipt

Status: **PARTIAL / tooling experiment**.

The checked-in fixture receipt is
`p0-layered-inspection-replay-20261112/receipt.json` (raw-file SHA-256
`0b6965f5987c9823dee12466b06c671892c5473ff3018258d0cac4f68909e253`; the
canonical JSON digest is
`2c4cc34fa164eaa09ab17bb04bf5c7cff715af39730b97a19eb624f552a2fb72`). Its
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
identity consumer and the separate spectral frame consumer when supplied. The
receipt includes canonical semantic bytes and digests for every layer,
prepared capacity/latency/fallback facts, event stream and epoch, source/tool
and SDK provenance, and canonical PCM format/channel/rate/partition/quality
metadata. The consumer fixtures emit explicit protocol and partition
attestation tokens
(`pulp.graph.g3-resolved-identity-consumer.v1` and
`pulp.signal.spectral-frame-codec-consumer.v1`) that attest only the bounded
invocation of the named consumer. They do not bind rendered output to an
installed-SDK package, source tree, or runtime implementation. This is fixture
output metadata only and does not add a runtime/API surface. The focused
self-test guards both tokens. The
checked-in envelope and layer values are synthetic tooling fixtures; they do
not invoke the production serializer, resolver, prepared-plan compiler,
notation, or probe paths. The receipt is
versioned as `pulp.graph.layered-replay-receipt.v1` and records each consumer's
command, executable digest, output digest, and return code. The consumer gate
is fail-closed: absent or invalid consumer commands exit 1 unless
`--allow-unavailable` is used;
that mode writes `NOT_QUALIFIED` and never reports a completed experiment.

The planted-negative controls below are synthetic bookkeeping only; they are
not parser/replay qualification.

The fixture oracle renders canonical signed 32-bit PCM and records two
partition labels (64 and 128 frames). Each installed consumer is run at both
labels, and the gate requires both protocol and partition attestations, but
the fixture does not execute distinct block boundaries, so partition execution
is **NOT_CHECKED**. It records one mutation at a time in authored, resolved,
plan, event, and output layers as synthetic digest attribution bookkeeping;
parser/replay rejection and positive acceptance remain **NOT_CHECKED**. These
controls remain bounded evidence and do not claim runtime scheduling, realtime
safety, or audio-provider parity.

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

The resulting receipt would still require both independent public consumers to
exit zero, native-magic/protocol checks to pass, and independent runtime
partition and mutation acceptance evidence. Those latter checks are absent in
this fixture, so both consumers cannot qualify this receipt and
`consumer_gate` remains `NOT_QUALIFIED`. Runtime/event scheduler, realtime,
cross-platform, provider parity, and broad graph identity lanes remain
unavailable by design.
