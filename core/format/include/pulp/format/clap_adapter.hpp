#pragma once

// CLAP Adapter for Pulp
// Implements the CLAP plugin entry point wrapping pulp::format::Processor
// Built from CLAP specification headers (MIT license)

#include <pulp/format/processor.hpp>
#include <pulp/format/ara.hpp>
#include <pulp/format/host_quirks.hpp>
#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/events/plugin_main_thread.hpp>
#include <pulp/state/parameter_event_queue.hpp>
#include <pulp/state/modulation_lane.hpp>
#include <pulp/state/preset_manager.hpp>
#include <pulp/signal/delay_line.hpp>
#include <clap/clap.h>
#include <array>
#include <vector>

// View includes only when building GUI-capable CLAP targets.
//
// Important: PulpClapPlugin is shared across clap_entry.hpp (compiled into the
// plugin module) and clap_adapter.cpp (compiled into pulp-format). Both
// translation units must see the same PULP_CLAP_GUI value or the instance
// layout diverges and lifecycle callbacks write through the wrong offsets.
#if defined(PULP_CLAP_GUI) && PULP_CLAP_GUI
#include <pulp/format/view_bridge.hpp>
#include <pulp/view/plugin_view_host.hpp>
#endif

namespace pulp::format::clap_adapter {

static constexpr int kMaxChannels = 8;
// Upper bound on output buses routed to the Processor in one block. Index 0 is
// the main output; indices 1..kMaxOutputBuses-1 are secondary (aux) outputs for
// multi-out instruments (drum machines, multitimbral, stem renderers). Host
// output buses beyond this cap are zero-filled, never routed — same safe
// fallback as before, just with a higher ceiling than one.
static constexpr int kMaxOutputBuses = 8;
static constexpr state::ModulationSourceId kClapHostModulationSourceId = 1;

// CLAP plugin instance — wraps a Pulp Processor
struct PulpClapPlugin {
    clap_plugin_t plugin;
    // The store is declared before the Processor so it is destroyed after it.
    // `Processor::state()` dereferences a pointer to this store, and a Processor
    // may read it from its destructor or from a worker thread that destructor is
    // about to join. Reversing these two lines hands that thread a freed store.
    state::StateStore store;
    std::unique_ptr<Processor> processor;
    ProcessorFactory factory;

    // Host accommodations, resolved once in clap_init() via the runtime
    // policy. Adapters consult these instead of hardcoding workarounds.
    HostQuirks host_quirks{};

    // Cached ParamID of the "Bypass" parameter, plugin-declared or synthesized
    // via synthesize_bypass_parameter. 0 when none; clap_process() then never
    // short-circuits to pass-through.
    state::ParamID bypass_param_id = 0;

    // Per-output-channel dry delay used by the bypass pass-through. The host
    // compensates the plugin path by its reported latency, so the bypassed
    // dry signal must be delayed by the same amount to stay sample-aligned
    // with the host's plugin-delay-compensation. Each line's storage is
    // allocated in clap_activate() (off the audio thread). Unused when the
    // reported latency is 0, preserving the zero-copy pass-through.
    std::array<signal::DelayLine, kMaxChannels> bypass_dry_delay{};
    int bypass_delay_samples = 0;

    // Stored at create_plugin() time so the adapter can publish
    // latency / tail change notifications back to the
    // host. `clap_on_main_thread()` consumes the processor's pending
    // flags and calls `clap_host_latency->changed()` /
    // `clap_host_tail->changed()` — never from process() itself.
    const clap_host_t* host = nullptr;

    // Audio working state
    double sample_rate = 48000.0;
    int max_buffer_size = 512;

    // Pre-allocated buffers — no heap allocation on audio thread
    float* output_ptrs[kMaxChannels] = {};
    const float* input_ptrs[kMaxChannels] = {};
    double* output64_ptrs[kMaxChannels] = {};
    const double* input64_ptrs[kMaxChannels] = {};
    std::array<std::vector<float>, kMaxChannels> f64_input_scratch{};
    std::array<std::vector<float>, kMaxChannels> f64_output_scratch{};
    // Per-bus channel-pointer storage for secondary (aux) output buses, indexed
    // [aux_bus_minus_1][channel]: row i backs the BufferView for the host output
    // bus at index i+1 (the main bus at index 0 uses output_ptrs above). Pre-
    // allocated so the multi-out routing path allocates nothing on the audio
    // thread.
    float* aux_output_ptrs[kMaxOutputBuses - 1][kMaxChannels] = {};
    double* aux_output64_ptrs[kMaxOutputBuses - 1][kMaxChannels] = {};
    std::array<std::array<std::vector<float>, kMaxChannels>, kMaxOutputBuses - 1>
        f64_aux_output_scratch{};
    // Descriptor-declared channel count per secondary output bus, cached in
    // clap_activate() (off the audio thread) so the process path can report a
    // bus's declared layout without calling descriptor() on the audio thread.
    // declared_aux_channels[i] is the declared channel count for the host
    // output bus at index i+1; 0 when the descriptor declares no such bus.
    int declared_aux_channels[kMaxOutputBuses - 1] = {};
    // Second input bus routed to Processor::set_sidechain(). Up to
    // kMaxChannels. Additional input buses beyond index 1 are currently
    // ignored; the Processor API is single-sidechain today.
    const float* sidechain_ptrs[kMaxChannels] = {};
    const double* sidechain64_ptrs[kMaxChannels] = {};
    std::array<std::vector<float>, kMaxChannels> f64_sidechain_scratch{};
    bool native_f64_enabled = false;

