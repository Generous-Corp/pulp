#include <pulp/timeline/smf.hpp>

#include "smf_decode.hpp"
#include "smf_error.hpp"
#include "smf_tick_scale.hpp"

#include <pulp/interchange/capability.hpp>
#include <pulp/timebase/compiled_meter_map.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timebase/tick.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::timeline {
namespace {

using detail::SmfDecodedEvent;
using detail::SmfDecodedFile;
using detail::SmfMessage;
using detail::decimal;
using detail::smf_error;
using detail::smf_to_canonical_ticks;
using detail::TickScale;
using runtime::Err;
using runtime::Ok;

using ImportResult = runtime::Result<SmfImport, SmfError>;

using interchange::Concept;

constexpr std::array kImplementedImports{
    Concept::TrackFlat,   Concept::ClipMusical, Concept::ClipNote,
    Concept::TempoSingle, Concept::TempoMap,    Concept::MeterSingle,
    Concept::MeterMap,
};

constexpr bool implemented(Concept concept_value) noexcept {
    return std::find(kImplementedImports.begin(), kImplementedImports.end(), concept_value) !=
           kImplementedImports.end();
}

static_assert([] {
    for (std::size_t index = 0; index < interchange::kConceptCount; ++index) {
        const auto concept_value = static_cast<Concept>(index);
        if (interchange::import_supports(interchange::Format::Smf, concept_value) &&
            !implemented(concept_value))
            return false;
    }
    return true;
}(), "an SMF import row is declared supported but this reader does not implement it");

std::optional<SmfError> admit(Concept concept_value, std::string message) {
    if (interchange::import_supports(interchange::Format::Smf, concept_value))
        return std::nullopt;
    return smf_error(SmfErrorCode::UnsupportedFeature, std::move(message));
}

constexpr double kMinimumBpm = 1.0;
constexpr double kMaximumBpm = 1'000.0;
constexpr std::uint8_t kMaximumMeterDenominatorPower = 8;

// A monotonic ItemId source. Sequential allocation gives the model the
// uniqueness and next-id monotonicity it requires without threading an
// allocator result through every call site.
struct IdSource {
    std::uint64_t next = 1;
    ItemId take() noexcept {
        return ItemId{next++};
    }
};

// The timebase validates more than this importer can produce: tempo points are
// seeded at tick zero, sorted, deduplicated, and range-checked before the map
// is built, so only the numeric code is worth reporting if one still fails.
std::string timebase_error_text(std::string_view surface, int code) {
    return std::string(surface) + " were rejected by the timebase (error " + decimal(code) + ")";
}

// A meter change off a bar boundary is the one timebase rejection an SMF can
// genuinely provoke, so it is named rather than reduced to a code.
std::string meter_map_error_text(timebase::MeterMapError error) {
    if (error == timebase::MeterMapError::ChangeNotOnBarBoundary)
        return "a time-signature change that is not on a bar boundary";
    return timebase_error_text("time-signature meta-events", static_cast<int>(error));
}

// One decoded event tagged with the track it came from, so a merge across
// tracks can keep a deterministic order for simultaneous meta-events.
struct MergedEvent {
    std::int64_t canonical_tick = 0;
    std::size_t track_index = 0;
    std::uint32_t order = 0;
    const SmfDecodedEvent* event = nullptr;
};

bool merged_before(const MergedEvent& lhs, const MergedEvent& rhs) noexcept {
    return std::tuple(lhs.canonical_tick, lhs.track_index, lhs.order) <
           std::tuple(rhs.canonical_tick, rhs.track_index, rhs.order);
}

// A Note On awaiting its Note Off. Note Ons for one (channel, pitch) pair are
// matched first-in-first-out, the only ordering the format defines for stacked
// identical notes.
struct PendingNote {
    std::int64_t canonical_tick = 0;
    std::uint8_t velocity = 0;
};

// Scale a 7-bit MIDI velocity to the model's 16-bit domain. This is the MIDI
// 2.0 min/center/max scaling, matching pulp::midi::scale_7_to_16; the interop
// module keeps its own copy rather than linking pulp::midi, whose headers pull
// the exception-using CHOC MIDI surface into this -fno-exceptions target.
constexpr std::uint16_t scale_velocity_7_to_16(std::uint8_t velocity_7) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint32_t>(velocity_7) * 0xffffu + 63u) /
                                      127u);
}

