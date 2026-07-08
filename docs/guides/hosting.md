# Hosting Plugins in Pulp

Pulp can both *be* a plugin and *host* plugins. The hosting APIs live in
`pulp::host` and let you load VST3 / AU / CLAP / LV2 binaries, wire them
into a DAG, and process audio through the chain.

Current scope: CLAP, VST3, AU, and LV2 have real `PluginSlot` loaders
when the matching SDK or platform support is compiled in. Each loader can
open a bundle, prepare it, and process audio through the common host
interface. Feature depth still varies by format: parameter, MIDI, state,
editor, and extension support are not identical across loaders.
`SignalGraph` topology, block processing, automation routing, and delay
compensation are implemented over the same `PluginSlot` interface.

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

## Swapping a hosted plugin live

Because the graph owns each hosted plugin behind a node, you can **replace
the plugin sitting in a node while audio keeps playing** — re-instantiate
the same plugin to reset it or feed it different state, or fall back to
substituting a different one. When the change can be made seamlessly the
audio stream never drops out; otherwise the graph re-prepares with a brief
silence. Opt a node in with `set_node_live_swap_policy()`, register the
plugins you're willing to swap in with `register_scanned_plugin()`, then
stage and commit the change inside a `begin_swap_edit()` /
`prepare_swap()` transaction. No new code is loaded — the swap only
rearranges plugins already installed and scanned on the machine — so it
carries no signing or trust surface. See
[Live plugin swap](../reference/signal-graph.md#live-plugin-swap) for the
full workflow and the fail-closed checks that protect the stream.

## Delay compensation (PDC)

`PluginSlot::latency_samples()` reports per-node latency. The graph sums
latencies along each path and inserts delay lines on the shorter paths so
signals stay phase-aligned where they recombine. Feedback loops need an
explicit delay (there's no acausal solver).

Automation has two rates with different latency behavior. Sparse
`connect_automation()` delivers two source-block-relative control points per
block and does not participate in PDC; processors may interpolate those points
with `ParamCursor` or subblock helpers. Dense
`connect_audio_rate_modulation()` is the PDC-aligned path for parameters that
must track a delayed audio branch sample by sample.

## CLI

Two commands surface hosting in the CLI:

```
pulp scan [--format clap|vst3|au|auv3|lv2] [--no-load]  # list discoverable plugins
pulp host <path>                                           # load and briefly run a plugin
```

These exist to smoke-test the loaders outside a full DAW context.

## Limits

- Feature coverage is format-specific. CLAP, VST3, and AU route parameter
  automation through their native event paths; LV2 routes host parameter
  events through block-rate control ports, so the last event in a block wins.
- LV2 atom sysex and other variable-length atom events are not routed yet;
  only short MIDI messages in the atom input sequence reach the processor.
- Only one factory descriptor per `.clap` is selected (first one, or one
  matching `info.unique_id`).
- Third-party hosted editor embedding is not wired in the current host
  loaders; the typed hosted-editor API exists, but CLAP / VST3 / AU / LV2
  slots still report no hosted editor.
