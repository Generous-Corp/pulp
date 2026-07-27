#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timeline/model.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pulp::timeline {

// Import and export of Standard MIDI Files (SMF, RP-001) against the project's
// tempo and meter maps.
//
// An SMF carries its own musical timebase: a header division in ticks per
// quarter note, plus Set Tempo and Time Signature meta-events that place tempo
// and meter changes on that grid. Conversion therefore stays entirely in the
// musical domain — SMF ticks scale to canonical timebase::kTicksPerQuarter
// ticks, and the file's tempo/meter meta-events become the Project's TempoMap
// and MeterMap. No seconds-domain flattening is involved in either direction,
// so a tempo change mid-file survives the round trip as a tempo point rather
// than as pre-multiplied wall-clock positions.
//
// Tick exactness. `canonical = round(smf_tick * kTicksPerQuarter / division)`
// is exact for every tick when `kTicksPerQuarter % division == 0` (true for
// 96, 120, 192, 240, 480, 960, and every other divisor of 705600; false for
// 384 and 1920). Otherwise the conversion rounds half away from zero and the
// error is bounded by 0.5 canonical ticks; SmfImport reports both facts.
// Export is exact by default: a canonical tick that the requested division
// cannot represent is an error unless `allow_lossy_tick_rounding` is set.
//
// Documented import subset (everything outside it fails closed):
//   * MThd format 0 or 1; a metrical (non-SMPTE) division
//   * MTrk chunks only, exactly the count the header declares, no trailing bytes
//   * Note On (0x9n) and Note Off (0x8n); a Note On with velocity 0 is a
//     Note Off, per the specification
//   * Meta events Set Tempo (0x51), Time Signature (0x58),
//     Sequence/Track Name (0x03), and End of Track (0x2F)
// Any other channel message, system-exclusive block, or meta event is rejected
// unless the caller explicitly opts into SmfUnsupportedEventPolicy::IgnoreNonNote.
//
// Fields carried by supported events that have no timeline counterpart are
// documented rather than silently reinterpreted: the Time Signature event's
// metronome hints (MIDI clocks per click, 32nd notes per quarter) are not
// retained, and export writes the conventional 24/8.
//
// Export covers note content plus the tempo and meter maps. Device chains,
// automation lanes, takes, freezes, and media assets have no SMF
// representation and are outside the format's scope. Per-note probability,
// conditions, and ratchets are playback semantics rather than fixed MIDI
// events, so exporting a clip that authors them fails with UnsupportedFeature.
/** @addtogroup timeline_interchange
 * @{
 */

/// @file smf.hpp
/// Bounded import and export of the documented Standard MIDI File subset.
///
/// An SMF carries its own musical timebase: a metrical header division plus
/// tempo and time-signature events. Conversion stays in the musical domain;
/// no seconds-domain flattening occurs.
///
/// Import accepts format 0 or 1, a metrical division, Note On/Off, Set Tempo,
/// Time Signature, Sequence/Track Name, and End of Track. Other well-formed
/// events require an explicit IgnoreNonNote policy. Export covers note content
/// plus tempo and meter maps; other Timeline content lies outside the format.
///
/// Tick conversion is exact when the file division divides
/// timebase::kTicksPerQuarter. Import reports rounding when it does not. Export
/// rejects inexact ticks unless allow_lossy_tick_rounding is explicitly set.

/// Stable category for an SMF import or export failure.
enum class SmfErrorCode : std::uint8_t {
    Truncated,             ///< Input ended inside a chunk, event, or payload.
    MissingHeader,         ///< No MThd chunk starts the input.
    InvalidHeader,         ///< The MThd contents are unusable.
    UnsupportedFormat,     ///< The file uses independent-sequence format 2.
    UnsupportedDivision,   ///< The division is SMPTE-based or zero.
    UnsupportedFeature,    ///< A construct lies outside the accepted subset.
    MalformedEvent,        ///< Running status, variable length, or meta length is invalid.
    UnbalancedNote,        ///< A note-on/off pair is incomplete.
    InvalidValue,          ///< A value is outside its representable range.
    InexactTickConversion, ///< A requested export tick is not exactly representable.
    TickRangeExceeded,     ///< A tick exceeds the SMF or canonical domain.
    LimitExceeded,         ///< A configured resource limit was exceeded.
    TempoMapRejected,      ///< The assembled tempo map is invalid.
    MeterMapRejected,      ///< The assembled meter map is invalid.
    ModelRejected,         ///< The assembled Project violates a model invariant.
};

/// Diagnostic returned when an SMF operation rejects the whole conversion.
struct SmfError {
    /// Failure category.
    SmfErrorCode code = SmfErrorCode::MissingHeader;
    /// Human-readable detail naming the offending chunk, event, or value.
    std::string message;
    /// Underlying model failure when code is ModelRejected.
    ModelError model_error{};
};

