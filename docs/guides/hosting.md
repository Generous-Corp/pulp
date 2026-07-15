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
The VST3 and AU loaders are not stubs: both instantiate and process third-party
plug-ins in supported desktop builds.

For a ready-to-use, isolated CLI/MCP workflow that discovers parameters and
renders controlled A/B experiments, see [Interrogating and Comparing
Third-Party Plugins](plugin-interrogation.md).

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
pulp::host::ParameterEventQueue parameter_events;  // sample-accurate param changes; empty is fine
slot->process(out_buffer, in_buffer, midi_in, midi_out, parameter_events, 512);
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

For applications that scan arbitrary third-party installations, prefer
`IsolatedPluginScanner` (`pulp/host/isolated_scanner.hpp`). It runs the scan in
the `pulp-scan-worker` child process, so a crash, hang, or timeout comes back as
a structured `ScanResult` / `ScanStatus` instead of taking down the host. That
isolation covers discovery; a tool that performs deeper analysis by
instantiating and processing untrusted plug-ins should run that probe in a child
process too.

## Analyzer-style inspection

Once a scanner has returned a `PluginInfo`, the same format-neutral `PluginSlot`
API inspects an AU, VST3, CLAP, or LV2 plug-in through one code path:

```cpp
auto slot = pulp::host::PluginSlot::load(info);
if (!slot || !slot->prepare(48'000.0, 512)) {
    return 1;
}

std::cout << slot->info().name << "\n"
          << "parameters: " << slot->parameters().size() << "\n"
          << "latency: " << slot->latency_samples() << " samples\n"
          << "tail: " << slot->tail_samples() << " samples\n"
          << "state bytes: " << slot->save_state().size() << "\n";

slot->release();
```

The repository includes the runnable
[`plugin-host-demo`](../../examples/plugin-host-demo/) host/analyzer example. It
lists the installed plug-ins, prints metadata and the first parameters, runs a
synthetic audio block, and reports the output peak:

```bash
cmake --build build --target pulp-plugin-host-demo
./build/examples/plugin-host-demo/pulp-plugin-host-demo --list
./build/examples/plugin-host-demo/pulp-plugin-host-demo \
  --path "/Library/Audio/Plug-Ins/VST3/MyPlugin.vst3"
```

An Audio Unit has no bundle path, so select it by the component identity printed
by `--list`:

```bash
./build/examples/plugin-host-demo/pulp-plugin-host-demo --id TYPE:SUBT:MANU
```

This example loads the plug-in in-process and is meant for trusted local
testing. An unattended analyzer should put the load / prepare / process / state
probes behind a child-process timeout, as noted above.

## Headless Audio Unit initialization

Some licensed Audio Units finish initialization asynchronously using XPC,
timers, or dispatch-main callbacks. A GUI application naturally services those
events. An offline analyzer or renderer must do so explicitly on its process
main thread:

```cpp
#include <pulp/events/message_loop_integration.hpp>

#include <thread>

using namespace std::chrono_literals;

// After load/prepare, and before reading or writing parameters:
const auto deadline = std::chrono::steady_clock::now() + 500ms;
while (std::chrono::steady_clock::now() < deadline) {
    const auto result =
        pulp::events::MessageLoopIntegration::pump_main_loop_for(25ms);
    if (result == pulp::events::MainLoopPumpResult::Unsupported ||
        result == pulp::events::MainLoopPumpResult::WrongThread) {
        break;
    }
    if (result == pulp::events::MainLoopPumpResult::Finished ||
        result == pulp::events::MainLoopPumpResult::Stopped) {
        std::this_thread::sleep_for(1ms);
    }
}
```

This API services at most one bounded slice and must never be called from an
audio callback. It reports event-loop progress, not plug-in or license
readiness. Analysis tools should make their warm-up and post-parameter-write
settle periods configurable, apply parameter writes after warm-up, and continue
servicing bounded slices between offline blocks when the plug-in requires it.
For plug-ins that slew parameter changes, render and discard an appropriate
settle interval before capturing the measurement.

## Runnable example

[`examples/plugin-host-demo`](../../examples/plugin-host-demo/) scans installed
plug-ins, selects a descriptor, loads and prepares it, prints metadata and
parameters, then processes a synthetic audio block. Build the examples and run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_EXAMPLES=ON
cmake --build build --config Release --target pulp-plugin-host-demo -j4
build/examples/plugin-host-demo/pulp-plugin-host-demo --list
build/examples/plugin-host-demo/pulp-plugin-host-demo --id <plugin-id> --warmup-ms 500
```

`--warmup-ms` demonstrates the headless main-loop policy above. It is not a
universal value; choose and validate a duration for the plug-ins being analyzed.
The demo is itself a convenient probe executable, but a production tool should
launch equivalent deep probes as disposable child processes.

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
  slots still report no hosted editor. This does not prevent metadata,
  parameter, state, MIDI, or audio analysis; it means Pulp cannot currently
  create and display the vendor's native editor UI.
