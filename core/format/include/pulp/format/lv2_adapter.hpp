#pragma once

// LV2 Adapter for Pulp
// Implements the LV2 plugin interface wrapping pulp::format::Processor
// Built from LV2 specification headers (ISC license)
//
// LV2 uses a C API with:
// - URI-based plugin identification
// - Port-based I/O (audio ports + control ports for parameters)
// - TTL manifest files for discovery

#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <lv2/core/lv2.h>
#include <lv2/options/options.h>
#include <lv2/urid/urid.h>

#include <cstdint>

namespace pulp::format::lv2_adapter {

// Per-bus channel ceiling. Independent of, but intentionally equal to,
// boundary::kBoundaryMaxChannels (adapter_boundary.hpp) — this lightweight LV2
// header deliberately does not include the boundary header, so the value is
// duplicated rather than aliased. If the shared ceiling changes, change this too.
static constexpr int kMaxChannels = 8;

// Block size assumed when the host supplies no `bufsz:maxBlockLength` option.
// A host that declares `bufsz:boundedBlockLength` always supplies the option;
// this is the floor for one that does not.
static constexpr int kDefaultMaxBlockLength = 4096;

// Byte capacity requested for each atom port via `lv2:minimumSize`.
//
// An LV2 host allocates the sequence buffer, so this is the plugin's only say
// in how many events fit. Sized for the adapter's worst-case block: the
// realtime MIDI capacity (see lv2_entry.hpp's kRealtimeMidiEventCapacity)
// times 24 bytes per event — a 16-byte LV2_Atom_Event header plus a 3-byte
// MIDI message padded to the sequence's 8-byte alignment — plus the sequence
// header, rounded up to a power of two. lv2_entry.hpp static_asserts the
// relationship so raising the event capacity cannot silently outgrow this.
static constexpr uint32_t kAtomPortMinimumSize = 32768;

// Property key URI for the single host-facing state blob stored through
// `state:interface`. One `atom:Chunk` carrying the same versioned envelope
// `plugin_state_io::serialize()` hands every other format.
inline constexpr const char* kStateBlobUri = "http://pulp.audio/ns/state#blob";

/// What a host-supplied LV2 port index addresses on a Pulp instance.
enum class Lv2PortKind {
    None, ///< Outside this instance's declared port range.
    AudioIn,
    AudioOut,
    Control,
    AtomIn,
    AtomOut,
    Latency,
};

/// The single definition of a Pulp LV2 plugin's port ordering.
///
/// LV2 names a port by a bare integer and nothing else: the manifest writes it
/// as `lv2:index N` and the host hands the same integer back to
/// `connect_port()`. `generate_plugin_ttl()` and `connect_port()` both derive
/// their indices from this type, so the manifest a host reads and the slot the
/// adapter wires cannot disagree — a disagreement delivers a `float*` where an
/// `LV2_Atom_Sequence` is read, which compiles, links and loads.
///
/// The order is: every audio input channel, every audio output channel, one
/// control port per parameter, the atom input port when the plugin accepts
/// MIDI, the atom output port when it produces MIDI, then the latency output
/// control port, always last. A host saves a session's port connections by
/// index, so inserting a port renumbers every port after it and reconnects an
/// existing session's ports to the wrong slots. Changing this order is a
/// compatibility event.
struct Lv2PortLayout {
    int num_audio_inputs = 0;
    int num_audio_outputs = 0;
    int num_params = 0;
    bool accepts_midi = false;
    bool produces_midi = false;

    [[nodiscard]] constexpr int audio_in_begin() const noexcept {
        return 0;
    }
    [[nodiscard]] constexpr int audio_out_begin() const noexcept {
        return num_audio_inputs;
    }
    [[nodiscard]] constexpr int control_begin() const noexcept {
        return audio_out_begin() + num_audio_outputs;
    }
    [[nodiscard]] constexpr int atom_in_begin() const noexcept {
        return control_begin() + num_params;
    }
    [[nodiscard]] constexpr int atom_out_begin() const noexcept {
        return atom_in_begin() + (accepts_midi ? 1 : 0);
    }
    [[nodiscard]] constexpr int latency_index() const noexcept {
        return atom_out_begin() + (produces_midi ? 1 : 0);
    }
    /// Total ports this plugin declares, latency port included.
    [[nodiscard]] constexpr int port_count() const noexcept {
        return latency_index() + 1;
    }

    /// Classify a port number. Every range is checked at both ends, so a
    /// negative or past-the-end index resolves to `None` rather than to a slot.
    [[nodiscard]] constexpr Lv2PortKind kind_of(int port) const noexcept {
        if (port < 0)
            return Lv2PortKind::None;
        if (port >= audio_in_begin() && port < audio_out_begin())
            return Lv2PortKind::AudioIn;
        if (port >= audio_out_begin() && port < control_begin())
            return Lv2PortKind::AudioOut;
        if (port >= control_begin() && port < atom_in_begin())
            return Lv2PortKind::Control;
        if (accepts_midi && port == atom_in_begin())
            return Lv2PortKind::AtomIn;
        if (produces_midi && port == atom_out_begin())
            return Lv2PortKind::AtomOut;
        if (port == latency_index())
            return Lv2PortKind::Latency;
        return Lv2PortKind::None;
    }

