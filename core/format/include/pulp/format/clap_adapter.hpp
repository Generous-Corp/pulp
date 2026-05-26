#pragma once

// CLAP Adapter for Pulp
// Implements the CLAP plugin entry point wrapping pulp::format::Processor
// Built from CLAP specification headers (MIT license)

#include <pulp/format/processor.hpp>
#include <pulp/format/ara.hpp>
#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/state/parameter_event_queue.hpp>
#include <pulp/state/preset_manager.hpp>
#include <clap/clap.h>

// View includes only when building plugin targets (not the shared format lib)
#ifdef PULP_CLAP_GUI
#include <pulp/format/view_bridge.hpp>
#include <pulp/view/plugin_view_host.hpp>
#endif

namespace pulp::format::clap_adapter {

static constexpr int kMaxChannels = 8;

// CLAP plugin instance — wraps a Pulp Processor
struct PulpClapPlugin {
    clap_plugin_t plugin;
    std::unique_ptr<Processor> processor;
    state::StateStore store;
    ProcessorFactory factory;

    // Stored at create_plugin() time so the adapter can publish
    // latency / tail change notifications (item 3.11) back to the
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
    // Second input bus routed to Processor::set_sidechain() (workstream 01
    // slice 1.1). Up to kMaxChannels. Additional input buses beyond index 1
    // are currently ignored — the Processor API is single-sidechain today.
    const float* sidechain_ptrs[kMaxChannels] = {};

    // Parameter snapshot for detecting plugin-side changes during process
    std::vector<float> param_snapshot;
    state::ParameterEventQueue param_events;

    // MPE sidecar — populated from midi_in before each process() call when
    // the Processor declares MPE in its effective PluginDescriptor capabilities.
    midi::MpeVoiceTracker mpe_tracker;
    midi::MpeBuffer mpe_buffer;
    int32_t mpe_current_sample_offset = 0;
    bool mpe_enabled = false;

    // UMP sidecar — populated by converting midi_in to MIDI 2.0 UMP packets
    // when the Processor declares UMP in its effective PluginDescriptor
    // capabilities. Native CLAP_EVENT_MIDI2 packets also append directly.
    midi::UmpBuffer ump_buffer;
    bool ump_enabled = false;

    // Preset management (optional — set by plugins that provide presets)
    std::unique_ptr<state::PresetManager> preset_manager;

    // ARA document controller (optional — Processor opts in by overriding
    // create_ara_document_controller(). Workstream 06 slice 6.5).
    // Lives for the plugin's lifetime once created; surfaced through
    // get_extension(kClapAraFactoryExtension).
    std::unique_ptr<AraDocumentController> ara_controller;

    // Editor state (created on GUI create, destroyed on GUI destroy).
    // `bridge` owns the view tree and dispatches Processor lifecycle
    // callbacks (on_view_opened/closed/resized). editor_host is the
    // platform-native window surface that hosts bridge->view().
#ifdef PULP_CLAP_GUI
    std::unique_ptr<ViewBridge> bridge;
    std::unique_ptr<view::PluginViewHost> editor_host;
#endif
    bool editor_visible = false;

    // Item 1.3 — previous-block transport snapshot used to derive the
    // `tempo_changed` / `time_sig_changed` / `transport_changed` flags
    // on `ProcessContext`. Default-constructed (no previous block) so
    // the first process() call after activation reports no changes.
    detail::PlayheadSnapshot playhead_prev{};
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
const void* clap_get_extension(const clap_plugin_t* plugin, const char* id);
void clap_on_main_thread(const clap_plugin_t* plugin);

} // namespace pulp::format::clap_adapter
