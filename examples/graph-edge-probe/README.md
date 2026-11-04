# Observe an audio edge through the installed SDK

Configure this directory as a standalone CMake project with `Pulp_DIR` pointing
to an installed SDK's `lib/cmake/Pulp`, then build and run CTest. The consumer
uses only published headers and `Pulp::host`; no source checkout include paths
or private test helpers are needed.

`pulp-graph-edge-probe [OUTPUT_DIRECTORY]` checks source-side peak/RMS against a
separate downstream audio oracle, unchanged audio, variable/partial blocks,
bounded overflow, attachment identity, and lifecycle/edge refusals. An optional
output directory receives the graph, deterministic input WAV and C++ observation
rows for exercising `pulp graph probe` and the matching MCP operation.

These offline checks do not qualify callback deadlines or other edge kinds.
