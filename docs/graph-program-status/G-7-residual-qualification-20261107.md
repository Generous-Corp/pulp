# G-7 residual qualification — installed consumer and probe parity

Owner: g7-residual-qualification  
Started from `origin/main`: `a5ebbeca6139db88265ea93997d70927d3f60aa4`  
State: **PARTIAL — bounded cross-head evidence; exact-head SDK receipt unavailable**

## Evidence

The current `origin/main` source was configured as Release and built with the
governed build wrapper (`-j1`) for `pulp-cpp` and `pulp-mcp`. The external
`examples/graph-edge-probe` consumer was configured against the previously
published installed SDK at source head
`8ecb5db3c9c0323ed065d7b9055e217ba715b292`; its CTest passed `1/1` and the
direct consumer run passed `567` checks.

The current-main CLI and MCP binaries were then run against a newly generated
fixture from that external consumer:

```text
graph-edge-probe PASS checks=567
graph-edge-probe installed CLI/MCP parity PASS
```

This exercised the positive audio-edge publication, plan and latency
introspection, node loads, CLI/MCP mirrored output, and planted negatives for
unknown options, invalid graph shape, unsupported nodes, ambiguous nodes,
malformed JSON, graph limits, non-finite input and non-finite samples.

Exact artifacts are recorded in
`G-7-residual-qualification-receipt-20261107.json`:

- source and `origin/main`: `a5ebbeca6139db88265ea93997d70927d3f60aa4`;
- current CLI SHA-256:
  `e924722fd305677a260f381d28d363527e45b0252d79d1f0b130c9b8f5e0e7f2`;
- current MCP SHA-256:
  `c59a255b8ba74cf013acd83a2bdf1991bfafb856dcdc087276aa8bd3392f6c86`;
- external consumer SHA-256:
  `0eb7750c389b7ef153c10edfc995a465acc823def2f2eb62043e26f76f1f8f16`;
- installed SDK provenance SHA-256:
  `4335a1fb583b56590a34253bcbe6564189942b9f59ef27cbc46429f43012cb4f`.

## Boundary and blocker

This is **not** an exact-head installed-SDK qualification. The current source
build's install step was attempted after the required CLI/MCP targets were
built, but the generated install graph stopped at a missing unrelated artifact:
`build/tools/audio/pulp-au-instrument-probe`. No current-head SDK was therefore
published or consumed, and this receipt does not claim installed-SDK parity,
control/event edge probes, realtime qualification, or full G-7 completion.

The next safe action is to complete the governed build/install artifact set (or
use a durable current-head SDK receipt), then rerun the same external consumer
against that exact SDK. The existing control/event probe PR remains a separate
implementation lane and must satisfy its review/CI gates before its evidence
can be promoted here.
