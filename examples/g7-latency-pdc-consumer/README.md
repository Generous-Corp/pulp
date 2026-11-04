# G-7 latency and PDC consumer

This installed-SDK consumer proves the existing public latency surface on a
diamond graph: a custom node declares four samples of intrinsic latency, the
prepared graph reports four samples of total latency, and the node-level query
reports the same value. The unknown-node query is a planted negative and must
return zero. This fixture deliberately does not claim a general latency map,
CLI/MCP serialization, or realtime qualification; those remain later G-7 work.

Build this directory against an installed Pulp SDK with CMake, then run its
single CTest. The output is a deterministic receipt suitable for the roadmap
ledger.
