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
(`add_input_node(1)` -> `add_processor_node` -> `add_output_node(4)`, connected
by the node's port handles `audio` and `out1` for `Front` and `Rear.0` and
`Rear.1` for `Rear`) and renders it with `OfflineSignalGraphHost`. The
`ProcessBuffers` harness stays the reference oracle: the graph's first two
output channels must equal the harness `Front` bus, and the last two the
harness `Rear` bus, sample for sample, compared with `==`, not a tolerance.

What the graph does and does not carry:

- A Processor node exposes its main input bus and every declared output bus,
  one graph port per channel: the main output first (`audio`, `out1`), then
  each aux output bus as `<bus>.<channel>` (`Rear.0`, `Rear.1`). Aux input
  buses are not graph ports; they reach the processor through an input-bus
  provider.
- The routed node hands the processor its buses under the descriptor's
  declared names, `Source`, `Front` and `Rear`. A variant that looks buses up
  by name only, with no `main()` fallback, renders exactly what the harness
  renders; the test pins that result.
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

This proves named format buses and layout selection, and parity of the graph
path for both the main and the aux bus, using public installed-SDK APIs. It does not claim azimuth, HRTF,
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
