# G-7 latency/PDC consumer fixture

This fixture is a bounded installed-SDK consumer for the prepared latency/PDC
snapshot API. It constructs a diamond graph with a custom four-sample delay,
checks deterministic per-node input/output latency, checks the delayed fan-in
branch (zero delay) and the dry fan-in branch (four samples of PDC), and checks
planted unknown-node and unknown-connection negatives. A copied snapshot stays
readable after a topology mutation while a fresh query fails closed until the
graph is prepared again.

It does not implement CLI/MCP serialization, realtime qualification, or full
G-7 completion. The API is control-thread introspection sourced from the
published prepared snapshot; it is not an audio callback surface.

State: PARTIAL pending exact-head installed-SDK evidence and review.
