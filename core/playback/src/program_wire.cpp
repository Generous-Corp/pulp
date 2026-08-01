#include <pulp/playback/program_wire.hpp>

#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/automation_program.hpp>
#include <pulp/playback/track_automation_program.hpp>
#include <pulp/timeline/automation_lane.hpp>
#include <pulp/timeline/note_modifier.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <variant>

namespace pulp::playback {

namespace {

using Error = ProgramWireError;
using Code = ProgramWireErrorCode;
using SizeResult = runtime::Result<std::size_t, ProgramWireError>;
using ViewResult = runtime::Result<ProgramWireView, ProgramWireError>;

constexpr std::uint32_t section_id(ProgramWireSection section) noexcept {
    return static_cast<std::uint32_t>(section);
}

/// The known sections, in the ascending id order a canonical payload lays them
/// out in. Indexing this array is also how the decoder decides an id is known.
constexpr std::array<ProgramWireSection, 9> kSections{
    ProgramWireSection::Program,
    ProgramWireSection::TempoPoints,
    ProgramWireSection::Tracks,
    ProgramWireSection::ClipIds,
    ProgramWireSection::NoteEvents,
    ProgramWireSection::NoteModifiers,
    ProgramWireSection::DevicePlacementIds,
    ProgramWireSection::AutomationLanes,
    ProgramWireSection::AutomationSegments,
};

constexpr std::size_t kSectionCount = kSections.size();

/// Byte size of one record in each known section, in kSections order.
constexpr std::array<std::size_t, kSectionCount> kRecordSizes{
    sizeof(ProgramWireProgramRecord),
    sizeof(ProgramWireTempoPointRecord),
    sizeof(ProgramWireTrackRecord),
    sizeof(ProgramWireIdRecord),
    sizeof(ProgramWireNoteEventRecord),
    sizeof(ProgramWireNoteModifierRecord),
    sizeof(ProgramWireIdRecord),
    sizeof(ProgramWireAutomationLaneRecord),
    sizeof(ProgramWireAutomationSegmentRecord),
};

/// Where the record counts land while the encoder measures a program, in
/// kSections order, so measuring and writing walk one layout rather than two.
struct WireCounts {
    std::array<std::size_t, kSectionCount> records{};

    std::size_t& operator[](ProgramWireSection section) noexcept {
        return records[index_of(section)];
    }
    std::size_t operator[](ProgramWireSection section) const noexcept {
        return records[index_of(section)];
    }

    static constexpr std::size_t index_of(ProgramWireSection section) noexcept {
        for (std::size_t i = 0; i < kSectionCount; ++i)
            if (kSections[i] == section)
                return i;
        return 0;
    }
};

struct WireLayout {
    WireCounts counts;
    std::array<std::size_t, kSectionCount> offsets{};
    std::array<std::size_t, kSectionCount> bytes{};
    std::size_t directory_bytes = 0;
    std::size_t payload_bytes = 0;
    std::size_t total_bytes = 0;
};

WireLayout lay_out(const WireCounts& counts) noexcept {
    WireLayout layout;
    layout.counts = counts;
    layout.directory_bytes = kSectionCount * sizeof(ProgramWireSectionEntry);
    std::size_t offset = 0;
    for (std::size_t i = 0; i < kSectionCount; ++i) {
        layout.offsets[i] = offset;
        layout.bytes[i] = counts.records[i] * kRecordSizes[i];
        offset += layout.bytes[i];
    }
    layout.payload_bytes = offset;
    layout.total_bytes = sizeof(ProgramWireHeader) + layout.directory_bytes + layout.payload_bytes;
    return layout;
}

constexpr bool fits_u32(std::size_t value) noexcept {
    return value <= std::numeric_limits<std::uint32_t>::max();
}

/// Measures the program, refusing anything this version cannot represent. The
/// refusals live here rather than in the write pass so a caller that only sizes
/// a buffer still learns the program is unrepresentable.
runtime::Result<WireCounts, ProgramWireError>
measure(const PlaybackProgram& program,
        std::span<const timebase::TempoPoint> tempo_points) noexcept {
    using Measured = runtime::Result<WireCounts, ProgramWireError>;

    if (tempo_points.empty() || !program.tempo_map().matches(tempo_points))
        return Measured(runtime::Err(Error{Code::TempoMapMismatch, 0, tempo_points.size()}));
    if (!program.tempo_map().sample_rate().valid())
        return Measured(runtime::Err(Error{Code::InvalidSampleRate}));

    WireCounts counts;
    counts[ProgramWireSection::Program] = 1;
    counts[ProgramWireSection::TempoPoints] = tempo_points.size();
    counts[ProgramWireSection::Tracks] = program.tracks().size();

    for (const auto& owner : program.tracks()) {
        const auto& track = *owner;
        if (track.audio_program() != nullptr)
            return Measured(
                runtime::Err(Error{Code::AudioProgramUnsupported,
                                   section_id(ProgramWireSection::Tracks), track.id().value}));
        const auto& production = track.arrangement_production();
        if (production.mode != timeline::ProductionMode::Synchronous ||
            production.reproducibility != timeline::ReproducibilityClass::Deterministic ||
            production.lookahead_ms != 0)
            return Measured(
                runtime::Err(Error{Code::ProductionDeclarationUnsupported,
                                   section_id(ProgramWireSection::Tracks), track.id().value}));

        counts[ProgramWireSection::ClipIds] += track.ordered_clip_ids().size();
        counts[ProgramWireSection::NoteEvents] += track.arrangement_note_events().size();
        counts[ProgramWireSection::NoteModifiers] += track.note_modifiers().size();
        counts[ProgramWireSection::DevicePlacementIds] +=
            track.ordered_device_placement_ids().size();

        const auto* automation = track.automation_program();
        if (!automation)
            continue;
        counts[ProgramWireSection::AutomationLanes] += automation->programs().size();
        for (const auto& lane : automation->programs())
            counts[ProgramWireSection::AutomationSegments] += lane->segments().size();
    }

    for (std::size_t i = 0; i < kSectionCount; ++i)
        if (!fits_u32(counts.records[i]))
            return Measured(runtime::Err(
                Error{Code::CountOverflow, section_id(kSections[i]), counts.records[i]}));
    return Measured(runtime::Ok(counts));
}

/// The index of `target` within `lanes`, or kProgramWireNoLane when absent.
/// TrackMixerProgram's lane pointers borrow from the TrackAutomationProgram the
/// owning track holds alive, so identity is the correct lookup here — and a
/// pointer that is not one of them is a program the wire must refuse rather
/// than silently re-point.
std::uint32_t lane_index(std::span<const std::shared_ptr<const AutomationProgram>> lanes,
                         const AutomationProgram* target) noexcept {
    if (!target)
        return kProgramWireNoLane;
    for (std::size_t i = 0; i < lanes.size(); ++i)
        if (lanes[i].get() == target)
            return static_cast<std::uint32_t>(i);
    return kProgramWireNoLane;
}

class Writer {
  public:
    explicit Writer(std::span<std::byte> out) noexcept : out_(out) {}

