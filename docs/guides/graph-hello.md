# Graph hello consumer

`examples/graph-hello` is a standalone consumer of the installed Pulp SDK. It
authors an input → gain → output `SignalGraph`, applies a generated-graph
budget, prepares it, and renders 32 frames through `OfflineSignalGraphHost`
without opening an audio device.

```sh
cmake -S examples/graph-hello -B build/graph-hello \
  -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk -DCMAKE_BUILD_TYPE=Release
cmake --build build/graph-hello
ctest --test-dir build/graph-hello --output-on-failure
./build/graph-hello/pulp-graph-hello
```

The positive control proves that every rendered sample is `0.5` and that the
offline host renders both blocks. A planted-negative control is the same
fixture with `set_node_gain(gain, 0.25f)`: it must fail the executable's exact
`0.5` oracle. Keep that mutation local while checking the control; it is not a
shipped source change.

## API-gap receipt (G1 ports)

Verified against the current public headers used by this consumer:

| Port | Status | Evidence / gap |
| --- | --- | --- |
| Graph authoring | available | `SignalGraph::add_input_node`, `add_gain_node`, `add_output_node`, `connect`, `set_node_gain` |
| Bounded generated graph validation | available | `set_limits` and `validate_generated_graph` |
| Deterministic offline render | available | `OfflineSignalGraphHost::prepare` and `render` |
| Installed SDK package | environment-dependent | The fixture requires a staged SDK exposing the `host` component and `pulp::host` target; this checkout does not itself prove a released package path. |
| External plugin loading | intentionally deferred | Today's fixture uses the built-in gain node. No stable, dependency-free installed-SDK plugin fixture was identified, so G1 plugin ports remain open. |

This receipt records the narrow consumer surface and keeps the unresolved
installed-package and external-plugin claims explicit.