class Importer {
  public:
    Importer(const SmfDecodedFile& file, const SmfImportOptions& options)
        : file_(file), options_(options), scale_(TickScale::create(file.division)) {}

    ImportResult run();

  private:
    std::optional<SmfError> collect_maps();
    runtime::Result<std::optional<Clip>, SmfError> build_track_clip(std::size_t track_index);

    const SmfDecodedFile& file_;
    const SmfImportOptions& options_;
    TickScale scale_;
    IdSource ids_{};
    timebase::TempoMap tempo_map_{};
    timebase::MeterMap meter_map_{};
    std::int64_t max_rounding_error_ = 0;
};

std::optional<SmfError> Importer::collect_maps() {
    std::vector<MergedEvent> tempo_events;
    std::vector<MergedEvent> meter_events;
    for (std::size_t track_index = 0; track_index < file_.tracks.size(); ++track_index) {
        for (const auto& event : file_.tracks[track_index].events) {
            if (event.message != SmfMessage::Tempo && event.message != SmfMessage::TimeSignature)
                continue;
            auto* destination =
                event.message == SmfMessage::Tempo ? &tempo_events : &meter_events;
            const auto limit = event.message == SmfMessage::Tempo
                                   ? options_.limits.max_tempo_points
                                   : options_.limits.max_meter_points;
            if (destination->size() >= limit)
                return smf_error(SmfErrorCode::LimitExceeded,
                                 event.message == SmfMessage::Tempo
                                     ? "tempo points exceed max_tempo_points"
                                     : "meter points exceed max_meter_points");
            const auto canonical = smf_to_canonical_ticks(event.tick, scale_);
            if (!canonical)
                return smf_error(SmfErrorCode::TickRangeExceeded,
                                 "tick " + decimal(event.tick) +
                                     " does not fit the canonical tick domain");
            destination->push_back(MergedEvent{*canonical, track_index, event.order, &event});
        }
    }
    std::sort(tempo_events.begin(), tempo_events.end(), merged_before);
    std::sort(meter_events.begin(), meter_events.end(), merged_before);

    // The specification's defaults when a file states nothing: 120 bpm and 4/4.
    std::vector<timebase::TempoPoint> tempo_points{
        {timebase::TickPosition{0}, 120.0, timebase::TempoCurve::Constant}};
    for (const auto& merged : tempo_events) {
        const auto microseconds = merged.event->microseconds_per_quarter;
        if (microseconds == 0)
            return smf_error(SmfErrorCode::InvalidValue,
                             "set-tempo event declares zero microseconds per quarter note");
        const auto bpm = 60'000'000.0 / static_cast<double>(microseconds);
        if (bpm < kMinimumBpm || bpm > kMaximumBpm)
            return smf_error(SmfErrorCode::InvalidValue,
                             "tempo of " + decimal(static_cast<std::int64_t>(bpm)) +
                                 " bpm is outside the representable range");
        // Later events at one tick win: the file's own order is the only
        // tie-break the format defines.
        if (tempo_points.back().tick.value == merged.canonical_tick)
            tempo_points.back().bpm = bpm;
        else
            tempo_points.push_back(
                {timebase::TickPosition{merged.canonical_tick}, bpm,
                 timebase::TempoCurve::Constant});
    }

    std::vector<timebase::MeterPoint> meter_points{
        {timebase::TickPosition{0}, timebase::MeterSignature{4, 4}}};
    for (const auto& merged : meter_events) {
        const auto numerator = merged.event->meter_numerator;
        const auto power = merged.event->meter_denominator_power;
        if (numerator == 0 || power > kMaximumMeterDenominatorPower)
            return smf_error(SmfErrorCode::InvalidValue,
                             "time signature " + decimal(numerator) + "/2^" + decimal(power) +
                                 " is not representable");
        const timebase::MeterSignature signature{static_cast<std::int32_t>(numerator),
                                                 static_cast<std::int32_t>(1) << power};
        if (meter_points.back().tick.value == merged.canonical_tick)
            meter_points.back().signature = signature;
        else
            meter_points.push_back({timebase::TickPosition{merged.canonical_tick}, signature});
    }

    if (auto failure = admit(tempo_points.size() > 1 ? Concept::TempoMap : Concept::TempoSingle,
                             "the SMF capability table refuses the decoded tempo map"))
        return failure;
    if (auto failure = admit(meter_points.size() > 1 ? Concept::MeterMap : Concept::MeterSingle,
                             "the SMF capability table refuses the decoded meter map"))
        return failure;

    auto tempo_map = timebase::TempoMap::create(tempo_points);
    if (!tempo_map)
        return smf_error(SmfErrorCode::TempoMapRejected,
                         timebase_error_text("tempo meta-events",
                                             static_cast<int>(tempo_map.error())));
    tempo_map_ = std::move(tempo_map.value());

    auto meter_map = timebase::MeterMap::create(meter_points);
    if (!meter_map)
        return smf_error(SmfErrorCode::MeterMapRejected,
                         "time-signature meta-events produced " +
                             meter_map_error_text(meter_map.error()));
    meter_map_ = std::move(meter_map.value());
    return std::nullopt;
}

