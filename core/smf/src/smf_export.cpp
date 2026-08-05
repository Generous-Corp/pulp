#include <pulp/timeline/smf.hpp>

#include "smf_error.hpp"
#include "smf_export_internal.hpp"
#include "smf_tick_scale.hpp"

#include <pulp/interchange/capability.hpp>
#include <pulp/timebase/compiled_meter_map.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timebase/tick.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace pulp::timeline {
namespace {

using detail::canonical_to_smf_ticks;
using detail::decimal;
using detail::smf_error;
using detail::TickScale;
using runtime::Err;
using runtime::Ok;

using ExportResult = runtime::Result<SmfExport, SmfError>;

using interchange::Concept;

constexpr std::array kImplementedExports{
    Concept::TrackFlat,   Concept::ClipMusical, Concept::ClipNote,
    Concept::TempoSingle, Concept::TempoMap,    Concept::MeterSingle,
    Concept::MeterMap,
};

constexpr bool implemented(Concept concept_value) noexcept {
    return std::find(kImplementedExports.begin(), kImplementedExports.end(), concept_value) !=
           kImplementedExports.end();
}

static_assert([] {
    for (std::size_t index = 0; index < interchange::kConceptCount; ++index) {
        const auto concept_value = static_cast<Concept>(index);
        if (interchange::export_is_lossless(interchange::Format::Smf, concept_value) &&
            !implemented(concept_value))
            return false;
    }
    return true;
}(), "a lossless SMF export row is not implemented by the writer");

constexpr std::uint32_t kMaximumVariableLength = 0x0fff'ffffu;
constexpr std::uint8_t kNoteOffReleaseVelocity = 0x40u;
// The conventional metronome hints: 24 MIDI clocks per click, 8 thirty-second
// notes per quarter. The timeline model carries no counterpart to reproduce.
constexpr std::uint8_t kMetronomeClocksPerClick = 24u;
constexpr std::uint8_t kThirtySecondsPerQuarter = 8u;

bool may_drop_non_note_content(const ClipContent& content,
                               const detail::SmfExportLossPolicy& policy) noexcept {
    return std::visit(ClipContentCases{
                          [](const EmptyContent&) { return true; },
                          [&](const MediaRef&) { return policy.drop_media_clips; },
                          [](const MidiContent&) { return false; },
                          [&](const RegisteredContent&) {
                              return policy.drop_registered_content;
                          },
                          [&](const OpaqueContent&) { return policy.drop_opaque_content; },
                          [&](const SequenceRef&) { return policy.drop_nested_sequences; },
                      },
                      content);
}

// Scale the model's 16-bit velocity down to 7-bit MIDI. Inverse of the
// importer's scale_velocity_7_to_16 over the 7-bit-representable values.
constexpr std::uint8_t scale_velocity_16_to_7(std::uint16_t velocity_16) noexcept {
    return static_cast<std::uint8_t>((static_cast<std::uint32_t>(velocity_16) * 127u + 0x7fffu) /
                                     0xffffu);
}

// One MIDI or meta event to emit, already positioned on the SMF grid.
struct OutputEvent {
    std::int64_t tick = 0;
    // Note offs sort before note ons at one tick so a retrigger of the same
    // pitch does not truncate the note that just started.
    int rank = 0;
    std::vector<std::uint8_t> bytes;
};

bool output_before(const OutputEvent& lhs, const OutputEvent& rhs) noexcept {
    if (lhs.tick != rhs.tick)
        return lhs.tick < rhs.tick;
    if (lhs.rank != rhs.rank)
        return lhs.rank < rhs.rank;
    // Byte order is the final tie-break so emission is deterministic for
    // simultaneous events that the timeline model leaves unordered.
    return lhs.bytes < rhs.bytes;
}

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8u));
    out.push_back(static_cast<std::uint8_t>(value));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24u));
    out.push_back(static_cast<std::uint8_t>(value >> 16u));
    out.push_back(static_cast<std::uint8_t>(value >> 8u));
    out.push_back(static_cast<std::uint8_t>(value));
}

void append_variable_length(std::vector<std::uint8_t>& out, std::uint32_t value) {
    std::uint8_t groups[4] = {0, 0, 0, 0};
    std::size_t count = 0;
    do {
        groups[count++] = static_cast<std::uint8_t>(value & 0x7fu);
        value >>= 7u;
    } while (value != 0);
    while (count != 0) {
        --count;
        out.push_back(static_cast<std::uint8_t>(count != 0 ? (groups[count] | 0x80u)
                                                           : groups[count]));
    }
}