    template <typename Record> void put(const Record& record, std::size_t at) noexcept {
        std::memcpy(out_.data() + at, &record, sizeof(Record));
    }

  private:
    std::span<std::byte> out_;
};

ProgramWireProgramRecord program_record(const PlaybackProgram& program,
                                        std::uint64_t producer_epoch) noexcept {
    const auto rate = program.tempo_map().sample_rate();
    const auto& limits = program.automation_limits();
    return {
        .producer_epoch = producer_epoch,
        .generation = program.generation(),
        .document_revision = program.document_revision(),
        .project_id = program.project_id().value,
        .sequence_id = program.sequence_id().value,
        .generated_id_base = program.generated_id_base(),
        .sample_rate_numerator = rate.numerator,
        .sample_rate_denominator = rate.denominator,
        .max_device_placements_per_track = limits.max_device_placements_per_track,
        .max_lanes_per_track = limits.max_lanes_per_track,
        .max_points_per_lane = limits.max_points_per_lane,
        .max_points_per_track = limits.max_points_per_track,
        .max_intersecting_segments_per_block = limits.max_intersecting_segments_per_block,
        .max_events_per_device_per_block = limits.max_events_per_device_per_block,
    };
}

ProgramWireAutomationLaneRecord lane_record(const AutomationProgram& lane,
                                            std::uint32_t segment_first) noexcept {
    ProgramWireAutomationLaneRecord record;
    record.lane_id = lane.lane_id().value;
    record.generation = lane.generation();
    record.segment_first = segment_first;
    record.segment_count = static_cast<std::uint32_t>(lane.segments().size());
    record.leading_value = lane.leading_value();
    record.evaluation_rate = static_cast<std::uint8_t>(AutomationEvaluationRate::SampleAccurate);
    std::visit(timeline::AutomationTargetCases{
                   [&](const timeline::DeviceParameterTarget& target) {
                       record.target_kind =
                           static_cast<std::uint8_t>(ProgramWireTargetKind::DeviceParameter);
                       record.device_placement_id = target.device_placement_id.value;
                       record.device_param_id = target.param_id;
                   },
                   [&](const timeline::TrackMixerTarget& target) {
                       record.target_kind =
                           static_cast<std::uint8_t>(ProgramWireTargetKind::TrackMixer);
                       record.mixer_parameter = static_cast<std::uint8_t>(target.parameter);
                   },
               },
               lane.target());
    return record;
}

} // namespace

namespace detail {

/// Holds the private accessors the decoder needs to populate a view without
/// making the view's borrowed spans writable to anyone else.
struct ProgramWireDecoder {
    static ViewResult decode(std::span<const std::byte> bytes) noexcept;

