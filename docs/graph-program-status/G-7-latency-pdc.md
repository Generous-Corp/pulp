# G-7 latency/PDC consumer fixture

This fixture is a bounded installed-SDK consumer for the existing public
latency queries. It constructs a diamond graph with a custom four-sample delay,
checks total latency and the delayed node's input latency, and checks the planted
unknown-node negative.
It does not implement a new runtime or claim a general latency/PDC map, CLI/MCP
serialization, realtime safety, or full G-7 completion.

State: PARTIAL; the fixture was validated against an SDK installed from exact worktree head `5cba5488a8e68ac920eeef9acd275878ea5bfc01` at `/Volumes/Workshop/Code/g7-sdk-exact-5cba5488a8e-final`. Its `sdk-provenance.json` records that SHA with `source_git_dirty=false`; the separate consumer cache records `Pulp_DIR=/Volumes/Workshop/Code/g7-sdk-exact-5cba5488a8e-final/lib/cmake/Pulp` and the same `PULP_SDK_SOURCE_GIT_SHA`. The Release consumer configured, built, and passed CTest 1/1. Direct receipt: `g7-latency-pdc PASS total_latency_samples=4 delayed_node_input_latency_samples=0`. The unknown-node query is the planted negative. This proves the existing public latency queries at this exact head, not a general latency map, CLI/MCP surface, realtime qualification, or full G-7 completion.