// Serialize one MTrk chunk from events already sorted into emission order.
std::optional<SmfError> append_track_chunk(std::vector<std::uint8_t>& out,
                                           const std::vector<OutputEvent>& events) {
    out.insert(out.end(), {'M', 'T', 'r', 'k'});
    const auto length_offset = out.size();
    append_u32(out, 0);
    const auto body_offset = out.size();

    std::int64_t previous_tick = 0;
    for (const auto& event : events) {
        const auto delta = event.tick - previous_tick;
        if (delta < 0 || delta > static_cast<std::int64_t>(kMaximumVariableLength))
            return smf_error(SmfErrorCode::TickRangeExceeded,
                             "delta time " + decimal(delta) +
                                 " exceeds the variable-length quantity range");
        append_variable_length(out, static_cast<std::uint32_t>(delta));
        out.insert(out.end(), event.bytes.begin(), event.bytes.end());
        previous_tick = event.tick;
    }
    // End of track, at the position of the last event.
    append_variable_length(out, 0);
    out.insert(out.end(), {0xffu, 0x2fu, 0x00u});

    const auto body_length = out.size() - body_offset;
    if (body_length > 0xffff'ffffull)
        return smf_error(SmfErrorCode::LimitExceeded, "track chunk exceeds the 32-bit chunk size");
    const auto length = static_cast<std::uint32_t>(body_length);
    out[length_offset] = static_cast<std::uint8_t>(length >> 24u);
    out[length_offset + 1] = static_cast<std::uint8_t>(length >> 16u);
    out[length_offset + 2] = static_cast<std::uint8_t>(length >> 8u);
    out[length_offset + 3] = static_cast<std::uint8_t>(length);
    return std::nullopt;
}

class Exporter {
  public:
    Exporter(const Project& project, const SmfExportOptions& options,
             detail::SmfExportLossPolicy loss_policy = {})
        : project_(project), options_(options),
          scale_(TickScale::create(options.ticks_per_quarter)), loss_policy_(loss_policy) {}

    ExportResult run();

  private:
    // Convert a canonical position, applying the caller's exactness policy.
    runtime::Result<std::int64_t, SmfError> to_smf_tick(std::int64_t canonical_tick,
                                                        const char* what);
    std::optional<SmfError> reserve_event();
    runtime::Result<std::vector<OutputEvent>, SmfError> build_conductor_track();
    runtime::Result<std::vector<OutputEvent>, SmfError> build_note_track(const Track& track);

    const Project& project_;
    const SmfExportOptions& options_;
    TickScale scale_;
    detail::SmfExportLossPolicy loss_policy_;
    std::size_t event_count_ = 0;
    bool exact_ = true;
    std::int64_t max_rounding_error_ = 0;
};

runtime::Result<std::int64_t, SmfError> Exporter::to_smf_tick(std::int64_t canonical_tick,
                                                               const char* what) {
    using TickResult = runtime::Result<std::int64_t, SmfError>;
    const auto converted = canonical_to_smf_ticks(canonical_tick, scale_);
    if (!converted)
        return TickResult(Err(smf_error(SmfErrorCode::TickRangeExceeded,
                                        std::string(what) + " tick " + decimal(canonical_tick) +
                                            " does not fit the SMF tick domain")));
    if (!converted->exact) {
        if (!options_.allow_lossy_tick_rounding)
            return TickResult(Err(smf_error(
                SmfErrorCode::InexactTickConversion,
                std::string(what) + " tick " + decimal(canonical_tick) +
                    " is not representable at " + decimal(options_.ticks_per_quarter) +
                    " ticks per quarter note")));
        exact_ = false;
        max_rounding_error_ = std::max(max_rounding_error_, converted->rounding_error);
    }
    return TickResult(Ok(converted->smf_tick));
}

std::optional<SmfError> Exporter::reserve_event() {
    if (event_count_ >= options_.max_events)
        return smf_error(SmfErrorCode::LimitExceeded,
                         "emitted events exceed max_events (" +
                             decimal(static_cast<std::int64_t>(options_.max_events)) + ")");
    ++event_count_;
    return std::nullopt;
}

runtime::Result<std::vector<OutputEvent>, SmfError> Exporter::build_conductor_track() {
    using TrackResult = runtime::Result<std::vector<OutputEvent>, SmfError>;
    std::vector<OutputEvent> events;

    for (const auto& point : project_.tempo_map().points()) {
        if (point.curve_to_next != timebase::TempoCurve::Constant &&
            !loss_policy_.step_tempo_ramps)
            return TrackResult(Err(smf_error(
                SmfErrorCode::UnsupportedFeature,
                "tempo ramp at tick " + decimal(point.tick.value) +
                    " has no standard MIDI file representation")));
        auto tick = to_smf_tick(point.tick.value, "tempo point");
        if (!tick)
            return TrackResult(Err(tick.error()));
        const auto microseconds =
            static_cast<std::int64_t>(60'000'000.0 / point.bpm + 0.5);
        if (microseconds <= 0 || microseconds > 0xff'ffff)
            return TrackResult(Err(smf_error(
                SmfErrorCode::InvalidValue,
                "tempo of " + decimal(static_cast<std::int64_t>(point.bpm)) +
                    " bpm is not representable as a set-tempo event")));
        if (auto failure = reserve_event())
            return TrackResult(Err(*failure));
        events.push_back(OutputEvent{
            tick.value(), 0,
            {0xffu, 0x51u, 0x03u, static_cast<std::uint8_t>(microseconds >> 16u),
             static_cast<std::uint8_t>(microseconds >> 8u),
             static_cast<std::uint8_t>(microseconds)}});
    }

    for (const auto& point : project_.meter_map().points()) {
        auto tick = to_smf_tick(point.tick.value, "meter point");
        if (!tick)
            return TrackResult(Err(tick.error()));
        std::uint8_t power = 0;
        auto denominator = static_cast<std::uint32_t>(point.signature.denominator);
        while (denominator > 1) {
            denominator >>= 1u;
            ++power;
        }
        if (point.signature.numerator > 0xff)
            return TrackResult(Err(smf_error(
                SmfErrorCode::InvalidValue,
                "time-signature numerator " + decimal(point.signature.numerator) +
                    " exceeds the single-byte meta-event field")));
        if (auto failure = reserve_event())
            return TrackResult(Err(*failure));
        events.push_back(OutputEvent{
            tick.value(), 0,
            {0xffu, 0x58u, 0x04u, static_cast<std::uint8_t>(point.signature.numerator), power,
             kMetronomeClocksPerClick, kThirtySecondsPerQuarter}});
    }

    std::sort(events.begin(), events.end(), output_before);
    return TrackResult(Ok(std::move(events)));
}

runtime::Result<std::vector<OutputEvent>, SmfError> Exporter::build_note_track(const Track& track) {
    using TrackResult = runtime::Result<std::vector<OutputEvent>, SmfError>;
    std::vector<OutputEvent> events;

    if (!track.name().empty()) {
        if (track.name().size() > 0x0fff'ffffull)
            return TrackResult(Err(smf_error(SmfErrorCode::LimitExceeded,
                                             "track name exceeds the meta-event length range")));
        std::vector<std::uint8_t> bytes{0xffu, 0x03u};
        append_variable_length(bytes, static_cast<std::uint32_t>(track.name().size()));
        bytes.insert(bytes.end(), track.name().begin(), track.name().end());
        // Rank -1 keeps the name ahead of any event that also sits at tick 0.
        events.push_back(OutputEvent{0, -1, std::move(bytes)});
    }

    for (const auto& clip : track.clips()) {
        if (clip.time_anchor() != ClipTimeAnchor::Musical &&
            loss_policy_.drop_absolute_clips)
            continue;
        const auto* notes = std::get_if<MidiContent>(&clip.content());
        if (notes == nullptr) {
            if (may_drop_non_note_content(clip.content(), loss_policy_))
                continue;
            if (options_.skip_non_note_clips)
                continue;
            return TrackResult(Err(smf_error(
                SmfErrorCode::UnsupportedFeature,
                "clip " + decimal(static_cast<std::int64_t>(clip.id().value)) +
                    " holds content that is not note content")));
        }
        // A sample-anchored clip has no musical start, so its notes cannot be
        // placed on the SMF grid at all.
        if (clip.time_anchor() != ClipTimeAnchor::Musical)
            return TrackResult(Err(smf_error(
                SmfErrorCode::UnsupportedFeature,
                "clip " + decimal(static_cast<std::int64_t>(clip.id().value)) +
                    " is anchored to absolute time, which has no musical tick position")));
        if ((!notes->modifiers().empty() || notes->modifier_seed() != 0) &&
            !loss_policy_.strip_note_modifiers)
            return TrackResult(Err(smf_error(
                SmfErrorCode::UnsupportedFeature,
                "clip " + decimal(static_cast<std::int64_t>(clip.id().value)) +
                    " has per-note playback modifiers, which have no standard MIDI file "
                    "representation")));
        // A lane's points are authored independently of the notes, so writing
        // the notes alone produces a file that opens successfully with every
        // controller movement gone. The raw entry point fails closed like the
        // modifier check above; only the interchange adapter clears this, and
        // only once `clip.midi-expression-lane` has been accepted by exact id.
        if (!notes->lanes().empty() && !loss_policy_.drop_midi_expression_lanes)
            return TrackResult(Err(smf_error(
                SmfErrorCode::UnsupportedFeature,
                "clip " + decimal(static_cast<std::int64_t>(clip.id().value)) +
                    " has controller and expression lanes, which this Standard MIDI file "
                    "writer does not emit")));

        for (const auto& note : notes->notes()) {
            auto velocity = scale_velocity_16_to_7(note.velocity);
            if (velocity == 0 && loss_policy_.quantize_note_velocity)
                velocity = 1;
            if (velocity == 0)
                return TrackResult(Err(smf_error(
                    SmfErrorCode::InvalidValue,
                    "note " + decimal(static_cast<std::int64_t>(note.id.value)) +
                        " scales to velocity zero, which a standard MIDI file reads as note off")));
            const auto start_canonical = clip.start().value + note.start.value;
            auto start = to_smf_tick(start_canonical, "note start");
            if (!start)
                return TrackResult(Err(start.error()));
            auto end = to_smf_tick(start_canonical + note.duration.value, "note end");
            if (!end)
                return TrackResult(Err(end.error()));
            if (auto failure = reserve_event())
                return TrackResult(Err(*failure));
            if (auto failure = reserve_event())
                return TrackResult(Err(*failure));

            const auto status = static_cast<std::uint8_t>(note.channel & 0x0fu);
            events.push_back(OutputEvent{start.value(), 1,
                                         {static_cast<std::uint8_t>(0x90u | status), note.pitch,
                                          velocity}});
            events.push_back(OutputEvent{end.value(), 0,
                                         {static_cast<std::uint8_t>(0x80u | status), note.pitch,
                                          kNoteOffReleaseVelocity}});
        }
    }

    std::sort(events.begin(), events.end(), output_before);
    return TrackResult(Ok(std::move(events)));
}

ExportResult Exporter::run() {
    if (options_.ticks_per_quarter == 0 || (options_.ticks_per_quarter & 0x8000u) != 0)
        return Err(smf_error(SmfErrorCode::UnsupportedDivision,
                             "ticks_per_quarter " + decimal(options_.ticks_per_quarter) +
                                 " is not a metrical division in 1..32767"));
    if (project_.sequences().size() != 1 &&
        !loss_policy_.drop_non_root_sequences)
        return Err(smf_error(SmfErrorCode::UnsupportedFeature,
                             "project holds " +
                                 decimal(static_cast<std::int64_t>(project_.sequences().size())) +
                                 " sequences; only a single root sequence is exportable"));
    const auto* root = project_.find_sequence(project_.root_sequence_id());
    if (root == nullptr)
        return Err(smf_error(SmfErrorCode::UnsupportedFeature,
                             "project has no root sequence to export"));

    auto conductor = build_conductor_track();
    if (!conductor)
        return Err(conductor.error());

    std::vector<std::vector<OutputEvent>> note_tracks;
    note_tracks.reserve(root->tracks().size());
    for (const auto& track : root->tracks()) {
        auto events = build_note_track(track);
        if (!events)
            return Err(events.error());
        note_tracks.push_back(std::move(events.value()));
    }

    const auto chunk_count = note_tracks.size() + 1;
    if (chunk_count > 0xffffull)
        return Err(smf_error(SmfErrorCode::LimitExceeded,
                             "project holds " +
                                 decimal(static_cast<std::int64_t>(note_tracks.size())) +
                                 " tracks, exceeding the 16-bit track count"));

    SmfExport result{};
    auto& bytes = result.bytes;
    bytes.insert(bytes.end(), {'M', 'T', 'h', 'd'});
    append_u32(bytes, 6);
    append_u16(bytes, 1);
    append_u16(bytes, static_cast<std::uint16_t>(chunk_count));
    append_u16(bytes, options_.ticks_per_quarter);

    if (auto failure = append_track_chunk(bytes, conductor.value()))
        return Err(*failure);
    for (const auto& events : note_tracks) {
        if (auto failure = append_track_chunk(bytes, events))
            return Err(*failure);
    }

    result.exact_tick_conversion = exact_;
    result.max_tick_rounding_error = max_rounding_error_;
    return Ok(std::move(result));
}

} // namespace

ExportResult export_smf(const Project& project) {
    return export_smf(project, SmfExportOptions{});
}

ExportResult export_smf(const Project& project, const SmfExportOptions& options) {
    return detail::export_smf_with_policy(project, options, {});
}

ExportResult detail::export_smf_with_policy(const Project& project,
                                            const SmfExportOptions& options,
                                            const detail::SmfExportLossPolicy& loss_policy) {
    Exporter exporter(project, options, loss_policy);
    return exporter.run();
}

} // namespace pulp::timeline
