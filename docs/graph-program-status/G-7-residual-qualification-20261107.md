# G-7 residual qualification — installed consumer and probe parity

Owner: g7-residual-qualification  
Started from `origin/main`: `3500e9da02756c0bef49ccaa3c93301ad66ec6be`
State: **PARTIAL — exact-head installed SDK evidence; qualification boundaries remain**

## Evidence

The merged current-main source was configured as Release and fully built with
the governed build wrapper. The installed SDK was produced from this exact
source head. The external `examples/graph-edge-probe` consumer was configured
against that SDK; its CTest passed `1/1` and the direct consumer run passed
`567` checks.

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

- source head: `eb23f84212600dec18912313af6c78846a146d15`;
- `origin/main` base: `3500e9da02756c0bef49ccaa3c93301ad66ec6be`;
- current CLI SHA-256:
  `f278dd5537e2150cd11e2d18adc83790866b777d953195280a94ed6dc49d6f20`;
- current MCP SHA-256:
  `41fbb3bb4894571311aa6628357098f534e5aa85e6b1371604eae21c85cff0a2`;
- external consumer SHA-256:
  `10a231ba1d0fd7300e3e57e23a8eab4d4062a610c577c4eca614235a986395a3`;
- installed SDK provenance SHA-256:
  `235e67842ad6b7ba19fd08f4f188ea8425ddda924342b481cdbc215255edce0c`.

## Boundary and blocker

The receipt proves exact-head installed-SDK parity for the covered audio-edge,
plan/latency, load, CLI/MCP, and planted-negative controls. It does not claim
control/event edge probes, realtime qualification, cross-platform coverage, or
full G-7 completion. The separate control/event probe implementation remains a
separate lane and must satisfy its review/CI gates before its evidence can be
promoted here.
