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

- Source head: `42992f6d8488f5e58628253e80134b1aa717083d` (this receipt head; base `3633377fdd47c086850a22ef8d87130cd17f4e6d`).
- Frozen Python G-5 oracle: `python3 -m unittest discover -s examples/g5-event-consumer -p 'test_*.py'` — **4 passed**.
- Current-head governed SDK configure/build: **NOT CHECKED**. Configure was
  stopped after a shared FetchContent clone (`vellum_gpu_runtime`) remained in
  progress; no current-head C++ result is claimed.
- A prior installed SDK prefix was attempted only as a stale negative control;
  it lacks current APIs and is not evidence about this slice.

The current-head consumer build and direct receipt must be rerun from a freshly
installed SDK before promoting this evidence beyond PARTIAL.
