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

#include <lv2/core/lv2.h>
#include <lv2/urid/urid.h>

#include <cstdint>

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

    // URID feature resolution (workstream 01 slice 1.5). The LV2 host passes
    // an LV2_URID_Map feature in instantiate(); we cache the map function
    // plus the URIDs we need at runtime so inner-loop code does not call
    // map() on hot paths. All fields are 0 when the feature is absent —
    // instantiate() returns nullptr in that case, so real plugin code
    // always sees non-zero values here.
    LV2_URID_Map* urid_map = nullptr;
    LV2_URID urid_midi_event = 0;      // LV2_MIDI__MidiEvent
    LV2_URID urid_atom_sequence = 0;   // LV2_ATOM__Sequence
    LV2_URID urid_atom_chunk = 0;      // LV2_ATOM__Chunk

    // Workstream 01 #241: atom-port MIDI. When the plug-in declares
    // accepts_midi, the host connects an LV2_Atom_Sequence buffer to
    // the port after the control ports. run() iterates it, extracts
    // MIDI events whose atom `type` field is urid_midi_event, and
    // feeds them to the Processor.
    bool accepts_midi = false;
    void* midi_in_atom = nullptr;

    // #491: parallel output-atom port for plugins that emit MIDI. The
    // TTL manifest already declared this port when produces_midi was
    // set, but run() never serialized the Processor's midi_out buffer
    // into it — silently dropping every outgoing event. The host pre-
    // sizes the buffer via lv2:minimumSize and signals capacity in the
    // atom.size field on entry to run(); the plugin overwrites the
    // sequence using the lv2_atom_sequence_clear + append_event helpers.
    bool produces_midi = false;
    void* midi_out_atom = nullptr;
};

/// Resolve LV2_URID_Map from a features array.
/// Returns nullptr if the feature is absent. Exposed for unit testing.
LV2_URID_Map* find_urid_map(const LV2_Feature* const* features);

// Generate an LV2 TTL manifest string for a plugin
// This creates the plugin.ttl content with port definitions
std::string generate_plugin_ttl(const PluginDescriptor& desc,
                                 const state::StateStore& store,
                                 const std::string& uri);

// Generate the manifest.ttl that points to the plugin binary
std::string generate_manifest_ttl(const std::string& plugin_uri,
                                   const std::string& binary_name);

} // namespace pulp::format::lv2_adapter