    template <typename Record>
    static std::span<const Record> records(std::span<const std::byte> payload, std::size_t offset,
                                           std::size_t bytes) noexcept {
        return {reinterpret_cast<const Record*>(payload.data() + offset), bytes / sizeof(Record)};
    }
};

} // namespace detail

AutomationPlaybackLimits ProgramWireView::automation_limits() const noexcept {
    return {
        .max_device_placements_per_track = program_->max_device_placements_per_track,
        .max_lanes_per_track = program_->max_lanes_per_track,
        .max_points_per_lane = program_->max_points_per_lane,
        .max_points_per_track = program_->max_points_per_track,
        .max_intersecting_segments_per_block = program_->max_intersecting_segments_per_block,
        .max_events_per_device_per_block = program_->max_events_per_device_per_block,
    };
}

std::uint64_t program_wire_process_epoch() noexcept {
    // Seeded once from a source that differs between processes started in the
    // same second, then held for the process lifetime. A collision would make
    // two producers look like one, so the address entropy is mixed in rather
    // than trusting the clock alone; forced nonzero because zero is the
    // format's "absent" value.
    static const std::uint64_t epoch = [] {
        const auto now =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto wall =
            static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
        std::uint64_t mixed = now ^ (wall * 0x9E37'79B9'7F4A'7C15ull);
        static int anchor = 0;
        mixed ^= reinterpret_cast<std::uintptr_t>(&anchor) * 0xBF58'476D'1CE4'E5B9ull;
        mixed ^= mixed >> 31;
        mixed *= 0x94D0'49BB'1331'11EBull;
        mixed ^= mixed >> 29;
        return mixed == 0 ? 1ull : mixed;
    }();
    return epoch;
}

SizeResult program_wire_encoded_size(const PlaybackProgram& program,
                                     std::span<const timebase::TempoPoint> tempo_points) noexcept {
    auto counts = measure(program, tempo_points);
    if (!counts)
        return SizeResult(runtime::Err(std::move(counts).error()));
    return SizeResult(runtime::Ok(lay_out(std::move(counts).value()).total_bytes));
}

SizeResult encode_program_wire(const PlaybackProgram& program,
                               std::span<const timebase::TempoPoint> tempo_points,
                               std::uint64_t producer_epoch, std::span<std::byte> out) noexcept {
    // Refused before measuring, so a caller that forgot producer identity is
    // told that rather than being handed a payload every consumer would
    // mistake for every other producer's.
    if (producer_epoch == 0)
        return SizeResult(runtime::Err(Error{Code::InvalidProducerEpoch}));
    auto measured = measure(program, tempo_points);
    if (!measured)
        return SizeResult(runtime::Err(std::move(measured).error()));
    const auto layout = lay_out(std::move(measured).value());

    if (out.size() < layout.total_bytes)
        return SizeResult(runtime::Err(Error{Code::InsufficientCapacity, 0, layout.total_bytes}));
    if (reinterpret_cast<std::uintptr_t>(out.data()) % kProgramWireAlignment != 0)
        return SizeResult(runtime::Err(
            Error{Code::MisalignedBuffer, 0,
                  static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(out.data()))}));

    Writer writer(out);
    const std::size_t directory_at = sizeof(ProgramWireHeader);
    const std::size_t payload_at = directory_at + layout.directory_bytes;

    for (std::size_t i = 0; i < kSectionCount; ++i) {
        ProgramWireSectionEntry entry;
        entry.id = section_id(kSections[i]);
        entry.flags = 0;
        entry.offset = layout.offsets[i];
        entry.bytes = layout.bytes[i];
        writer.put(entry, directory_at + i * sizeof(ProgramWireSectionEntry));
    }

    const auto at = [&](ProgramWireSection section, std::size_t record) {
        const auto index = WireCounts::index_of(section);
        return payload_at + layout.offsets[index] + record * kRecordSizes[index];
    };

    writer.put(program_record(program, producer_epoch), at(ProgramWireSection::Program, 0));

    for (std::size_t i = 0; i < tempo_points.size(); ++i) {
        const auto& point = tempo_points[i];
        ProgramWireTempoPointRecord record;
        record.tick = point.tick.value;
        record.bpm_bits = std::bit_cast<std::uint64_t>(point.bpm);
        record.curve_to_next = static_cast<std::uint8_t>(point.curve_to_next);
        writer.put(record, at(ProgramWireSection::TempoPoints, i));
    }

    std::size_t clip_cursor = 0;
    std::size_t note_cursor = 0;
    std::size_t modifier_cursor = 0;
    std::size_t placement_cursor = 0;
    std::size_t lane_cursor = 0;
    std::size_t segment_cursor = 0;

    const auto tracks = program.tracks();
    for (std::size_t t = 0; t < tracks.size(); ++t) {
        const auto& track = *tracks[t];
        const auto* automation = track.automation_program();
        const auto lanes = automation ? automation->programs()
                                      : std::span<const std::shared_ptr<const AutomationProgram>>{};

        const auto& mixer = track.mixer();
        const auto gain_lane = lane_index(lanes, mixer.gain_automation);
        const auto pan_lane = lane_index(lanes, mixer.pan_automation);
        if ((mixer.gain_automation && gain_lane == kProgramWireNoLane) ||
            (mixer.pan_automation && pan_lane == kProgramWireNoLane))
            return SizeResult(
                runtime::Err(Error{Code::MixerAutomationUnresolved,
                                   section_id(ProgramWireSection::Tracks), track.id().value}));

        ProgramWireTrackRecord record;
        record.id = track.id().value;
        record.generation = track.generation();
        record.clip_first = static_cast<std::uint32_t>(clip_cursor);
        record.clip_count = static_cast<std::uint32_t>(track.ordered_clip_ids().size());
        record.note_event_first = static_cast<std::uint32_t>(note_cursor);
        record.note_event_count =
            static_cast<std::uint32_t>(track.arrangement_note_events().size());
        record.note_modifier_first = static_cast<std::uint32_t>(modifier_cursor);
        record.note_modifier_count = static_cast<std::uint32_t>(track.note_modifiers().size());
        record.device_placement_first = static_cast<std::uint32_t>(placement_cursor);
        record.device_placement_count =
            static_cast<std::uint32_t>(track.ordered_device_placement_ids().size());
        record.automation_lane_first = static_cast<std::uint32_t>(lane_cursor);
        record.automation_lane_count = static_cast<std::uint32_t>(lanes.size());
        record.expanded_clip_count = track.expanded_clip_count();
        record.expanded_note_event_count = track.expanded_note_event_count();
        record.generated_id_start = track.generated_id_start();
        record.generated_id_count = track.generated_id_count();
        record.mixer_gain_linear = mixer.gain_linear;
        record.mixer_pan = mixer.pan;
        record.mixer_gain_lane = gain_lane;
        record.mixer_pan_lane = pan_lane;
        record.provider_selected = static_cast<std::uint8_t>(track.provider().selected);
        record.provider_available_mask = track.provider().available_mask;
        record.state_policy = static_cast<std::uint8_t>(track.state_policy());
        record.flags = automation ? kProgramWireTrackHasAutomationProgram : 0u;
        writer.put(record, at(ProgramWireSection::Tracks, t));

        for (const auto id : track.ordered_clip_ids())
            writer.put(ProgramWireIdRecord{id.value},
                       at(ProgramWireSection::ClipIds, clip_cursor++));
        for (const auto& event : track.arrangement_note_events()) {
            ProgramWireNoteEventRecord note;
            note.sample = event.sample.value;
            note.tick = event.tick.value;
            note.clip_id = event.clip_id.value;
            note.note_id = event.note_id.value;
            note.velocity = event.velocity;
            note.pitch = event.pitch;
            note.channel = event.channel;
            note.kind = static_cast<std::uint8_t>(event.kind);
            writer.put(note, at(ProgramWireSection::NoteEvents, note_cursor++));
        }
        for (const auto& compiled : track.note_modifiers()) {
            ProgramWireNoteModifierRecord modifier;
            modifier.draw_key = compiled.draw_key;
            modifier.note_id = compiled.modifier.note_id.value;
            modifier.probability = compiled.modifier.probability;
            modifier.condition_period = compiled.modifier.condition_period;
            modifier.condition_offset = compiled.modifier.condition_offset;
            modifier.ratchet_count = compiled.modifier.ratchet_count;
            modifier.condition = static_cast<std::uint8_t>(compiled.modifier.condition);
            writer.put(modifier, at(ProgramWireSection::NoteModifiers, modifier_cursor++));
        }
        for (const auto id : track.ordered_device_placement_ids())
            writer.put(ProgramWireIdRecord{id.value},
                       at(ProgramWireSection::DevicePlacementIds, placement_cursor++));

        for (const auto& lane : lanes) {
            writer.put(lane_record(*lane, static_cast<std::uint32_t>(segment_cursor)),
                       at(ProgramWireSection::AutomationLanes, lane_cursor++));
            for (const auto& segment : lane->segments()) {
                ProgramWireAutomationSegmentRecord record_out;
                record_out.start_tick = segment.start_tick.value;
                record_out.end_tick = segment.end_tick.value;
                record_out.start_sample = segment.start_sample.value;
                record_out.end_sample = segment.end_sample.value;
                record_out.start_value = segment.start_value;
                record_out.end_value = segment.end_value;
                record_out.curvature = segment.curvature;
                record_out.interpolation = static_cast<std::uint8_t>(segment.interpolation);
                writer.put(record_out,
                           at(ProgramWireSection::AutomationSegments, segment_cursor++));
            }
        }
    }

    ProgramWireHeader header;
    header.header_bytes = static_cast<std::uint32_t>(sizeof(ProgramWireHeader));
    header.section_count = static_cast<std::uint32_t>(kSectionCount);
    header.payload_bytes = layout.payload_bytes;
    header.body_checksum = program_wire_checksum(
        out.subspan(directory_at, layout.directory_bytes + layout.payload_bytes));
    writer.put(header, 0);

    return SizeResult(runtime::Ok(layout.total_bytes));
}

namespace detail {

ViewResult ProgramWireDecoder::decode(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < sizeof(ProgramWireHeader))
        return ViewResult(runtime::Err(Error{Code::ShortBuffer, 0, bytes.size()}));
    if (reinterpret_cast<std::uintptr_t>(bytes.data()) % kProgramWireAlignment != 0)
        return ViewResult(runtime::Err(
            Error{Code::MisalignedBuffer, 0,
                  static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(bytes.data()))}));

    ProgramWireHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kProgramWireMagic)
        return ViewResult(runtime::Err(Error{Code::BadMagic, 0, header.magic}));
    if (header.min_reader_version > kProgramWireVersion)
        return ViewResult(
            runtime::Err(Error{Code::UnsupportedVersion, 0, header.min_reader_version}));
    if (header.header_bytes != sizeof(ProgramWireHeader))
        return ViewResult(runtime::Err(Error{Code::BadHeaderSize, 0, header.header_bytes}));

    const std::uint64_t directory_bytes =
        static_cast<std::uint64_t>(header.section_count) * sizeof(ProgramWireSectionEntry);
    // Each declared length is compared against what the buffer has left rather
    // than summed first, so a header claiming a payload near the 64-bit ceiling
    // cannot wrap the accumulator into a small, passing total.
    if (header.payload_bytes > bytes.size() ||
        directory_bytes > bytes.size() - header.payload_bytes)
        return ViewResult(runtime::Err(Error{Code::ShortBuffer, 0, header.payload_bytes}));
    const std::uint64_t declared =
        sizeof(ProgramWireHeader) + directory_bytes + header.payload_bytes;
    if (declared > bytes.size())
        return ViewResult(runtime::Err(Error{Code::ShortBuffer, 0, declared}));
    // A payload the buffer merely contains is not a payload the buffer is: a
    // trailing region would ride along unhashed and unaccounted for.
    if (declared < bytes.size())
        return ViewResult(runtime::Err(Error{Code::TrailingData, 0, declared}));

    const auto body =
        bytes.subspan(sizeof(ProgramWireHeader),
                      static_cast<std::size_t>(directory_bytes + header.payload_bytes));
    if (program_wire_checksum(body) != header.body_checksum)
        return ViewResult(runtime::Err(Error{Code::ChecksumMismatch, 0, header.body_checksum}));

    const auto payload = body.subspan(static_cast<std::size_t>(directory_bytes));

    std::array<bool, kSectionCount> seen{};
    std::array<std::size_t, kSectionCount> offsets{};
    std::array<std::size_t, kSectionCount> lengths{};
    std::uint64_t tiled = 0;

    for (std::uint32_t i = 0; i < header.section_count; ++i) {
        ProgramWireSectionEntry entry;
        std::memcpy(&entry,
                    bytes.data() + sizeof(ProgramWireHeader) + i * sizeof(ProgramWireSectionEntry),
                    sizeof(entry));
        if (entry.bytes > header.payload_bytes || entry.offset > header.payload_bytes - entry.bytes)
            return ViewResult(
                runtime::Err(Error{Code::SectionOutOfBounds, entry.id, entry.offset}));
        if (entry.offset != tiled)
            return ViewResult(runtime::Err(Error{Code::SectionsNotTiled, entry.id, entry.offset}));
        if (entry.offset % kProgramWireAlignment != 0)
            return ViewResult(runtime::Err(Error{Code::SectionMisaligned, entry.id, entry.offset}));
        tiled = entry.offset + entry.bytes;

        std::size_t known = kSectionCount;
        for (std::size_t k = 0; k < kSectionCount; ++k)
            if (section_id(kSections[k]) == entry.id)
                known = k;
        if (known == kSectionCount) {
            if ((entry.flags & kProgramWireSectionOptional) == 0)
                return ViewResult(runtime::Err(Error{Code::UnknownSection, entry.id, i}));
            continue;
        }
        if (seen[known])
            return ViewResult(runtime::Err(Error{Code::DuplicateSection, entry.id, i}));
        if (entry.bytes % kRecordSizes[known] != 0)
            return ViewResult(runtime::Err(Error{Code::BadRecordSize, entry.id, entry.bytes}));
        seen[known] = true;
        offsets[known] = static_cast<std::size_t>(entry.offset);
        lengths[known] = static_cast<std::size_t>(entry.bytes);
    }
    if (tiled != header.payload_bytes)
        return ViewResult(runtime::Err(Error{Code::SectionsNotTiled, 0, tiled}));
    for (std::size_t k = 0; k < kSectionCount; ++k)
        if (!seen[k])
            return ViewResult(runtime::Err(Error{Code::MissingSection, section_id(kSections[k])}));

    const auto count_of = [&](ProgramWireSection section) {
        const auto index = WireCounts::index_of(section);
        return lengths[index] / kRecordSizes[index];
    };
    if (count_of(ProgramWireSection::Program) != 1)
        return ViewResult(
            runtime::Err(Error{Code::BadSectionCardinality, section_id(ProgramWireSection::Program),
                               count_of(ProgramWireSection::Program)}));
    if (count_of(ProgramWireSection::TempoPoints) == 0)
        return ViewResult(runtime::Err(
            Error{Code::BadSectionCardinality, section_id(ProgramWireSection::TempoPoints), 0}));

    ProgramWireView view;
    view.bytes_ = bytes;
    view.header_ = reinterpret_cast<const ProgramWireHeader*>(bytes.data());
    const auto slice = [&](ProgramWireSection section) {
        const auto index = WireCounts::index_of(section);
        return std::pair{offsets[index], lengths[index]};
    };
    {
        const auto [offset, length] = slice(ProgramWireSection::Program);
        view.program_ = records<ProgramWireProgramRecord>(payload, offset, length).data();
    }
    {
        const auto [offset, length] = slice(ProgramWireSection::TempoPoints);
        view.tempo_points_ = records<ProgramWireTempoPointRecord>(payload, offset, length);
    }
    {
        const auto [offset, length] = slice(ProgramWireSection::Tracks);
        view.tracks_ = records<ProgramWireTrackRecord>(payload, offset, length);
    }
    {
        const auto [offset, length] = slice(ProgramWireSection::ClipIds);
        view.clip_ids_ = records<ProgramWireIdRecord>(payload, offset, length);
    }
    {
        const auto [offset, length] = slice(ProgramWireSection::NoteEvents);
        view.note_events_ = records<ProgramWireNoteEventRecord>(payload, offset, length);
    }
    {
        const auto [offset, length] = slice(ProgramWireSection::NoteModifiers);
        view.note_modifiers_ = records<ProgramWireNoteModifierRecord>(payload, offset, length);
    }
    {
        const auto [offset, length] = slice(ProgramWireSection::DevicePlacementIds);
        view.device_placement_ids_ = records<ProgramWireIdRecord>(payload, offset, length);
    }
    {
        const auto [offset, length] = slice(ProgramWireSection::AutomationLanes);
        view.automation_lanes_ = records<ProgramWireAutomationLaneRecord>(payload, offset, length);
    }
    {
        const auto [offset, length] = slice(ProgramWireSection::AutomationSegments);
        view.automation_segments_ =
            records<ProgramWireAutomationSegmentRecord>(payload, offset, length);
    }

    const auto& scalars = *view.program_;
    if (scalars.producer_epoch == 0)
        return ViewResult(runtime::Err(
            Error{Code::InvalidProducerEpoch, section_id(ProgramWireSection::Program)}));
    if (scalars.sample_rate_numerator == 0 || scalars.sample_rate_denominator == 0)
        return ViewResult(
            runtime::Err(Error{Code::InvalidSampleRate, section_id(ProgramWireSection::Program),
                               scalars.sample_rate_denominator}));
    if (!view.automation_limits().valid())
        return ViewResult(
            runtime::Err(Error{Code::InvalidLimits, section_id(ProgramWireSection::Program)}));

    for (const auto& point : view.tempo_points_)
        if (point.curve_to_next > static_cast<std::uint8_t>(timebase::TempoCurve::LinearInTicks))
            return ViewResult(
                runtime::Err(Error{Code::InvalidEnum, section_id(ProgramWireSection::TempoPoints),
                                   point.curve_to_next}));

    // Every span a caller can reach is bounds-checked here, so the accessors
    // that return subspans can be unconditional.
    const auto in_range = [](std::uint32_t first, std::uint32_t count, std::size_t total) {
        return static_cast<std::uint64_t>(first) + count <= total;
    };
    for (std::size_t t = 0; t < view.tracks_.size(); ++t) {
        const auto& track = view.tracks_[t];
        if (!in_range(track.clip_first, track.clip_count, view.clip_ids_.size()) ||
            !in_range(track.note_event_first, track.note_event_count, view.note_events_.size()) ||
            !in_range(track.note_modifier_first, track.note_modifier_count,
                      view.note_modifiers_.size()) ||
            !in_range(track.device_placement_first, track.device_placement_count,
                      view.device_placement_ids_.size()) ||
            !in_range(track.automation_lane_first, track.automation_lane_count,
                      view.automation_lanes_.size()))
            return ViewResult(runtime::Err(
                Error{Code::RangeOutOfBounds, section_id(ProgramWireSection::Tracks), t}));
        if (track.provider_selected > static_cast<std::uint8_t>(ProviderKind::ExternalInput) ||
            track.state_policy > static_cast<std::uint8_t>(RendererStatePolicy::CarryByItemId) ||
            (track.flags & ~kProgramWireTrackHasAutomationProgram) != 0)
            return ViewResult(
                runtime::Err(Error{Code::InvalidEnum, section_id(ProgramWireSection::Tracks), t}));
        if ((track.mixer_gain_lane != kProgramWireNoLane &&
             track.mixer_gain_lane >= track.automation_lane_count) ||
            (track.mixer_pan_lane != kProgramWireNoLane &&
             track.mixer_pan_lane >= track.automation_lane_count))
            return ViewResult(runtime::Err(
                Error{Code::RangeOutOfBounds, section_id(ProgramWireSection::Tracks), t}));
    }

    for (std::size_t l = 0; l < view.automation_lanes_.size(); ++l) {
        const auto& lane = view.automation_lanes_[l];
        if (!in_range(lane.segment_first, lane.segment_count, view.automation_segments_.size()))
            return ViewResult(runtime::Err(
                Error{Code::RangeOutOfBounds, section_id(ProgramWireSection::AutomationLanes), l}));
        if (lane.target_kind > static_cast<std::uint8_t>(ProgramWireTargetKind::TrackMixer) ||
            lane.evaluation_rate > static_cast<std::uint8_t>(AutomationEvaluationRate::BlockRate) ||
            lane.mixer_parameter > static_cast<std::uint8_t>(timeline::TrackMixerParameter::Pan))
            return ViewResult(runtime::Err(
                Error{Code::InvalidEnum, section_id(ProgramWireSection::AutomationLanes), l}));
    }

    for (std::size_t s = 0; s < view.automation_segments_.size(); ++s) {
        const auto& segment = view.automation_segments_[s];
        if (segment.interpolation >
            static_cast<std::uint8_t>(timeline::AutomationInterpolation::Continuous))
            return ViewResult(runtime::Err(
                Error{Code::InvalidEnum, section_id(ProgramWireSection::AutomationSegments), s}));
        // A segment is interpolated over, so a reversed span or a non-finite
        // endpoint does not stay local: it reaches every sample the lane feeds.
        if (segment.end_tick < segment.start_tick || segment.end_sample < segment.start_sample ||
            !std::isfinite(segment.start_value) || !std::isfinite(segment.end_value) ||
            !std::isfinite(segment.curvature))
            return ViewResult(runtime::Err(Error{
                Code::MalformedSegment, section_id(ProgramWireSection::AutomationSegments), s}));
    }

    for (std::size_t n = 0; n < view.note_events_.size(); ++n)
        if (view.note_events_[n].kind > static_cast<std::uint8_t>(NoteProgramEventKind::On))
            return ViewResult(runtime::Err(
                Error{Code::InvalidEnum, section_id(ProgramWireSection::NoteEvents), n}));

    for (std::size_t m = 0; m < view.note_modifiers_.size(); ++m) {
        const auto& record = view.note_modifiers_[m];
        if (record.condition > static_cast<std::uint8_t>(timeline::NoteConditionKind::Fill))
            return ViewResult(runtime::Err(
                Error{Code::InvalidEnum, section_id(ProgramWireSection::NoteModifiers), m}));
        // Checked against the document model's own predicate rather than a
        // second rule invented here, so the two can never drift apart.
        timeline::NoteModifier modifier;
        modifier.note_id = timeline::ItemId{record.note_id};
        modifier.probability = record.probability;
        modifier.condition_period = record.condition_period;
        modifier.condition_offset = record.condition_offset;
        modifier.ratchet_count = record.ratchet_count;
        modifier.condition = static_cast<timeline::NoteConditionKind>(record.condition);
        if (!timeline::note_modifier_well_formed(modifier))
            return ViewResult(runtime::Err(Error{
                Code::MalformedNoteModifier, section_id(ProgramWireSection::NoteModifiers), m}));
    }

    return ViewResult(runtime::Ok(view));
}

} // namespace detail

