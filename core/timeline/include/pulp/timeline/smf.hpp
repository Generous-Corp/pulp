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

enum class SmfErrorCode : std::uint8_t {
    Truncated,             // The data ended inside a chunk, event, or payload.
    MissingHeader,         // No MThd chunk at the start of the data.
    InvalidHeader,         // MThd is present but its contents are unusable.
    UnsupportedFormat,     // MThd format 2 (independent sequences).
    UnsupportedDivision,   // An SMPTE (negative) division, or a zero division.
    UnsupportedFeature,    // A construct outside the documented subset.
    MalformedEvent,        // Bad running status, variable-length quantity, or meta length.
    UnbalancedNote,        // A Note On with no Note Off, or a Note Off with no Note On.
    InvalidValue,          // A value is well-formed but out of the representable range.
    InexactTickConversion, // A tick the requested SMF division cannot represent exactly.
    TickRangeExceeded,     // A tick falls outside the SMF or canonical tick domain.
    LimitExceeded,         // A caller-configurable resource limit was exceeded.
    TempoMapRejected,      // The assembled tempo points were rejected by the timebase.
    MeterMapRejected,      // The assembled meter points were rejected by the timebase.
    ModelRejected,         // The timeline model rejected the assembled project.
};

struct SmfError {
    SmfErrorCode code = SmfErrorCode::MissingHeader;
    // Human-readable detail naming the offending chunk, event, or value.
    std::string message;
    // Populated only when code == ModelRejected: the underlying model failure.
    ModelError model_error{};
};

// How to treat a well-formed event that falls outside the documented subset.
enum class SmfUnsupportedEventPolicy : std::uint8_t {
    // Reject the file. The default: an unhandled event is never dropped.
    Reject,
    // Discard non-note channel messages, system-exclusive blocks, and meta
    // events outside the subset. An explicit caller decision, not a silent drop.
    IgnoreNonNote,
};

// Hard resource ceilings for one import. Every limit is checked before the
// corresponding state grows, so untrusted input cannot force an allocation
// larger than the caller authorized. A zero limit rejects any non-empty
// corresponding resource.
struct SmfImportLimits {
    // Raw file bytes, checked before any decoding.
    std::size_t max_file_bytes = 64u * 1024u * 1024u;
    std::size_t max_tracks = 4'096;
    // Totals across the whole file, not per track.
    std::size_t max_events = 5'000'000;
    std::size_t max_notes = 5'000'000;
    // Notes sounding simultaneously on one track while pairing Note On/Off.
    std::size_t max_concurrent_notes = 65'536;
    std::size_t max_tempo_points = 65'536;
    std::size_t max_meter_points = 65'536;
    // One meta-event or system-exclusive payload.
    std::size_t max_payload_bytes = 1u * 1024u * 1024u;
    std::size_t max_track_name_bytes = 4'096;
    // Absolute SMF tick ceiling for any event. Bounds the canonical tick
    // product well inside the signed 64-bit domain.
    std::int64_t max_smf_ticks = std::int64_t{1} << 34;
};

struct SmfImportOptions {
    SmfImportLimits limits{};
    SmfUnsupportedEventPolicy unsupported_events = SmfUnsupportedEventPolicy::Reject;
};

struct SmfImport {
    Project project;
    // The MThd division the file declared, in ticks per quarter note.
    std::uint16_t division = 0;
    // True when kTicksPerQuarter % division == 0, i.e. every SMF tick in the
    // file has an exact canonical representation.
    bool exact_tick_conversion = true;
    // Worst-case |canonical - exact| introduced by rounding, in canonical
    // ticks. Zero when exact_tick_conversion is true, otherwise at most 1.
    std::int64_t max_tick_rounding_error = 0;
};

// Decode SMF bytes into a timeline Project. Malformed input and constructs
// outside the documented subset are rejected with a descriptive error rather
// than partially imported. Each MTrk becomes one Track, preserving its name;
// a track's notes become a single musical Clip spanning its first note start
// to its last note end. A chunk with neither notes nor a name carries no
// track-level content — its tempo and time-signature events already live in
// the maps — so it yields no Track, which is what makes import(export(project))
// return the same track set rather than growing a conductor track each pass.
runtime::Result<SmfImport, SmfError> import_smf(std::span<const std::uint8_t> file_bytes);
runtime::Result<SmfImport, SmfError> import_smf(std::span<const std::uint8_t> file_bytes,
                                                const SmfImportOptions& options);

struct SmfExportOptions {
    // MThd division in ticks per quarter note. 960 divides kTicksPerQuarter,
    // so the default grid is exactly representable in both directions.
    std::uint16_t ticks_per_quarter = 960;
    // When false (the default) a canonical tick the division cannot represent
    // exactly is an error. When true the tick rounds half away from zero and
    // the reported error bound applies.
    bool allow_lossy_tick_rounding = false;
    // When false (the default) a clip whose content is neither note content
    // nor empty is an error. When true such clips are skipped — an explicit
    // caller decision, not a silent drop.
    bool skip_non_note_clips = false;
    // Ceiling on emitted MIDI events across the whole file.
    std::size_t max_events = 5'000'000;
};

struct SmfExport {
    std::vector<std::uint8_t> bytes;
    // False when any note tick was rounded to the SMF grid, which is possible
    // only under allow_lossy_tick_rounding.
    bool exact_tick_conversion = true;
    // Worst-case |canonical - reconstructed| over every emitted tick, in
    // canonical ticks. Bounded by kTicksPerQuarter / (2 * ticks_per_quarter).
    // A note's duration can shift by twice this, since its start and end round
    // independently.
    std::int64_t max_tick_rounding_error = 0;
};

// Encode a Project's root sequence as a format-1 SMF. Track 0 is the conductor
// track carrying the project's tempo and meter maps; each timeline track with
// note content follows as its own MTrk. Note velocity is scaled from 16-bit to
// the 7-bit MIDI domain; a velocity that would scale to zero is rejected rather
// than silently rewritten, because a zero-velocity Note On means Note Off.
// Running status is never emitted.
runtime::Result<SmfExport, SmfError> export_smf(const Project& project);
runtime::Result<SmfExport, SmfError> export_smf(const Project& project,
                                                const SmfExportOptions& options);

} // namespace pulp::timeline
