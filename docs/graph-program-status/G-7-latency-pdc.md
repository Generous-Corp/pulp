# G-7 latency/PDC consumer fixture

This fixture is a bounded installed-SDK consumer for the existing public
latency queries. It constructs a diamond graph with a custom four-sample delay,
checks total latency and the delayed node's input latency, and checks the planted
unknown-node negative.
It does not implement a new runtime or claim a general latency/PDC map, CLI/MCP
serialization, realtime safety, or full G-7 completion.

State: PARTIAL; the fixture was validated against an SDK installed from this exact worktree head `38609bce981522faf0120970217b8818e4f56751` via governed `pulp build` followed by `cmake --install build --prefix sdk-exact`. The Release consumer configured, built, and passed CTest 1/1. Direct receipt: `g7-latency-pdc PASS total_latency_samples=4 delayed_node_input_latency_samples=0`. The unknown-node query is the planted negative. This proves the existing public latency queries at this exact head, not a general latency map, CLI/MCP surface, realtime qualification, or full G-7 completion.
