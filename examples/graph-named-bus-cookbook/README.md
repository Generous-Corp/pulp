# Named bus layout cookbook

This installed-SDK consumer demonstrates the public format-layer bus contract
alongside G-11's existing graph stereo placement recipe. It declares a mono
`Source` input, stereo `Front` main output, and optional stereo `Rear` aux
output. The host-side harness selects either `Front only` or `Front and Rear`
by checking the processor's declared layout, then supplies named
`ProcessBuffers` directly. The processor finds buses by name and renders one
impulse to fixed channel weights.

Configure this directory with `-DCMAKE_PREFIX_PATH=<installed Pulp SDK>`, build
the executable, and run `ctest` in its build directory. The positive oracle
checks the selected layout's bus names, roles, widths, every output sample,
and silence on the inactive rear bus. A planted swap of the two output bus
names must fail that same audio oracle, and an undeclared layout must be
rejected before processing.

## The same processor inside a SignalGraph

The recipe also runs `NamedBusPlacement` as a node of a real `SignalGraph`
(`add_input_node(1)` -> `add_processor_node` -> `add_output_node(2)`, connected
by the node's port handles `audio` and `out1`) and renders it with
`OfflineSignalGraphHost`. The `ProcessBuffers` harness stays the reference
oracle: the graph's two output channels must equal the harness `Front` bus
sample for sample, compared with `==`, not a tolerance.

What the graph does and does not carry:

- A Processor node exposes only its **main** input and output buses, one graph
  port per channel. The aux `Rear` bus has no graph port, so the graph proves
  the `Front` bus, not `Rear`.
- The routed node hands the processor buses named `main`, not the descriptor's
  `Source` and `Front`. The processor therefore falls back to `inputs.main()`
  and `outputs.main()` when a declared name is absent. A variant that looks up
  names only renders silence in the graph; the test pins that result.
- The planted negatives are checked in the graph path. A harness fed swapped
  output names, and a graph whose two main-bus channels are crossed, must each
  differ from the correct graph output (`--planted-graph-swap`, marker
  `named-bus graph planted defect detected: swapped-names`).

### Baked-artifact boundary

The graph is then passed to the existing public `bake()`. It refuses, fail
closed: no processor is returned, `reason` is
`LowerRejectReason::HostedPluginNotSelfContained`, and `offending_node` is the
Processor node. A Processor node is runtime-only and is not frozen into a
baked artifact, so named-bus layouts are not preserved by baking; the cookbook
asserts the refusal rather than a baked output. No bake API was added.

This proves named format buses and layout selection, and main-bus parity of the
graph path, using public installed-SDK APIs. It does not claim azimuth, HRTF,
realtime performance, a baked bus layout, tuning, or live sampling.

## Notation

`graph.pat` spells this fixture's graph in SignalGraph patch notation as far as
the notation profile allows. It cannot be accepted: the fixture's nodes are
application code or constructs outside the default catalog, so the real
validator refuses it, and that refusal is the recorded result:

```sh
pulp graph validate graph.pat --json
# exit 1, "code": "UnsupportedContract", "rule": "unregistered type/version '<id>'"
```

The `graph-named-bus-cookbook-notation-refusal` ctest runs the validator and pins the exit code,
code, node and type. See docs/guides/graph-patch-notation.md for the profile.
