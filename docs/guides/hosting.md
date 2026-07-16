# Hosting Plugins in Pulp

> **Looking to analyze or compare a plugin?** Use the
> [Plugin Interrogation guide](plugin-interrogation.md) for the ready-to-use
> CLI and MCP workflow. This guide covers embedding the C++ hosting APIs.

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

AudioComponent enumeration may return an Audio Unit descriptor without a bundle
path, so the demo can select it by the component identity printed by `--list`:

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

## Showing a hosted plugin's editor

`EditorAttachment` embeds a hosted plugin's own GUI in a `WindowHost`, so you
can show the vendor's interface instead of rebuilding it from the parameter
list. It is RAII: the attachment owns the editor and detaches and destroys it in
the right order.

```cpp
#include <pulp/view/hosted_editor_attachment.hpp>

auto attachment = pulp::view::EditorAttachment::create(slot.get(), window);
if (!attachment) {
    // No editor for this format/platform, or the plugin refused. Fall back to
    // your own parameter UI — this is a normal answer, not an error.
}
```

Available for CLAP on macOS today; see Limits below.

Two things to design around:

- **A native child composites ABOVE Pulp's whole GPU layer.** The window server
  draws it over the Skia surface, so you cannot paint Pulp chrome on top of an
  embedded editor. Use `set_native_child_view_clip` (which `NativeViewHost` does
  for you) to keep a child inside a scroll viewport.
- **Editor calls are main-thread only.** Create, resize, and destroy on the main
  thread; never from `process()`. Destroy the attachment before dropping the
  slot — the CLAP slot logs an error and force-tears-down if you don't, but that
  is a diagnostic for a contract violation, not a supported path.

Resize negotiates in both directions. A plugin asking to resize itself reaches
the handler you install; refusing is legal and the plugin must cope:

```cpp
slot->set_editor_resize_request_handler([&](uint32_t w, uint32_t h) {
    return attachment->set_bounds(0.0f, 0.0f, float(w), float(h));
});

// Host-initiated: the plugin may snap to its own constraints, so read back.
uint32_t w = 900, h = 700;
if (slot->set_hosted_editor_size(w, h)) {
    attachment->set_bounds(0.0f, 0.0f, float(w), float(h));  // w/h are the accepted size
}
```

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
- Hosted editor embedding is wired for CLAP on macOS. VST3, AU, and LV2 slots
  still report no editor, and Windows and Linux have no desktop `WindowHost`
  implementing the native-child seam, so nothing can be parented there yet.
  Where an editor is unavailable, `has_editor()` is false and
  `EditorAttachment::create` returns nullptr — branch on that rather than
  assuming a view. A missing editor never blocks metadata, parameter, state,
  MIDI, or audio work; it only means the vendor's own UI cannot be shown.
