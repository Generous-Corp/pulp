# Hosting Plugins in Pulp

Pulp can both *be* a plugin and *host* plugins. The hosting APIs live in
`pulp::host` and let you load VST3 / AU / CLAP / LV2 binaries, wire them
into a DAG, and process audio through the chain.

Status today: CLAP loads and processes audio; VST3 / AU / LV2 loaders are
stubbed and log a warning. The signal-graph topology (DAG + topological
sort) is implemented; execution along the topology is in progress.

## Quick start

```cpp
#include <pulp/host/plugin_slot.hpp>

pulp::host::PluginInfo info;
info.name   = "MyGain";
info.path   = "/path/to/MyGain.clap";
info.format = pulp::host::PluginFormat::CLAP;

auto slot = pulp::host::PluginSlot::load(info);
if (!slot) {
    // Loader not available for this format yet, or the bundle failed to open.
    return 1;
}
slot->prepare(48000.0, 512);
slot->process(out_buffer, in_buffer, midi_in, midi_out, 512);
slot->release();
```

`PluginSlot::load()` dispatches by `info.format`. Each format backend lives
in its own translation unit (`plugin_slot_clap.cpp`, …) and is compiled in
only when the matching SDK is available. The symbol a backend exposes is a
plain function — `load_clap_plugin(info)`, `load_vst3_plugin(info)`, … —
that the dispatcher forwards to. No dynamic registry; the dispatcher knows
the formats at compile time.

## Scanning

`PluginScanner` walks the standard plug-in directories for each format and
returns `PluginInfo` descriptors.

```cpp
auto scanner = pulp::host::PluginScanner{};
for (auto& info : scanner.scan(pulp::host::PluginFormat::CLAP)) {
    // Each info is ready to pass to PluginSlot::load().
}
```

Format-specific scanners (e.g. `scanner_clap.cpp`) read bundle metadata so
name / vendor / version / unique id come back populated.

## Signal graph

`SignalGraph` is a DAG of nodes: `InputNode`, `OutputNode`, `GainNode`,
`MidiInputNode`, `MidiOutputNode`, and `PluginNode`. You add nodes and
connect output ports to input ports. The graph runs topological sort to
produce a deterministic processing order.

```cpp
pulp::host::SignalGraph graph;
auto in    = graph.add_input_node(2, "in");
auto plug  = graph.add_plugin_node(std::move(slot), "gain");
auto out   = graph.add_output_node(2, "out");
graph.connect(in,   0, plug, 0);
graph.connect(plug, 0, out,  0);
```

Execution walks `graph.processing_order()` each block and invokes
`PluginSlot::process()` on each plugin node.

## Delay compensation (PDC)

`PluginSlot::latency_samples()` reports per-node latency. The graph sums
latencies along each path and inserts delay lines on the shorter paths so
signals stay phase-aligned where they recombine. Feedback loops need an
explicit delay (there's no acausal solver).

## CLI

Two commands surface hosting in the CLI:

```
pulp scan [--format clap|vst3|au|lv2]   # list discoverable plugins
pulp host <path>                        # load and briefly run a plugin
```

These exist to smoke-test the loaders outside a full DAW context.

## Limits

- CLAP events (note / param changes) are stubbed at empty streams — the
  plugin still processes, but parameter automation isn't routed yet.
- Only one factory descriptor per `.clap` is selected (first one, or one
  matching `info.unique_id`).
- Editor view creation (`create_editor_view()`) returns `nullptr`; hosting
  UIs ships after ViewBridge lands.