runtime::Result<std::optional<Clip>, SmfError> Importer::build_track_clip(std::size_t index) {
    using ClipResult = runtime::Result<std::optional<Clip>, SmfError>;
    const auto& track = file_.tracks[index];

    // A deque per (channel, pitch) so first-in-first-out matching stays O(1)
    // even when untrusted input stacks the limit's worth of identical notes.
    std::unordered_map<std::uint16_t, std::deque<PendingNote>> open_notes;
    std::size_t open_count = 0;
    std::vector<NoteEvent> notes;
    std::int64_t first_start = std::numeric_limits<std::int64_t>::max();
    std::int64_t last_end = std::numeric_limits<std::int64_t>::min();

    for (const auto& event : track.events) {
        if (event.message != SmfMessage::NoteOn && event.message != SmfMessage::NoteOff)
            continue;
        const auto canonical = smf_to_canonical_ticks(event.tick, scale_);
        if (!canonical)
            return ClipResult(Err(smf_error(SmfErrorCode::TickRangeExceeded,
                                            "note tick " + decimal(event.tick) +
                                                " does not fit the canonical tick domain")));
        const auto key = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(event.channel) << 8u) | event.pitch);

        if (event.message == SmfMessage::NoteOn) {
            if (open_count >= options_.limits.max_concurrent_notes)
                return ClipResult(Err(smf_error(
                    SmfErrorCode::LimitExceeded,
                    "sounding notes exceed max_concurrent_notes (" +
                        decimal(static_cast<std::int64_t>(options_.limits.max_concurrent_notes)) +
                        ")")));
            open_notes[key].push_back(PendingNote{*canonical, event.velocity});
            ++open_count;
            continue;
        }

        const auto found = open_notes.find(key);
        if (found == open_notes.end() || found->second.empty())
            return ClipResult(Err(smf_error(
                SmfErrorCode::UnbalancedNote,
                "note off for channel " + decimal(event.channel) + " pitch " +
                    decimal(event.pitch) + " at tick " + decimal(event.tick) +
                    " has no matching note on")));
        const auto pending = found->second.front();
        found->second.pop_front();
        --open_count;

        const auto duration = *canonical - pending.canonical_tick;
        if (duration <= 0)
            return ClipResult(Err(smf_error(
                SmfErrorCode::InvalidValue,
                "note on channel " + decimal(event.channel) + " pitch " + decimal(event.pitch) +
                    " ending at tick " + decimal(event.tick) + " has no positive duration")));
        if (notes.size() >= options_.limits.max_notes)
            return ClipResult(Err(smf_error(
                SmfErrorCode::LimitExceeded,
                "note count exceeds max_notes (" +
                    decimal(static_cast<std::int64_t>(options_.limits.max_notes)) + ")")));

        NoteEvent note{};
        note.start = timebase::TickPosition{pending.canonical_tick};
        note.duration = timebase::TickDuration{duration};
        note.velocity = scale_velocity_7_to_16(pending.velocity);
        note.pitch = event.pitch;
        note.channel = event.channel;
        notes.push_back(note);
        first_start = std::min(first_start, pending.canonical_tick);
        last_end = std::max(last_end, *canonical);
    }

    if (open_count != 0)
        return ClipResult(Err(smf_error(SmfErrorCode::UnbalancedNote,
                                        decimal(static_cast<std::int64_t>(open_count)) +
                                            " notes are still sounding at the end of the track")));
    if (notes.empty())
        return ClipResult(Ok(std::optional<Clip>{}));

    if (auto failure = admit(Concept::ClipNote,
                             "the SMF capability table refuses decoded note content"))
        return ClipResult(Err(*failure));
    if (auto failure = admit(Concept::ClipMusical,
                             "the SMF capability table refuses musical clip placement"))
        return ClipResult(Err(*failure));

    // Notes are stored relative to their clip, so the clip spans exactly the
    // track's sounding range and every note start stays inside it.
    for (auto& note : notes) {
        note.id = ids_.take();
        note.start = timebase::TickPosition{note.start.value - first_start};
    }
    auto content = NoteContent::create(std::move(notes));
    if (!content) {
        SmfError error = smf_error(SmfErrorCode::ModelRejected, "note content was rejected");
        error.model_error = content.error();
        return ClipResult(Err(std::move(error)));
    }
    auto clip = Clip::create(ids_.take(), timebase::TickPosition{first_start},
                             timebase::TickDuration{last_end - first_start},
                             std::move(content.value()));
    if (!clip) {
        SmfError error = smf_error(SmfErrorCode::ModelRejected,
                                   "track " + decimal(static_cast<std::int64_t>(index)) +
                                       " clip was rejected");
        error.model_error = clip.error();
        return ClipResult(Err(std::move(error)));
    }
    return ClipResult(Ok(std::optional<Clip>{std::move(clip.value())}));
}