    /// Offset of @p port within its own kind — the array index for the
    /// multi-slot kinds, and 0 for the single-slot ones. Meaningless for a
    /// port `kind_of()` calls `None`.
    [[nodiscard]] constexpr int slot_of(int port) const noexcept {
        switch (kind_of(port)) {
        case Lv2PortKind::AudioIn:
            return port - audio_in_begin();
        case Lv2PortKind::AudioOut:
            return port - audio_out_begin();
        case Lv2PortKind::Control:
            return port - control_begin();
        default:
            return 0;
        }
    }
};

/// URIDs the `time:Position` decode needs, resolved once in instantiate().
/// All zero when the host provides no URID map, in which case the decode is a
/// no-op — every comparison against a zero URID fails.
struct Lv2TimeUrids {
    LV2_URID atom_object = 0;
    LV2_URID atom_blank = 0; // pre-1.8 hosts still send Blank for an object
    LV2_URID atom_float = 0;
    LV2_URID atom_double = 0;
    LV2_URID atom_int = 0;
    LV2_URID atom_long = 0;
    LV2_URID atom_bool = 0;

    LV2_URID time_position = 0;
    LV2_URID time_bar = 0;
    LV2_URID time_beat = 0;
    LV2_URID time_beat_unit = 0;
    LV2_URID time_beats_per_bar = 0;
    LV2_URID time_beats_per_minute = 0;
    LV2_URID time_frame = 0;
    LV2_URID time_speed = 0;
};

/// A `time:Position` object decoded into LV2's own units and conventions.
///
/// Each field carries its own presence flag because a host sends only the
/// properties it knows: `time:frame` + `time:speed` from a plain sample
/// transport, the full musical set from a tempo-aware one. The flags become
/// `TransportValidity` bits on `ProcessContext`, so a processor can tell an
/// unavailable value from a default one.
struct Lv2TimePosition {
    bool has_speed = false;
    bool has_frame = false;
    bool has_bar = false;
    bool has_beat = false;
    bool has_beats_per_bar = false;
    bool has_beat_unit = false;
    bool has_beats_per_minute = false;

    float speed = 0.0f;         ///< 0 = stopped, 1 = normal-rate playback
    int64_t frame = 0;          ///< sample position on the host timeline
    int64_t bar = 0;            ///< zero-based bar number
    double beat = 0.0;          ///< running beat count, in `beat_unit` beats
    float beats_per_bar = 4.0f; ///< time-signature numerator
    int32_t beat_unit = 4;      ///< time-signature denominator
    float beats_per_minute = 120.0f;

    [[nodiscard]] bool any() const noexcept {
        return has_speed || has_frame || has_bar || has_beat || has_beats_per_bar ||
               has_beat_unit || has_beats_per_minute;
    }
};

// LV2 plugin instance — wraps a Pulp Processor
struct PulpLv2Instance {
    // The store is declared before the Processor so it is destroyed after it.
    // `Processor::state()` dereferences a pointer to this store, and a Processor
    // may read it from its destructor or from a worker thread that destructor is
    // about to join. Reversing these two lines hands that thread a freed store.
    state::StateStore store;
    std::unique_ptr<Processor> processor;
    // Param-events sidecar, set on the Processor each run() so the contract is
    // uniform across formats. LV2 control-port values are applied through
    // `store` as before; this queue carries no events yet (atom-based
    // sample-accurate param events are future work), but it gives the Processor
    // a non-null queue and pairs with the RT-safety guard around process().
    state::ParameterEventQueue param_events;
    ProcessorFactory factory;

    // Pre-reserved MIDI scratch, owned by the instance so run() never
    // heap-allocates on the audio thread. Sized once in instantiate() with
    // set_realtime_capacity_limit(true) — mirrors the CLAP/VST3 adapters
    // (clap_adapter.cpp reserve() + set_realtime_capacity_limit(true)).
    // run() clear()s and reuses these every block instead of constructing
    // fresh stack-local buffers whose first add() would allocate.
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;

    // Audio working state
    double sample_rate = 48000.0;

    // Block ceiling the Processor was prepared for. Read from the host's
    // `bufsz:maxBlockLength` option in instantiate() when the options feature
    // is present, else kDefaultMaxBlockLength. run() clamps to it so a host
    // that neither bounds its block length nor declares the option cannot
    // overrun scratch sized by prepare().
    int max_block_length = kDefaultMaxBlockLength;

