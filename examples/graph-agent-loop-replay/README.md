# A9 edit and replay evidence consumer

This standalone consumer exercises the existing public `Pulp::host` surface in
a separate fixture from `examples/graph-agent-loop`. It saves a stereo
Input → Gain → Gain → Output graph, reloads it with plugin loading disabled,
renders a fixed input, applies one gain edit, and reloads the saved baseline.

The receipt records two independent values:

* a topology-only semantic fingerprint, which stays equal across the edit and
  baseline replay; and
* the detached prepared-description `content_hash()`, which changes for the
  gain edit and returns to the baseline on replay. This is resolved-plan
  evidence for the admitted prepared I/O + Gain profile, not a whole-graph
  identity or A9 completion claim.

The two mutation fixtures plant the wrong-edit and wrong-replay expectations.
CTest marks both as expected failures and requires the named refusal rule.

Configure against either a source build's package config or an installed SDK:

```sh
cmake -S examples/graph-agent-loop-replay -B build/a9-edit-replay \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk
cmake --build build/a9-edit-replay
ctest --test-dir build/a9-edit-replay --output-on-failure
```

The fixture does not execute plugins, model code, network calls, CLI/MCP
transport, realtime callbacks, or arbitrary graph node families.
