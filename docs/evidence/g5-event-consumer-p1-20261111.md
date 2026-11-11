# G-5 P1 event consumer receipt — 2026-11-11

State: **PARTIAL; consumer controls added, current-head C++ build NOT CHECKED**.

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
- Source file SHA-256: `examples/g5-host-event-consumer/main.cpp` `1fea6b1260d1d3f0e34368e5e1afeca3fc158a3a1bc112129b89b63578527486`; included public headers `graph_event_contract.hpp` `1364609e77aee4e39fa3c1692e36c715ec9385856515dc3076d1de7ef0b0350a`, `signal_graph_runtime.hpp` `9527c8e535cf0a2f7eaa4e7eff0249784c8a02ec891a998a458422687e0d1748`, `trigger_graph.hpp` `f26b6613f55461f4f0d36fe4b2d027b45246b9220f18213b271d17831fbac68a`, and `process_context.hpp` `4ced24c83b665720d97d1a0a2f43a113e8b8f5afbacb82374aa70c0be4b50fb9`.
- Python probe status: `python3 -m unittest discover -s examples/g5-event-consumer -p 'test_*.py'` — **0 tests collected; exits 5 (`NO TESTS RAN`)**. This directory has no Python tests, so that command is not a validation oracle.
- Validated oracle: source-level inspection only. The current control set implies 368 runtime checks (including the lifecycle controls); no executable PASS receipt was produced.
- Current-head governed SDK configure/build: **NOT CHECKED**. Configure was
  stopped after a shared FetchContent clone (`vellum_gpu_runtime`) remained in
  progress; no current-head C++ result is claimed.
- A prior installed SDK prefix was attempted only as a stale negative control;
  it lacks current APIs and is not evidence about this slice.

The current-head consumer build and direct receipt must be rerun from a freshly
installed SDK before promoting this evidence beyond PARTIAL.