    // Port connections (set by connect_port). `Lv2PortLayout` above owns the
    // index → slot mapping; the audio arrays are fixed at kMaxChannels, and
    // instantiate() refuses a descriptor whose buses sum past that ceiling in
    // either direction, so a declared port index can never run off them.
    float* audio_in_ports[kMaxChannels] = {};
    float* audio_out_ports[kMaxChannels] = {};
    float** control_in_ports = nullptr;   // One per parameter
    float** control_out_ports = nullptr;  // One per parameter (for output)

    int num_audio_inputs = 0;
    int num_audio_outputs = 0;
    int num_params = 0;
    std::vector<state::ParamID> param_ids;  // Maps control port index → ParamID

    // URID feature resolution. The LV2 host passes an LV2_URID_Map feature in
    // instantiate(); we cache the map function plus the URIDs we need at
    // runtime so inner-loop code does not call map() on hot paths. All fields
    // are 0 when the feature is absent; instantiate() returns nullptr in that
    // case, so real plugin code always sees non-zero values here.
    LV2_URID_Map* urid_map = nullptr;
    LV2_URID urid_midi_event = 0;      // LV2_MIDI__MidiEvent
    LV2_URID urid_atom_sequence = 0;   // LV2_ATOM__Sequence
    LV2_URID urid_atom_chunk = 0;      // LV2_ATOM__Chunk
    LV2_URID urid_state_blob = 0;      // kStateBlobUri
    Lv2TimeUrids time_urids{};

    // Latched host transport. LV2 hosts are not required to send a
    // `time:Position` every cycle — some send one only when the transport
    // changes — so the last one received is retained and its position fields
    // advanced by the block length while `speed` is non-zero. A fresh Position
    // always overwrites the extrapolation, so a host that does send one per
    // cycle never drifts. Without the latch a stale sample position would read
    // as a seek on every block and reset tempo-synced DSP continuously.
    Lv2TimePosition transport{};
    bool has_transport = false;

    // Previous-block transport snapshot backing the shared change-flag diff
    // (`tempo_changed` / `transport_jump` / ...), exactly as the other format
    // adapters keep one.
    detail::PlayheadSnapshot playhead_prev{};

    // Atom-port MIDI input. When the plug-in declares accepts_midi, the host
    // connects an LV2_Atom_Sequence buffer to the port after the control
    // ports. run() iterates it, extracts MIDI events whose atom `type` field is
    // urid_midi_event, and feeds them to the Processor.
    bool accepts_midi = false;
    void* midi_in_atom = nullptr;

    // Parallel output-atom port for plugins that emit MIDI. The host pre-sizes
    // the buffer via lv2:minimumSize and signals capacity in the atom.size
    // field on entry to run(); the plugin overwrites the sequence using the
    // lv2_atom_sequence_clear + append_event helpers.
    bool produces_midi = false;
    void* midi_out_atom = nullptr;

    // Latency-reporting output control port. Emitted unconditionally by
    // generate_plugin_ttl() as the last port (designation lv2:latency,
    // portProperty lv2:reportsLatency); connect_port() wires the host's
    // single-float buffer here and run() writes the processor's current
    // latency_samples() into it each block, so a latent Pulp processor is
    // PDC-compensated under LV2 like it is under every other format.
    float* latency_port = nullptr;

    /// This instance's port ordering — the same one `generate_plugin_ttl()`
    /// numbered the manifest with.
    [[nodiscard]] Lv2PortLayout port_layout() const noexcept {
        return {num_audio_inputs, num_audio_outputs, num_params, accepts_midi, produces_midi};
    }
};

/// Resolve LV2_URID_Map from a features array.
/// Returns nullptr if the feature is absent. Exposed for unit testing.
LV2_URID_Map* find_urid_map(const LV2_Feature* const* features);

/// Resolve the `options:options` feature from a features array.
/// Returns nullptr if the feature is absent. The returned array is terminated
/// by a zeroed option. Exposed for unit testing.
const LV2_Options_Option* find_options(const LV2_Feature* const* features);

/// Read `bufsz:maxBlockLength` out of a host options array.
///
/// Returns @p fallback when @p options is null, the key is absent, or the
/// value is not a positive integer. Accepts the option as `atom:Int` or
/// `atom:Long` — hosts differ, and the spec fixes only the property, not the
/// width. Pure; exposed for unit testing.
int max_block_length_from_options(const LV2_Options_Option* options, LV2_URID key_max_block_length,
                                  LV2_URID urid_atom_int, LV2_URID urid_atom_long, int fallback);

// Generate an LV2 TTL manifest string for a plugin
// This creates the plugin.ttl content with port definitions
std::string generate_plugin_ttl(const PluginDescriptor& desc,
                                 const state::StateStore& store,
                                 const std::string& uri);

// Generate the manifest.ttl that points to the plugin binary
std::string generate_manifest_ttl(const std::string& plugin_uri,
                                   const std::string& binary_name);

} // namespace pulp::format::lv2_adapter
