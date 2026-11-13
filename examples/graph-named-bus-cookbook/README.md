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

This proves named format buses and layout selection using public installed-SDK
APIs. It does not assert that SignalGraph exposes named host buses: the graph
currently maps processor nodes to main input/output channel counts. It also
does not claim azimuth, HRTF, realtime performance, baked parity, tuning, or
live sampling.

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
