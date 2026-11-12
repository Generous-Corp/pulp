# G-5 P1 event consumer receipt — 2026-11-11

State: **PARTIAL; exact-head installed-SDK consumer PASS; broader runtime and platform claims remain NOT CHECKED**.

This evidence-only slice extends the existing public `g5-host-event-consumer`
fixture. It uses the existing `HostEventIngress` and
`GraphAuthoredEventLane` primitives and adds no runtime seam or scheduler.

## Scope and controls

- Host projection queue: fixed-capacity overflow, transactional output-capacity
  refusal, late publication, epoch reset, and stale-epoch refusal.
- Graph queue: queued look-ahead delivery, late and per-sample overflow
  refusals, one-block versus two-block partition equivalence, and retired-epoch
  refusal after reset.
- Boundaries retained: no host scheduling, plugin parity, hard-realtime,
  cross-platform, or universal scheduler claim.

## Validation

- Source parent: `7aa7d247e3fd188417c0ecce1338d8fa286f4ac6`; the corrected fixture is checked in on the repair head.
- Source file SHA-256: `examples/g5-host-event-consumer/main.cpp` `ef7a54f1e1e8137164b7088272881e9e9dec0d47d5c640cbcf6d14951282d6c6`; included public headers `graph_event_contract.hpp` `1364609e77aee4e39fa3c1692e36c715ec9385856515dc3076d1de7ef0b0350a`, `signal_graph_runtime.hpp` `9527c8e535cf0a2f7eaa4e7eff0249784c8a02ec891a998a458422687e0d1748`, `trigger_graph.hpp` `f26b6613f55461f4f0d36fe4b2d027b45246b9220f18213b271d17831fbac68a`, and `process_context.hpp` `4ced24c83b665720d97d1a0a2f43a113e8b8f5afbacb82374aa70c0be4b50fb9`.
- Python probe status: `python3 -m unittest discover -s examples/g5-event-consumer -p 'test_*.py'` — **0 tests collected; exits 5 (`NO TESTS RAN`)**. This directory has no Python tests, so that command is not a validation oracle.
- Validated oracle: exact-head installed-SDK Release consumer and CTest — `g5-host-event-contract PASS checks=378 host_mpe_events=3 graph_events=9 graph_ingress_events=5 partitions=1,32,64,127,256 planted_negative=5`.
- Installed SDK provenance: development prefix source revision
  `6f986351b05f9557ec994618d9e9ff66e1b03125`, which contains the exact fixture
  source; executable SHA-256 `356caa7a7dc4cf9818c2aacdffc3f1465580ab8d6d03a17bf974af5baa128a51`.
- Current-head governed SDK configure/build remains **NOT CHECKED**; the
  installed consumer is the bounded fixture qualification and does not claim
  plugin parity, hard realtime, cross-platform support, or a universal scheduler.

The exact-head consumer receipt is qualified for this bounded fixture; broader
runtime and platform claims remain PARTIAL/NOT CHECKED.
