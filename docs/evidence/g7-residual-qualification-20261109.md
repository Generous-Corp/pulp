# G7 residual qualification evidence — 2026-11-09

State: **PARTIAL / HOLD**.

This document records bounded evidence from the public G7 graph probe APIs. It
contains no implementation change and does not modify the G7 roadmap/status
files. The evidence branch was fast-forwarded to current `origin/main` before
this document was committed.

## Provenance

- Evidence source head used for build/test: `2f6aa0d90421a24c9c092c1bc96ab2dd34ad8914`.
- Current `origin/main` at handoff/commit: `546051b66b44df141b7f0bec1217fa96e971022c`.
- Worktree: `codex/g7-qualification-20261109`, classified active as
  `g7-qualification-evidence`.
- Configure: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=ON -DPULP_BUILD_EXAMPLES=OFF`.
- Governed build targets: `pulp-test-graph-control-event-probe`,
  `pulp-test-group-sampler-graph-runtime`, `pulp-cpp`, `pulp-mcp`, then the
  install prerequisites `pulp-control-broker`, `pulp-au-instrument-probe`,
  `pulp-au-effect-ab`, `pulp-runtime-pins`, and `pulp-scan-worker`.
- All build invocations used `tools/ci/governed-build.sh cmake --build`.
- Final build environment: Darwin arm64 `Mac15,14`, 28 logical CPUs, 96 GiB;
  final observed load averages `39.17, 70.19, 76.51`.

## Passing positive controls

- CTest #131, `G7 control probes cover lifecycle refusals, dense rejection, and reset clocks`: **PASS**.
- CTest #132, `G7 event probes cover lifecycle, identity, reset detach, and publication`: **PASS**.
- Combined CTest result: **2/2 passed**; Catch2 output reports **49 assertions in 2 test cases**.
- CTest #883, `GraphRuntimeProbe keeps a bounded attached edge sample ring`: **PASS**; **12 assertions in 1 test case**.
- The source control/event consumer includes planted missing-publication and
  missing-event-publication switches. Those are compiled negative controls in
  the merged source. The installed-SDK wrapper was not promoted because its
  install failed closed at an optional artifact (below).

Binary SHA-256 values from the Release build:

- `pulp-cpp`: `d1da86b31aba6a54199238734aecb8461b991cbc4ad31866c28a7a8965231512`.
- `pulp-mcp`: `36106754c278e2cda56cfb565f57b9570819e5ffe137df0aabe675788209c99d`.
- `pulp-test-graph-control-event-probe`:
  `07819098aaa3d32f3c86e3e09afb2db3dabc00b58a9d0419309f99f27e7d606`.
- `pulp-test-group-sampler-graph-runtime`:
  `2d723d451a734601ce3621bd4d3439fd50f544e0acfc34427f3245b14a991c02`.

Raw receipt artifacts were retained in the evidence worktree under
`build/g7-evidence/`:

- `handoff.md` SHA-256:
  `bac7592be054f2306389b65bf3e8a532daef1493af3f8e9d8981500ac82bcb79`.
- `control-event-ctest.txt` SHA-256:
  `284374732bb750124f0c548f6ae3c90a1930f21e962850cc5090d7c93c9a9390`.
- `ring-ctest.txt` SHA-256:
  `39c795444d6d9aafde924beb3445a20dda92604335635e8d6c521cf1cde177b2`.

## Qualification boundaries

| Dimension | Disposition | Evidence boundary |
|---|---|---|
| Callback deadline / WCET | **NOT CHECKED / HOLD** | No callback deadline receipt, audio-device scheduler, or WCET measurement was run. Host load was heavily contended. |
| Latency / load | **PARTIAL** | Bounded public probe ring and control-thread lifecycle tests pass. No latency/load qualification receipt was produced. |
| Paced realtime | **NOT CHECKED / HOLD** | Existing public graph probes are diagnostic/control-thread or offline surfaces; no hard realtime claim is made. |
| Parallel execution | **NOT CHECKED / HOLD** | Public probe attachment rejects parallel/legacy/anticipative execution; no positive parallel qualification run was performed. |
| Long-run loss | **NOT CHECKED / HOLD** | No sustained run with a loss denominator and terminal disposition trace was run. |
| Host coverage | **NOT CHECKED / HOLD** | Evidence is from one local M3 arm64 process; no DAW or plugin-host lane was run. |
| Provider coverage | **N/A for CPU graph probes; HOLD for provider-backed claims** | These graph probes use the CPU/public host API. No authenticated external provider receipt was available or inferred. |
| Cross-platform | **NOT CHECKED / HOLD** | No Windows/Linux or second-host run was performed. |
| Installed SDK parity | **HOLD** | The CTest wrapper repeatedly failed closed while installing optional SDK artifacts; after building broker, AU probes, runtime pins, and scan worker, install stopped at missing `tools/import-design/pulp-import-design`. |

The initial governed multi-target build was denied an interactive lease for 90
seconds (`capacity_exceeded`, used cores 19/26, non-gate availability 0) and
continued at the wrapper's bounded `-j2`. Later small targets acquired leases.
`tart list` showed standard build images stopped and one Vellum gate VM running;
that inventory does not establish provider or realtime availability.

No PR #11733 was revived, no duplicate implementation was created, and no PR
was armed, enqueued, approved, or merged by this lane.