ImportResult Importer::run() {
    if (!scale_.exact) {
        // The coarser grid cannot tile the canonical one; every position rounds
        // to the nearest canonical tick, which is at most half a tick away.
        max_rounding_error_ = 1;
    }
    if (auto failure = collect_maps())
        return Err(*failure);

    const auto project_id = ids_.take();
    const auto sequence_id = ids_.take();

    std::vector<Track> tracks;
    tracks.reserve(file_.tracks.size());
    for (std::size_t index = 0; index < file_.tracks.size(); ++index) {
        auto clip = build_track_clip(index);
        if (!clip)
            return Err(clip.error());
        const auto& name = file_.tracks[index].name;
        // A chunk with neither notes nor a name carries no track-level content:
        // its tempo and time-signature events already live in the maps. Not
        // reconstructing it is what keeps import(export(project)) stable, since
        // export always writes such a chunk as the conductor track.
        if (!clip.value() && name.empty())
            continue;
        if (auto failure = admit(Concept::TrackFlat,
                                 "the SMF capability table refuses flat tracks"))
            return Err(*failure);
        std::vector<Clip> clips;
        if (clip.value())
            clips.push_back(std::move(*clip.value()));
        auto track = Track::create(ids_.take(), name, std::move(clips));
        if (!track) {
            SmfError error = smf_error(SmfErrorCode::ModelRejected,
                                       "track " + decimal(static_cast<std::int64_t>(index)) +
                                           " was rejected");
            error.model_error = track.error();
            return Err(std::move(error));
        }
        tracks.push_back(std::move(track.value()));
    }

    auto sequence = Sequence::create(sequence_id, "Arrangement", std::nullopt, std::move(tracks));
    if (!sequence) {
        SmfError error = smf_error(SmfErrorCode::ModelRejected, "root sequence was rejected");
        error.model_error = sequence.error();
        return Err(std::move(error));
    }

    ProjectInput input{};
    input.id = project_id;
    input.next_item_id = ids_.next;
    input.root_sequence_id = sequence_id;
    input.sequences.push_back(std::move(sequence.value()));
    input.tempo_map = tempo_map_;
    input.meter_map = meter_map_;

    auto project = Project::create(std::move(input));
    if (!project) {
        SmfError error = smf_error(SmfErrorCode::ModelRejected, "project was rejected");
        error.model_error = project.error();
        return Err(std::move(error));
    }

    SmfImport imported{std::move(project.value()), file_.division, scale_.exact,
                       max_rounding_error_};
    return Ok(std::move(imported));
}

} // namespace

ImportResult import_smf(std::span<const std::uint8_t> file_bytes) {
    return import_smf(file_bytes, SmfImportOptions{});
}

ImportResult import_smf(std::span<const std::uint8_t> file_bytes,
                        const SmfImportOptions& options) {
    auto decoded = detail::decode_smf(file_bytes, options);
    if (!decoded)
        return Err(decoded.error());
    Importer importer(decoded.value(), options);
    return importer.run();
}

} // namespace pulp::timeline
