# G-5 qualification follow-up — 2026-11-10

Status: **PARTIAL / INSTALLED-CONSUMER BLOCKED**

This follow-up starts from exact `origin/main` head
`ebded6b0a98e7353a13c4e74d1e9bc138dd6b342`, after merged PR #12002
(`fad3859f5cf2d9456980641eeeff30f344086b2b`). It does not add a runtime or
public API seam. The smallest remaining qualification slice is a clean,
exact-head installed-SDK consumer that exercises the graph-authored
`SignalGraph` host-ingress path.

## Evidence completed

The governed Release target `pulp-test-g5-host-event-consumer` built from this
head and CTest passed `g5-host-event-public-consumer` (`1/1`, 0.36 s). The
consumer source covers host MIDI/MPE projection, graph-authored producer and
consumer delivery, SignalGraph ingress, and partition schedules
`1,32,64,127,129,256`, with stale/late/sequence/capacity, missing-edge,
one-sample, reversed-tie, and payload-mismatch negatives. The merged #12002
callback fixture separately records 65,990 Release assertions, callback
allocation/lock/I/O planted controls, and reset mutation proof; its realtime
gate remains open.

## Remaining blocker and next action

The exact-head SDK installation could not be completed on this lane: the
governed target build is available, but the complete `install` target requires
the full 4,391-target install graph and was stopped after 239 targets to avoid
holding the shared builder. The checked-in installed-consumer receipt remains
from an older head and explicitly does not claim `SignalGraph` wiring. Do not
promote it to current-head evidence.

Next safe action: run the full governed Release install on a dedicated builder,
configure `examples/g5-host-event-consumer` against that prefix, and retain
positive, private-header, wrong-output, and mutation receipts. Until then,
installed-consumer and cross-platform/hard-realtime claims remain **NOT
CHECKED**, while the in-tree Release consumer remains **PASSED**.