    // Parameter snapshot for detecting plugin-side changes during process
    std::vector<float> param_snapshot;
    state::ParameterEventQueue param_events;
    // Sample-accurate parameter OUTPUT: the processor pushes offset-tagged events
    // via push_output_param_event(); we merge them (by ascending sample offset)
    // with the plugin's MIDI-out shorts and sysex into out_events, which CLAP
    // requires to be globally time-ordered. output_param_has_event is the
    // skip-set that keeps the offset-0 snapshot fallback from double-reporting a
    // param that already emitted explicit events. Both are sized off the audio
    // thread at block start so the drain stays allocation-free.
    state::ParameterEventQueue output_param_events;
    std::vector<std::uint8_t> output_param_has_event;

    // Reused per-block MIDI buffers. Reserved and capacity-limited during
    // activate() so capacity survives warmup and processors can append
    // outbound MIDI while the process no-allocation guard is active.
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;

    // MPE sidecar — populated from midi_in before each process() call when
    // the Processor declares MPE in its effective PluginDescriptor capabilities.
    // Reserved and capacity-limited during activate(); one MIDI event can fan
    // out to many MPE callbacks.
    midi::MpeVoiceTracker mpe_tracker;
    midi::MpeBuffer mpe_buffer;
    int32_t mpe_current_sample_offset = 0;
    bool mpe_enabled = false;

    // UMP sidecar — populated by converting midi_in to MIDI 2.0 UMP packets
    // when the Processor declares UMP in its effective PluginDescriptor
    // capabilities. Native CLAP_EVENT_MIDI2 packets also append directly.
    // Reserved and capacity-limited during activate().
    midi::UmpBuffer ump_buffer;
    bool ump_enabled = false;

    // Preset management (optional — set by plugins that provide presets)
    std::unique_ptr<state::PresetManager> preset_manager;

    // ARA document controller (optional; Processor opts in by overriding
    // create_ara_document_controller()).
    // Lives for the plugin's lifetime once created; surfaced through
    // get_extension(kClapAraFactoryExtension).
    std::unique_ptr<AraDocumentController> ara_controller;

    // Editor state (created on GUI create, destroyed on GUI destroy).
    // `bridge` owns the view tree and dispatches Processor lifecycle
    // callbacks (on_view_opened/closed/resized). editor_host is the
    // platform-native window surface that hosts bridge->view().
#if defined(PULP_CLAP_GUI) && PULP_CLAP_GUI
    std::unique_ptr<ViewBridge> bridge;
    std::unique_ptr<view::PluginViewHost> editor_host;
#endif
    bool editor_visible = false;

    // Previous-block transport snapshot used to derive the `tempo_changed` /
    // `time_sig_changed` / `transport_changed` flags on `ProcessContext`.
    // Default-constructed (no previous block) so the first process() call after
    // activation reports no changes.
    detail::PlayheadSnapshot playhead_prev{};

    // Process-wide MainThreadDispatcher backend token. Acquired in clap_init()
    // so adapter callsites (e.g. host-callback dispatches posted via
    // `clap_host->request_callback()`) and any view-side code can use
    // `pulp::events::MainThreadDispatcher::call_async` to marshal work onto
    // the DAW's main thread. Released in clap_destroy().
    pulp::events::MainThreadDispatcher::Token main_thread_token = 0;
};

// CLAP entry point and factory
const clap_plugin_entry_t* get_clap_entry();

// Create a CLAP plugin descriptor from a Pulp PluginDescriptor
clap_plugin_descriptor_t make_clap_descriptor(const PluginDescriptor& desc);

// Plugin lifecycle callbacks
bool clap_init(const clap_plugin_t* plugin);
void clap_destroy(const clap_plugin_t* plugin);
bool clap_activate(const clap_plugin_t* plugin, double sr, uint32_t min_frames, uint32_t max_frames);
void clap_deactivate(const clap_plugin_t* plugin);
bool clap_start_processing(const clap_plugin_t* plugin);
void clap_stop_processing(const clap_plugin_t* plugin);
void clap_reset(const clap_plugin_t* plugin);
clap_process_status clap_process(const clap_plugin_t* plugin, const clap_process_t* process);
bool clap_param_modulation_lane(const PulpClapPlugin& self,
                                const clap_event_param_mod_t& event,
                                state::ModulationLane& lane);
const void* clap_get_extension(const clap_plugin_t* plugin, const char* id);
void clap_on_main_thread(const clap_plugin_t* plugin);

} // namespace pulp::format::clap_adapter