/// Policy for a well-formed event outside the documented import subset.
enum class SmfUnsupportedEventPolicy : std::uint8_t {
    /// Reject the file so an unhandled event is never silently dropped.
    Reject,
    /// Discard well-formed non-note channel, system-exclusive, and meta events.
    IgnoreNonNote,
};

/// Hard resource ceilings for one SMF import.
///
/// Every limit is checked before the corresponding state grows, so untrusted
/// input cannot force an allocation larger than the caller authorized. A zero
/// limit rejects any corresponding non-empty resource.
struct SmfImportLimits {
    /// Raw file bytes, checked before decoding.
    std::size_t max_file_bytes = 64u * 1024u * 1024u;
    /// Maximum MTrk chunks.
    std::size_t max_tracks = 4'096;
    /// Maximum decoded events across the whole file.
    std::size_t max_events = 5'000'000;
    /// Maximum completed notes across the whole file.
    std::size_t max_notes = 5'000'000;
    /// Maximum simultaneously open notes on one track.
    std::size_t max_concurrent_notes = 65'536;
    /// Maximum Set Tempo events.
    std::size_t max_tempo_points = 65'536;
    /// Maximum Time Signature events.
    std::size_t max_meter_points = 65'536;
    /// Maximum bytes in one meta-event or system-exclusive payload.
    std::size_t max_payload_bytes = 1u * 1024u * 1024u;
    /// Maximum decoded bytes in one track name.
    std::size_t max_track_name_bytes = 4'096;
    /// Absolute SMF tick ceiling for any event.
    std::int64_t max_smf_ticks = std::int64_t{1} << 34;
};

/// Caller-selected resource and unsupported-event policy for SMF import.
struct SmfImportOptions {
    /// Resource ceilings.
    SmfImportLimits limits{};
    /// Policy for well-formed events outside the note subset.
    SmfUnsupportedEventPolicy unsupported_events = SmfUnsupportedEventPolicy::Reject;
};

/// Imported immutable Project plus tick-conversion fidelity metadata.
struct SmfImport {
    /// Atomically assembled Timeline project.
    Project project;
    /// MThd ticks-per-quarter division.
    std::uint16_t division = 0;
    /// Whether every imported SMF tick has an exact canonical representation.
    bool exact_tick_conversion = true;
    /// Worst imported tick rounding error in canonical ticks.
    std::int64_t max_tick_rounding_error = 0;
};

/// Decodes SMF bytes into a Project using default limits and strict event policy.
///
/// @param file_bytes Complete SMF bytes, borrowed for the call.
/// @return The imported Project and fidelity metadata, or a structured error.
runtime::Result<SmfImport, SmfError> import_smf(std::span<const std::uint8_t> file_bytes);

/// Decodes SMF bytes into a Project under explicit import options.
///
/// Malformed input and unsupported constructs reject the whole import. Each
/// content-bearing MTrk becomes one Track; tempo-only conductor chunks create
/// no empty Timeline track.
///
/// @param file_bytes Complete SMF bytes, borrowed for the call.
/// @param options Resource and unsupported-event policy.
/// @return The imported Project and fidelity metadata, or a structured error.
runtime::Result<SmfImport, SmfError> import_smf(std::span<const std::uint8_t> file_bytes,
                                                const SmfImportOptions& options);

/// Tick-grid, loss policy, and work bound for SMF export.
struct SmfExportOptions {
    /// MThd ticks per quarter; 960 is exact against the canonical tick grid.
    std::uint16_t ticks_per_quarter = 960;
    /// Whether unrepresentable ticks may round half away from zero.
    bool allow_lossy_tick_rounding = false;
    /// Whether clips with non-note, non-empty content may be omitted.
    bool skip_non_note_clips = false;
    /// Maximum emitted MIDI events across the whole file.
    std::size_t max_events = 5'000'000;
};

/// Encoded SMF bytes plus tick-conversion fidelity metadata.
struct SmfExport {
    /// Complete encoded SMF bytes.
    std::vector<std::uint8_t> bytes;
    /// Whether every emitted tick was represented exactly.
    bool exact_tick_conversion = true;
    /// Worst canonical-to-reconstructed tick error over emitted events.
    std::int64_t max_tick_rounding_error = 0;
};

/// Encodes a Project's root sequence as a format-1 SMF with strict defaults.
///
/// @param project Immutable source project.
/// @return Encoded bytes and fidelity metadata, or a structured error.
runtime::Result<SmfExport, SmfError> export_smf(const Project& project);

/// Encodes a Project's root sequence as a format-1 SMF under explicit options.
///
/// Track 0 carries tempo and meter maps; each Timeline track with note content
/// follows as its own MTrk. A note velocity that scales to MIDI zero is rejected
/// because zero-velocity Note On means Note Off. Running status is not emitted.
///
/// @param project Immutable source project.
/// @param options Tick-grid, loss, and resource policy.
/// @return Encoded bytes and fidelity metadata, or a structured error.
runtime::Result<SmfExport, SmfError> export_smf(const Project& project,
                                                const SmfExportOptions& options);

/// @}

} // namespace pulp::timeline