ViewResult decode_program_wire(std::span<const std::byte> bytes) noexcept {
    return detail::ProgramWireDecoder::decode(bytes);
}

bool program_wire_matches(const ProgramWireView& view, const PlaybackProgram& program,
                          std::span<const timebase::TempoPoint> tempo_points,
                          std::uint64_t producer_epoch) noexcept {
    const auto& scalars = view.program();
    if (scalars.producer_epoch != producer_epoch)
        return false;
    if (scalars.generation != program.generation() ||
        scalars.document_revision != program.document_revision() ||
        scalars.project_id != program.project_id().value ||
        scalars.sequence_id != program.sequence_id().value ||
        scalars.generated_id_base != program.generated_id_base())
        return false;
    if (view.sample_rate() != program.tempo_map().sample_rate())
        return false;
    if (!(view.automation_limits() == program.automation_limits()))
        return false;

    // The wire carries the tempo points, not the compiled segments. Comparing
    // the carried points against the caller's and the caller's against the map
    // is what proves the wire describes the map the program was compiled from.
    if (!program.tempo_map().matches(tempo_points) ||
        view.tempo_points().size() != tempo_points.size())
        return false;
    for (std::size_t i = 0; i < tempo_points.size(); ++i) {
        const auto& record = view.tempo_points()[i];
        const auto& point = tempo_points[i];
        if (record.tick != point.tick.value ||
            record.bpm_bits != std::bit_cast<std::uint64_t>(point.bpm) ||
            record.curve_to_next != static_cast<std::uint8_t>(point.curve_to_next))
            return false;
    }

    const auto tracks = program.tracks();
    if (view.tracks().size() != tracks.size())
        return false;

    for (std::size_t t = 0; t < tracks.size(); ++t) {
        const auto& track = *tracks[t];
        const auto& record = view.tracks()[t];
        if (track.audio_program() != nullptr)
            return false;
        const auto& production = track.arrangement_production();
        if (production.mode != timeline::ProductionMode::Synchronous ||
            production.reproducibility != timeline::ReproducibilityClass::Deterministic ||
            production.lookahead_ms != 0)
            return false;
        if (record.id != track.id().value || record.generation != track.generation() ||
            record.provider_selected != static_cast<std::uint8_t>(track.provider().selected) ||
            record.provider_available_mask != track.provider().available_mask ||
            record.state_policy != static_cast<std::uint8_t>(track.state_policy()) ||
            record.expanded_clip_count != track.expanded_clip_count() ||
            record.expanded_note_event_count != track.expanded_note_event_count() ||
            record.generated_id_start != track.generated_id_start() ||
            record.generated_id_count != track.generated_id_count())
            return false;

        const auto* automation = track.automation_program();
        const bool has_program = (record.flags & kProgramWireTrackHasAutomationProgram) != 0;
        if (has_program != (automation != nullptr))
            return false;

        const auto clips = track.ordered_clip_ids();
        const auto wire_clips = view.clip_ids_for(record);
        if (wire_clips.size() != clips.size())
            return false;
        for (std::size_t i = 0; i < clips.size(); ++i)
            if (wire_clips[i].value != clips[i].value)
                return false;

        const auto notes = track.arrangement_note_events();
        const auto wire_notes = view.note_events_for(record);
        if (wire_notes.size() != notes.size())
            return false;
        for (std::size_t i = 0; i < notes.size(); ++i) {
            const auto& source = notes[i];
            const auto& copy = wire_notes[i];
            if (copy.sample != source.sample.value || copy.tick != source.tick.value ||
                copy.clip_id != source.clip_id.value || copy.note_id != source.note_id.value ||
                copy.velocity != source.velocity || copy.pitch != source.pitch ||
                copy.channel != source.channel ||
                copy.kind != static_cast<std::uint8_t>(source.kind))
                return false;
        }

        const auto modifiers = track.note_modifiers();
        const auto wire_modifiers = view.note_modifiers_for(record);
        if (wire_modifiers.size() != modifiers.size())
            return false;
        for (std::size_t i = 0; i < modifiers.size(); ++i) {
            const auto& source = modifiers[i];
            const auto& copy = wire_modifiers[i];
            if (copy.draw_key != source.draw_key || copy.note_id != source.modifier.note_id.value ||
                copy.probability != source.modifier.probability ||
                copy.condition_period != source.modifier.condition_period ||
                copy.condition_offset != source.modifier.condition_offset ||
                copy.ratchet_count != source.modifier.ratchet_count ||
                copy.condition != static_cast<std::uint8_t>(source.modifier.condition))
                return false;
        }

        const auto placements = track.ordered_device_placement_ids();
        const auto wire_placements = view.device_placement_ids_for(record);
        if (wire_placements.size() != placements.size())
            return false;
        for (std::size_t i = 0; i < placements.size(); ++i)
            if (wire_placements[i].value != placements[i].value)
                return false;

        const auto lanes = automation ? automation->programs()
                                      : std::span<const std::shared_ptr<const AutomationProgram>>{};
        const auto wire_lanes = view.automation_lanes_for(record);
        if (wire_lanes.size() != lanes.size())
            return false;
        for (std::size_t i = 0; i < lanes.size(); ++i) {
            const auto& lane = *lanes[i];
            const auto& copy = wire_lanes[i];
            if (copy.lane_id != lane.lane_id().value || copy.generation != lane.generation() ||
                copy.leading_value != lane.leading_value())
                return false;
            const auto expected = lane_record(lane, copy.segment_first);
            if (copy.target_kind != expected.target_kind ||
                copy.device_placement_id != expected.device_placement_id ||
                copy.device_param_id != expected.device_param_id ||
                copy.mixer_parameter != expected.mixer_parameter ||
                copy.evaluation_rate != expected.evaluation_rate)
                return false;

            const auto segments = lane.segments();
            const auto wire_segments = view.segments_for(copy);
            if (wire_segments.size() != segments.size())
                return false;
            for (std::size_t s = 0; s < segments.size(); ++s) {
                const auto& source = segments[s];
                const auto& piece = wire_segments[s];
                if (piece.start_tick != source.start_tick.value ||
                    piece.end_tick != source.end_tick.value ||
                    piece.start_sample != source.start_sample.value ||
                    piece.end_sample != source.end_sample.value ||
                    piece.start_value != source.start_value ||
                    piece.end_value != source.end_value || piece.curvature != source.curvature ||
                    piece.interpolation != static_cast<std::uint8_t>(source.interpolation))
                    return false;
            }
        }

        const auto& mixer = track.mixer();
        if (record.mixer_gain_linear != mixer.gain_linear || record.mixer_pan != mixer.pan ||
            record.mixer_gain_lane != lane_index(lanes, mixer.gain_automation) ||
            record.mixer_pan_lane != lane_index(lanes, mixer.pan_automation))
            return false;
    }
    return true;
}

} // namespace pulp::playback
