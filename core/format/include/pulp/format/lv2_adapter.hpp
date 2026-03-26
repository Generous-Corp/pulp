#pragma once

// LV2 Adapter for Pulp
// Implements the LV2 plugin interface wrapping pulp::format::Processor
// Built from LV2 specification headers (ISC license)
//
// LV2 uses a C API with:
// - URI-based plugin identification
// - Port-based I/O (audio ports + control ports for parameters)
// - TTL manifest files for discovery

#include <pulp/format/processor.hpp>

namespace pulp::format::lv2_adapter {

static constexpr int kMaxChannels = 8;

// LV2 plugin instance — wraps a Pulp Processor
struct PulpLv2Instance {
    std::unique_ptr<Processor> processor;
    state::StateStore store;
    ProcessorFactory factory;

    // Audio working state
    double sample_rate = 48000.0;

    // Port connections (set by connect_port)
    // Layout: [audio_in_0..N, audio_out_0..M, control_in_0..P, control_out_0..P]
    float* audio_in_ports[kMaxChannels] = {};
    float* audio_out_ports[kMaxChannels] = {};
    float** control_in_ports = nullptr;   // One per parameter
    float** control_out_ports = nullptr;  // One per parameter (for output)

    int num_audio_inputs = 0;
    int num_audio_outputs = 0;
    int num_params = 0;
    std::vector<state::ParamID> param_ids;  // Maps control port index → ParamID
};

// Generate an LV2 TTL manifest string for a plugin
// This creates the plugin.ttl content with port definitions
std::string generate_plugin_ttl(const PluginDescriptor& desc,
                                 const state::StateStore& store,
                                 const std::string& uri);

// Generate the manifest.ttl that points to the plugin binary
std::string generate_manifest_ttl(const std::string& plugin_uri,
                                   const std::string& binary_name);

} // namespace pulp::format::lv2_adapter
