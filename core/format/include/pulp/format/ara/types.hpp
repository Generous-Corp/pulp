#pragma once

// ARA 2.x core type layer (workstream 06 slice 6.2).
//
// SDK-independent mirrors of the ARA concepts a Pulp plugin needs to author
// against. The companion factory layer (slices 6.3..6.5 — VST3, AU, CLAP)
// translates between these types and the Celemony ARA SDK's C interface.
// Plugin authors only ever see the Pulp types; the SDK header is never
// re-exported from a Pulp public header so that PULP_HAS_ARA gating stays
// invisible to consumers.
//
// All identifiers are opaque integer handles (ARAObjectRef-like): they are
// minted by the host and Pulp passes them back through callbacks. Pulp
// never inspects them.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::format::ara {

// ── Opaque host-minted identifiers ───────────────────────────────────────

using AudioSourceId       = uint64_t;
using AudioModificationId = uint64_t;
using MusicalContextId    = uint64_t;
using RegionSequenceId    = uint64_t;
using PlaybackRegionId    = uint64_t;

// ── Audio source ─────────────────────────────────────────────────────────

/// Properties of an audio source, as reported by the host. The audio data
/// itself is fetched on demand through the host's content reader callbacks.
struct AraAudioSource {
    AudioSourceId id = 0;
    std::string name;            ///< Display name (track name, sample name, ...)
    std::string persistent_id;   ///< Stable across host sessions when reopened
    int64_t sample_count = 0;    ///< Total length in frames at native rate
    double sample_rate = 0.0;    ///< Native sample rate
    int channel_count = 0;
    bool merits_content_based_processing = false;
};

// ── Audio modification ───────────────────────────────────────────────────

/// A user-editable view onto an AraAudioSource. Multiple modifications of
/// the same source are independent — they let the user create variations
/// (e.g. different pitch corrections of the same vocal take) without
/// duplicating audio.
struct AraAudioModification {
    AudioModificationId id = 0;
    AudioSourceId source_id = 0;
    std::string name;
    std::string persistent_id;
};

// ── Musical context ──────────────────────────────────────────────────────

/// Tempo + meter context shared by one or more region sequences. Pulp
/// surfaces only the fields plugins commonly read — chord/key/scale
/// readers can be added when a slice needs them.
struct AraTempoEntry {
    double position_seconds = 0.0;
    double position_quarters = 0.0;
};

struct AraMusicalContext {
    MusicalContextId id = 0;
    std::string name;
    std::vector<AraTempoEntry> tempo_map;   ///< Sparse; empty = constant tempo
    int time_signature_numerator = 4;
    int time_signature_denominator = 4;
};

// ── Region sequence + playback region ────────────────────────────────────

/// A region sequence groups playback regions on the same logical track
/// (host's notion of "track" or "lane"). The musical_context field links
/// it to a tempo/meter map.
struct AraRegionSequence {
    RegionSequenceId id = 0;
    MusicalContextId musical_context_id = 0;
    std::string name;
    int colour_argb = 0;          ///< Track colour, AARRGGBB; 0 if unset
};

/// A playback region maps a slice of an audio modification onto the
/// host's timeline. All four time fields are in seconds, matching ARA's
/// ARAPlaybackRegionProperties contract. Earlier drafts modelled the
/// modification-time pair as int64 samples, which would have forced
/// sample-rate-dependent conversion in every adapter and dropped
/// fractional-time precision (#185 review).
struct AraPlaybackRegion {
    PlaybackRegionId id = 0;
    AudioModificationId modification_id = 0;
    RegionSequenceId region_sequence_id = 0;
    double start_in_playback_time = 0.0;        // seconds
    double duration_in_playback_time = 0.0;     // seconds
    double start_in_modification_time = 0.0;    // seconds
    double duration_in_modification_time = 0.0; // seconds
    std::string name;
    int colour_argb = 0;
};

// ── Content-changed flags ────────────────────────────────────────────────

/// Bitset describing what changed about an audio source's content. Plugins
/// use these to decide what analysis to invalidate.
enum class AudioSourceContentChange : uint32_t {
    None       = 0,
    Notes      = 1u << 0,
    Tempo      = 1u << 1,
    Tuning     = 1u << 2,
    KeySignatures = 1u << 3,
    Samples    = 1u << 4,   ///< raw sample data changed
};

inline AudioSourceContentChange operator|(AudioSourceContentChange a,
                                          AudioSourceContentChange b) {
    return static_cast<AudioSourceContentChange>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool any(AudioSourceContentChange flags) {
    return static_cast<uint32_t>(flags) != 0;
}
inline bool has(AudioSourceContentChange flags, AudioSourceContentChange bit) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(bit)) != 0;
}

} // namespace pulp::format::ara
