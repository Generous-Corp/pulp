#include <pulp/playback/program_wire.hpp>

#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/automation_program.hpp>
#include <pulp/playback/track_automation_program.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/timeline/automation_lane.hpp>
#include <pulp/timeline/note_modifier.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
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
    record.instance_token = lane.instance_token().value;
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
    std::uint32_t previous_section_id = 0;
    bool noncanonical_section_order = false;
    std::uint32_t noncanonical_section_id = 0;
    std::uint32_t noncanonical_previous_id = 0;

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
        if (i != 0 && entry.id <= previous_section_id && !noncanonical_section_order) {
            noncanonical_section_order = true;
            noncanonical_section_id = entry.id;
            noncanonical_previous_id = previous_section_id;
        }
        previous_section_id = entry.id;
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
    if (noncanonical_section_order)
        return ViewResult(runtime::Err(Error{Code::NonCanonicalSectionOrder,
                                             noncanonical_section_id, noncanonical_previous_id}));

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
    std::uint64_t clip_cursor = 0;
    std::uint64_t note_event_cursor = 0;
    std::uint64_t note_modifier_cursor = 0;
    std::uint64_t device_cursor = 0;
    std::uint64_t lane_cursor = 0;
    std::uint64_t segment_cursor = 0;
    if (view.tracks_.size() > kProgramWireMaximumTracks)
        return ViewResult(runtime::Err(
            Error{Code::InvalidLimits, section_id(ProgramWireSection::Tracks),
                  view.tracks_.size()}));
    std::array<std::uint64_t, kProgramWireMaximumTracks * 2u> track_ids{};
    for (std::size_t t = 0; t < view.tracks_.size(); ++t) {
        const auto& track = view.tracks_[t];
        if (track.id == 0 || track.generation == 0)
            return ViewResult(runtime::Err(Error{Code::NonCanonicalRangeOwnership,
                                                 section_id(ProgramWireSection::Tracks), t}));
        auto slot = static_cast<std::size_t>(track.id) & (track_ids.size() - 1u);
        while (track_ids[slot] != 0 && track_ids[slot] != track.id)
            slot = (slot + 1u) & (track_ids.size() - 1u);
        if (track_ids[slot] == track.id)
                return ViewResult(runtime::Err(Error{Code::NonCanonicalRangeOwnership,
                                                     section_id(ProgramWireSection::Tracks), t}));
        track_ids[slot] = track.id;
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
        if (track.clip_first != clip_cursor || track.note_event_first != note_event_cursor ||
            track.note_modifier_first != note_modifier_cursor ||
            track.device_placement_first != device_cursor ||
            track.automation_lane_first != lane_cursor)
            return ViewResult(runtime::Err(Error{Code::NonCanonicalRangeOwnership,
                                                 section_id(ProgramWireSection::Tracks), t}));
        clip_cursor += track.clip_count;
        note_event_cursor += track.note_event_count;
        note_modifier_cursor += track.note_modifier_count;
        device_cursor += track.device_placement_count;
        lane_cursor += track.automation_lane_count;
        if (track.device_placement_count > scalars.max_device_placements_per_track ||
            track.automation_lane_count > scalars.max_lanes_per_track)
            return ViewResult(runtime::Err(
                Error{Code::InvalidLimits, section_id(ProgramWireSection::Tracks), t}));
        if (track.provider_selected > static_cast<std::uint8_t>(ProviderKind::ExternalInput) ||
            track.state_policy > static_cast<std::uint8_t>(RendererStatePolicy::CarryByItemId) ||
            (track.flags & ~kProgramWireTrackHasAutomationProgram) != 0)
            return ViewResult(
                runtime::Err(Error{Code::InvalidEnum, section_id(ProgramWireSection::Tracks), t}));
        if (track.automation_lane_count != 0 &&
            (track.flags & kProgramWireTrackHasAutomationProgram) == 0)
            return ViewResult(runtime::Err(Error{Code::NonCanonicalRangeOwnership,
                                                 section_id(ProgramWireSection::Tracks), t}));
        if ((track.mixer_gain_lane != kProgramWireNoLane &&
             track.mixer_gain_lane >= track.automation_lane_count) ||
            (track.mixer_pan_lane != kProgramWireNoLane &&
             track.mixer_pan_lane >= track.automation_lane_count))
            return ViewResult(runtime::Err(
                Error{Code::RangeOutOfBounds, section_id(ProgramWireSection::Tracks), t}));

        const auto placements = view.device_placement_ids_for(track);
        for (std::size_t placement = 0; placement < placements.size(); ++placement)
            for (std::size_t previous = 0; previous < placement; ++previous)
                if (placements[previous].value == placements[placement].value)
                    return ViewResult(runtime::Err(
                        Error{Code::NonCanonicalRangeOwnership,
                              section_id(ProgramWireSection::DevicePlacementIds), placement}));

        std::uint64_t track_points = 0;
        const auto lanes = view.automation_lanes_for(track);
        for (std::size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
            const auto& lane = lanes[lane_index];
            if (lane.segment_first != segment_cursor)
                return ViewResult(runtime::Err(Error{
                    Code::NonCanonicalRangeOwnership,
                    section_id(ProgramWireSection::AutomationLanes), track.automation_lane_first}));
            segment_cursor += lane.segment_count;
            track_points += lane.segment_count;
            if (lane.segment_count > scalars.max_points_per_lane ||
                track_points > scalars.max_points_per_track)
                return ViewResult(runtime::Err(
                    Error{Code::InvalidLimits, section_id(ProgramWireSection::AutomationLanes),
                          lane.lane_id}));
            if (lane.target_kind ==
                static_cast<std::uint8_t>(ProgramWireTargetKind::DeviceParameter)) {
                const auto found =
                    std::find_if(placements.begin(), placements.end(),
                                 [&](const ProgramWireIdRecord& placement) {
                                     return placement.value == lane.device_placement_id;
                                 });
                if (found == placements.end())
                    return ViewResult(runtime::Err(
                        Error{Code::AutomationTargetUnresolved,
                              section_id(ProgramWireSection::AutomationLanes), lane_index}));
            }
            for (std::size_t previous = 0; previous < lane_index; ++previous) {
                const auto& related = lanes[previous];
                const bool duplicate_device =
                    lane.target_kind ==
                        static_cast<std::uint8_t>(ProgramWireTargetKind::DeviceParameter) &&
                    related.target_kind == lane.target_kind &&
                    related.device_placement_id == lane.device_placement_id &&
                    related.device_param_id == lane.device_param_id;
                const bool duplicate_mixer =
                    lane.target_kind ==
                        static_cast<std::uint8_t>(ProgramWireTargetKind::TrackMixer) &&
                    related.target_kind == lane.target_kind &&
                    related.mixer_parameter == lane.mixer_parameter;
                if (duplicate_device || duplicate_mixer)
                    return ViewResult(runtime::Err(
                        Error{Code::DuplicateAutomationTarget,
                              section_id(ProgramWireSection::AutomationLanes), lane_index}));
            }
        }
    }
    if (clip_cursor != view.clip_ids_.size() || note_event_cursor != view.note_events_.size() ||
        note_modifier_cursor != view.note_modifiers_.size() ||
        device_cursor != view.device_placement_ids_.size() ||
        lane_cursor != view.automation_lanes_.size() ||
        segment_cursor != view.automation_segments_.size())
        return ViewResult(runtime::Err(
            Error{Code::NonCanonicalRangeOwnership, section_id(ProgramWireSection::Tracks)}));

    for (std::size_t l = 0; l < view.automation_lanes_.size(); ++l) {
        const auto& lane = view.automation_lanes_[l];
        if (!in_range(lane.segment_first, lane.segment_count, view.automation_segments_.size()))
            return ViewResult(runtime::Err(
                Error{Code::RangeOutOfBounds, section_id(ProgramWireSection::AutomationLanes), l}));
        // Zero is what a lane record left unwritten holds, and it would compare
        // equal to every other unwritten lane, so a consumer would report
        // Unchanged for a program it has never adopted. Refused here rather
        // than treated as "no identity available".
        if (lane.instance_token == 0)
            return ViewResult(runtime::Err(Error{
                Code::InvalidInstanceToken, section_id(ProgramWireSection::AutomationLanes), l}));
        if (lane.lane_id == 0 || lane.generation == 0 || lane.segment_count == 0 ||
            !std::isfinite(lane.leading_value))
            return ViewResult(runtime::Err(
                Error{Code::MalformedSegment, section_id(ProgramWireSection::AutomationLanes), l}));
        if (lane.target_kind > static_cast<std::uint8_t>(ProgramWireTargetKind::TrackMixer) ||
            lane.evaluation_rate > static_cast<std::uint8_t>(AutomationEvaluationRate::BlockRate) ||
            lane.mixer_parameter > static_cast<std::uint8_t>(timeline::TrackMixerParameter::Pan))
            return ViewResult(runtime::Err(
                Error{Code::InvalidEnum, section_id(ProgramWireSection::AutomationLanes), l}));
        if ((lane.target_kind ==
                 static_cast<std::uint8_t>(ProgramWireTargetKind::DeviceParameter) &&
             (lane.device_placement_id == 0 || lane.mixer_parameter != 0)) ||
            (lane.target_kind == static_cast<std::uint8_t>(ProgramWireTargetKind::TrackMixer) &&
             (lane.device_placement_id != 0 || lane.device_param_id != 0)))
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

    for (const auto& lane : view.automation_lanes_) {
        const auto segments = view.segments_for(lane);
        for (std::size_t index = 1; index < segments.size(); ++index) {
            const auto& previous = segments[index - 1u];
            const auto& current = segments[index];
            if (previous.end_tick != current.start_tick ||
                previous.end_sample != current.start_sample ||
                previous.end_value != current.start_value)
                return ViewResult(runtime::Err(Error{
                    Code::MalformedSegment, section_id(ProgramWireSection::AutomationSegments),
                    lane.segment_first + index}));
        }
        const auto& terminal = segments.back();
        if (terminal.start_tick != terminal.end_tick ||
            terminal.start_sample != terminal.end_sample ||
            terminal.start_value != terminal.end_value)
            return ViewResult(runtime::Err(Error{Code::MalformedSegment,
                                                 section_id(ProgramWireSection::AutomationSegments),
                                                 lane.segment_first + lane.segment_count - 1u}));
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
            if (copy.instance_token != expected.instance_token ||
                copy.target_kind != expected.target_kind ||
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

namespace {

AutomationProgramSegment read_wire_segment(const void* records, std::size_t index) noexcept {
    const auto& record = static_cast<const ProgramWireAutomationSegmentRecord*>(records)[index];
    return {
        .start_tick = {record.start_tick},
        .end_tick = {record.end_tick},
        .start_sample = {record.start_sample},
        .end_sample = {record.end_sample},
        .start_value = record.start_value,
        .end_value = record.end_value,
        .interpolation = static_cast<timeline::AutomationInterpolation>(record.interpolation),
        .curvature = record.curvature,
    };
}

bool wire_tempo_matches(const ProgramWireView& view, const timebase::CompiledTempoMap& tempo_map,
                        std::span<const timebase::TempoPoint> points) noexcept {
    if (tempo_map.sample_rate() != view.sample_rate() || !tempo_map.matches(points) ||
        points.size() != view.tempo_points().size())
        return false;
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto& point = points[index];
        const auto& record = view.tempo_points()[index];
        if (record.tick != point.tick.value ||
            record.bpm_bits != std::bit_cast<std::uint64_t>(point.bpm) ||
            record.curve_to_next != static_cast<std::uint8_t>(point.curve_to_next))
            return false;
    }
    return true;
}

ProgramWireLaneIdentity lane_identity(const ProgramWireView& view,
                                      const ProgramWireTrackRecord& track,
                                      const ProgramWireAutomationLaneRecord& lane) noexcept {
    return {
        .producer_epoch = view.producer_epoch(),
        .program_generation = view.program().generation,
        .track_id = {track.id},
        .track_generation = track.generation,
        .lane_id = {lane.lane_id},
        .lane_generation = lane.generation,
        .instance_token = {lane.instance_token},
    };
}

ProgramWireLaneState* find_lane_state(std::span<ProgramWireLaneState> states,
                                      timeline::ItemId track_id,
                                      timeline::ItemId lane_id) noexcept {
    for (auto& state : states)
        if (state.identity.track_id == track_id && state.identity.lane_id == lane_id)
            return &state;
    return nullptr;
}

ProgramWireLaneState* find_lane_state(std::span<ProgramWireLaneState> states,
                                      timeline::ItemId lane_id) noexcept {
    for (auto& state : states)
        if (state.identity.lane_id == lane_id)
            return &state;
    return nullptr;
}

bool same_cursor_identity(const ProgramWireLaneIdentity& lhs,
                          const ProgramWireLaneIdentity& rhs) noexcept {
    return lhs.producer_epoch == rhs.producer_epoch && lhs.track_id == rhs.track_id &&
           lhs.lane_id == rhs.lane_id && lhs.lane_generation == rhs.lane_generation &&
           lhs.instance_token == rhs.instance_token;
}

bool same_publication_identity(const ProgramWireView& lhs, const ProgramWireView& rhs) noexcept {
    if (lhs.producer_epoch() != rhs.producer_epoch() ||
        lhs.program().generation != rhs.program().generation ||
        lhs.tracks().size() != rhs.tracks().size() ||
        lhs.automation_lanes().size() != rhs.automation_lanes().size())
        return false;
    for (std::size_t track_index = 0; track_index < lhs.tracks().size(); ++track_index) {
        const auto& left_track = lhs.tracks()[track_index];
        const auto& right_track = rhs.tracks()[track_index];
        if (left_track.id != right_track.id || left_track.generation != right_track.generation ||
            left_track.automation_lane_count != right_track.automation_lane_count)
            return false;
        const auto left_lanes = lhs.automation_lanes_for(left_track);
        const auto right_lanes = rhs.automation_lanes_for(right_track);
        for (std::size_t lane_index = 0; lane_index < left_lanes.size(); ++lane_index) {
            if (left_lanes[lane_index].lane_id != right_lanes[lane_index].lane_id ||
                left_lanes[lane_index].generation != right_lanes[lane_index].generation ||
                left_lanes[lane_index].instance_token != right_lanes[lane_index].instance_token)
                return false;
        }
    }
    return true;
}

bool same_capacity_group(const ProgramWireAutomationLaneRecord& lhs,
                         const ProgramWireAutomationLaneRecord& rhs) noexcept {
    if (lhs.target_kind != rhs.target_kind)
        return false;
    if (lhs.target_kind == static_cast<std::uint8_t>(ProgramWireTargetKind::DeviceParameter))
        return lhs.device_placement_id == rhs.device_placement_id;
    return lhs.mixer_parameter == rhs.mixer_parameter;
}

std::uint64_t wire_selected_rank(std::uint32_t selection, std::uint32_t selected_count,
                                 std::uint64_t candidate_count) noexcept {
    if (selected_count <= 1u || candidate_count <= 1u)
        return candidate_count == 0 ? 0 : candidate_count - 1u;
    return static_cast<std::uint64_t>(selection) * (candidate_count - 1u) / (selected_count - 1u);
}

std::optional<ProgramWireError>
wire_consumer_capacity_error(std::span<const std::byte> bytes, std::size_t track_capacity,
                             std::size_t lane_capacity, std::size_t byte_capacity) noexcept {
    constexpr std::uint32_t kMaximumDirectoryEntries = 32;
    if (bytes.size() > byte_capacity)
        return ProgramWireError{ProgramWireErrorCode::InvalidLimits, 0, bytes.size()};
    if (bytes.size() < sizeof(ProgramWireHeader))
        return std::nullopt;
    ProgramWireHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.section_count > kMaximumDirectoryEntries)
        return ProgramWireError{ProgramWireErrorCode::InvalidLimits, 0, header.section_count};
    const auto directory_bytes =
        static_cast<std::uint64_t>(header.section_count) * sizeof(ProgramWireSectionEntry);
    if (directory_bytes > bytes.size() - sizeof(ProgramWireHeader))
        return std::nullopt;
    for (std::uint32_t index = 0; index < header.section_count; ++index) {
        ProgramWireSectionEntry entry;
        std::memcpy(&entry,
                    bytes.data() + sizeof(ProgramWireHeader) +
                        index * sizeof(ProgramWireSectionEntry),
                    sizeof(entry));
        if (entry.id == section_id(ProgramWireSection::Tracks) &&
            entry.bytes / sizeof(ProgramWireTrackRecord) > track_capacity)
            return ProgramWireError{ProgramWireErrorCode::InvalidLimits, entry.id,
                                    entry.bytes / sizeof(ProgramWireTrackRecord)};
        if (entry.id == section_id(ProgramWireSection::AutomationLanes) &&
            entry.bytes / sizeof(ProgramWireAutomationLaneRecord) > lane_capacity)
            return ProgramWireError{ProgramWireErrorCode::InvalidLimits, entry.id,
                                    entry.bytes / sizeof(ProgramWireAutomationLaneRecord)};
    }
    return std::nullopt;
}

} // namespace

ProgramWireAdoptionResult
ProgramWireAutomationConsumer::adopt(ProgramWireBytePin candidate,
                                     const timebase::CompiledTempoMap& tempo_map,
                                     std::span<const timebase::TempoPoint> tempo_points) noexcept {
    runtime::ScopedNoAlloc no_alloc;
    ProgramWireAdoptionResult result;
    auto reject = [&](ProgramWireConsumerCode code, ProgramWireError error = {}) {
        result.code = code;
        result.adoption = ProgramWireAdoption::Rejected;
        result.wire_error = error;
        result.returned_pin = std::move(candidate);
    };

    if (const auto capacity = wire_consumer_capacity_error(
            candidate.bytes(), track_capacity_, lane_state_.size(), wire_byte_capacity_)) {
        reject(ProgramWireConsumerCode::StateCapacityExceeded, *capacity);
        return result;
    }

    auto decoded = decode_program_wire(candidate.bytes());
    if (!decoded) {
        reject(ProgramWireConsumerCode::DecodeRejected, decoded.error());
        return result;
    }
    const auto candidate_view = decoded.value();
    if (!wire_tempo_matches(candidate_view, tempo_map, tempo_points)) {
        reject(ProgramWireConsumerCode::TempoMapMismatch, {ProgramWireErrorCode::TempoMapMismatch});
        return result;
    }
    if (candidate_view.automation_lanes().size() > lane_state_.size()) {
        reject(ProgramWireConsumerCode::StateCapacityExceeded,
               {ProgramWireErrorCode::InvalidLimits,
                static_cast<std::uint32_t>(ProgramWireSection::AutomationLanes),
                candidate_view.automation_lanes().size()});
        return result;
    }
    if (candidate_view.tracks().size() > track_capacity_) {
        reject(ProgramWireConsumerCode::StateCapacityExceeded,
               {ProgramWireErrorCode::InvalidLimits,
                static_cast<std::uint32_t>(ProgramWireSection::Tracks),
                candidate_view.tracks().size()});
        return result;
    }
    for (std::size_t lane = 0; lane < candidate_view.automation_lanes().size(); ++lane) {
        for (std::size_t previous = 0; previous < lane; ++previous) {
            if (candidate_view.automation_lanes()[previous].lane_id ==
                candidate_view.automation_lanes()[lane].lane_id) {
                reject(ProgramWireConsumerCode::DecodeRejected,
                       {ProgramWireErrorCode::NonCanonicalRangeOwnership,
                        static_cast<std::uint32_t>(ProgramWireSection::AutomationLanes), lane});
                return result;
            }
        }
    }
    for (std::size_t index = 0; index < candidate_view.automation_lanes().size(); ++index) {
        if (candidate_view.automation_lanes()[index].evaluation_rate !=
            static_cast<std::uint8_t>(AutomationEvaluationRate::SampleAccurate)) {
            reject(ProgramWireConsumerCode::DecodeRejected,
                   {ProgramWireErrorCode::InvalidEnum,
                    static_cast<std::uint32_t>(ProgramWireSection::AutomationLanes), index});
            return result;
        }
    }
    if (active_pin_ && candidate_view.producer_epoch() == active_view_.producer_epoch()) {
        for (const auto& track : candidate_view.tracks()) {
            for (const auto& lane : candidate_view.automation_lanes_for(track)) {
                const auto* active = find_lane_state(lane_state_, {lane.lane_id});
                if (active != nullptr &&
                    active->identity.producer_epoch == candidate_view.producer_epoch() &&
                    lane.generation < active->identity.lane_generation) {
                    reject(ProgramWireConsumerCode::StalePublication,
                           {ProgramWireErrorCode::StaleLaneGeneration,
                            static_cast<std::uint32_t>(ProgramWireSection::AutomationLanes),
                            lane.lane_id});
                    return result;
                }
            }
        }
    }

    if (active_pin_ && candidate_view.producer_epoch() == active_view_.producer_epoch() &&
        candidate_view.program().generation < active_view_.program().generation) {
        reject(ProgramWireConsumerCode::StalePublication);
        return result;
    }
    if (active_pin_ && &tempo_map == tempo_map_ &&
        same_publication_identity(candidate_view, active_view_)) {
        result.adoption = ProgramWireAdoption::Unchanged;
        result.returned_pin = std::move(candidate);
        return result;
    }

    // The decoder and all capacity checks have completed. From this point the
    // candidate cannot be rejected, so updating state and transferring pins is
    // one commit rather than a partially visible adoption.
    for (auto& state : lane_state_) {
        bool still_present = false;
        for (const auto& track : candidate_view.tracks()) {
            for (const auto& lane : candidate_view.automation_lanes_for(track)) {
                if (state.identity.track_id == timeline::ItemId{track.id} &&
                    state.identity.lane_id == timeline::ItemId{lane.lane_id}) {
                    still_present = true;
                    break;
                }
            }
            if (still_present)
                break;
        }
        if (!still_present) {
            state.cursor.reset();
            state.identity = {};
        }
    }
    for (const auto& track : candidate_view.tracks()) {
        for (const auto& lane : candidate_view.automation_lanes_for(track)) {
            const auto identity = lane_identity(candidate_view, track, lane);
            auto* state = find_lane_state(lane_state_, identity.track_id, identity.lane_id);
            if (state == nullptr) {
                for (auto& slot : lane_state_) {
                    if (slot.identity.lane_id.value == 0) {
                        state = &slot;
                        break;
                    }
                }
            }
            if (state == nullptr) {
                // Capacity was preflighted and lane ids are decoder-unique, so
                // reaching this would mean internal state corruption.
                std::abort();
            }
            if (!same_cursor_identity(state->identity, identity))
                state->cursor.reset();
            state->identity = identity;
        }
    }

    result.returned_pin = std::move(active_pin_);
    active_pin_ = std::move(candidate);
    active_view_ = candidate_view;
    tempo_map_ = &tempo_map;
    lane_count_ = candidate_view.automation_lanes().size();
    result.adoption = ProgramWireAdoption::Adopted;
    return result;
}

ProgramWireAutomationRenderResult
ProgramWireAutomationConsumer::render(const TransportSnapshot& transport,
                                      std::span<ProgramWireAutomationLaneOutput> output) noexcept {
    runtime::ScopedNoAlloc no_alloc;
    ProgramWireAutomationRenderResult result;
    if (!active_pin_) {
        result.code = ProgramWireConsumerCode::MissingProgram;
        return result;
    }
    if (!valid_transport_ranges(transport) || transport.tempo_map != tempo_map_) {
        result.code = ProgramWireConsumerCode::InvalidTransport;
        return result;
    }
    if (output.size() < lane_count_) {
        result.code = ProgramWireConsumerCode::OutputCapacityExceeded;
        return result;
    }

    const auto limits = active_view_.automation_limits();
    std::uint64_t total_intersecting = 0;
    std::size_t output_index = 0;
    for (const auto& track : active_view_.tracks()) {
        for (const auto& lane : active_view_.automation_lanes_for(track)) {
            auto* state = find_lane_state(lane_state_, {track.id}, {lane.lane_id});
            state->next_cursor = state->cursor;
            state->selected_count = 0;
            state->write_position = 0;
            state->output_index = output_index++;
            state->group_coalesced = false;
            const auto segment_records = active_view_.segments_for(lane);
            const AutomationProgramView program{
                lane.generation,
                {lane.instance_token},
                {lane.lane_id},
                *tempo_map_,
                AutomationSegmentView::from_records(segment_records, read_wire_segment),
                lane.leading_value};
            const auto remaining_work = static_cast<std::uint32_t>(
                limits.max_intersecting_segments_per_block - total_intersecting);
            state->cursor_result =
                state->next_cursor.process(program, transport, state->scratch, remaining_work);
            if (state->cursor_result.code != AutomationCursorCode::Ok &&
                state->cursor_result.code != AutomationCursorCode::Coalesced) {
                result.code =
                    state->cursor_result.code == AutomationCursorCode::InsufficientCapacity
                        ? ProgramWireConsumerCode::OutputCapacityExceeded
                        : ProgramWireConsumerCode::CursorRejected;
                return result;
            }
            state->event_count = state->cursor_result.emitted_events;
            total_intersecting += state->cursor_result.intersecting_segments;
        }
    }

    const auto walk_group = [&](const ProgramWireTrackRecord& track,
                                std::span<const ProgramWireAutomationLaneRecord> lanes,
                                std::size_t anchor, auto&& visitor) noexcept {
        std::size_t heap_size = 0;
        for (std::size_t index = 0; index < lanes.size(); ++index) {
            if (!same_capacity_group(lanes[anchor], lanes[index]))
                continue;
            auto* state = find_lane_state(lane_state_, {track.id}, {lanes[index].lane_id});
            state->merge_position = 0;
            if (state->event_count != 0)
                merge_heap_[heap_size++] = state;
        }
        const auto state_less = [](const ProgramWireLaneState* lhs,
                                   const ProgramWireLaneState* rhs) noexcept {
            const auto& left = lhs->scratch[lhs->merge_position];
            const auto& right = rhs->scratch[rhs->merge_position];
            return left.sample_offset < right.sample_offset ||
                   (left.sample_offset == right.sample_offset &&
                    lhs->identity.lane_id < rhs->identity.lane_id);
        };
        const auto sift_down = [&](std::size_t root) noexcept {
            while (true) {
                const auto left = root * 2u + 1u;
                if (left >= heap_size)
                    return;
                const auto right = left + 1u;
                auto smallest = left;
                if (right < heap_size && state_less(merge_heap_[right], merge_heap_[left]))
                    smallest = right;
                if (!state_less(merge_heap_[smallest], merge_heap_[root]))
                    return;
                std::swap(merge_heap_[root], merge_heap_[smallest]);
                root = smallest;
            }
        };
        for (auto parent = heap_size / 2u; parent > 0; --parent)
            sift_down(parent - 1u);
        while (heap_size != 0) {
            auto* selected_state = merge_heap_[0];
            const auto event = selected_state->scratch[selected_state->merge_position++];
            visitor(*selected_state, event);
            if (selected_state->merge_position < selected_state->event_count) {
                sift_down(0);
            } else {
                --heap_size;
                if (heap_size != 0) {
                    merge_heap_[0] = merge_heap_[heap_size];
                    sift_down(0);
                }
            }
        }
    };

    // Count and select at the same per-device boundary as the native track
    // renderer. Nothing is committed or copied to caller output in this pass.
    for (const auto& track : active_view_.tracks()) {
        const auto lanes = active_view_.automation_lanes_for(track);
        for (std::size_t anchor = 0; anchor < lanes.size(); ++anchor) {
            bool seen_group = false;
            for (std::size_t previous = 0; previous < anchor; ++previous)
                seen_group = seen_group || same_capacity_group(lanes[anchor], lanes[previous]);
            if (seen_group)
                continue;

            std::uint64_t candidate_count = 0;
            std::uint64_t mandatory_count = 0;
            bool lane_coalesced = false;
            for (std::size_t index = 0; index < lanes.size(); ++index) {
                if (!same_capacity_group(lanes[anchor], lanes[index]))
                    continue;
                auto* state = find_lane_state(lane_state_, {track.id}, {lanes[index].lane_id});
                candidate_count += state->event_count;
                lane_coalesced =
                    lane_coalesced || state->cursor_result.code == AutomationCursorCode::Coalesced;
                for (std::uint32_t event = 0; event < state->event_count; ++event)
                    mandatory_count +=
                        state->scratch[event].transition != AutomationTransition::LinearRamp;
            }
            if (mandatory_count > limits.max_events_per_device_per_block) {
                result.code = ProgramWireConsumerCode::OutputCapacityExceeded;
                return result;
            }
            const auto optional_count = candidate_count - mandatory_count;
            const auto selected_optional = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                optional_count, limits.max_events_per_device_per_block - mandatory_count));
            const bool group_coalesced =
                lane_coalesced || candidate_count > limits.max_events_per_device_per_block;
            std::uint64_t optional_rank = 0;
            std::uint32_t optional_selection = 0;
            walk_group(track, lanes, anchor,
                       [&](ProgramWireLaneState& state, AutomationBlockEvent event) noexcept {
                           const bool mandatory =
                               event.transition != AutomationTransition::LinearRamp;
                           bool selected = mandatory;
                           if (!mandatory) {
                               const auto wanted =
                                   selected_optional == 0
                                       ? std::numeric_limits<std::uint64_t>::max()
                                       : wire_selected_rank(optional_selection, selected_optional,
                                                            optional_count);
                               selected = optional_selection < selected_optional &&
                                          optional_rank == wanted;
                               if (selected)
                                   ++optional_selection;
                               ++optional_rank;
                           }
                           if (selected)
                               ++state.selected_count;
                           state.group_coalesced = group_coalesced;
                       });
        }
    }
    for (const auto& state : lane_state_) {
        if (state.identity.lane_id.value != 0 &&
            output[state.output_index].events.size() < state.selected_count) {
            result.code = ProgramWireConsumerCode::OutputCapacityExceeded;
            return result;
        }
    }

    // Repeat the deterministic selection walk to publish the preflighted
    // events. Cursors are committed only after every group has succeeded.
    for (const auto& track : active_view_.tracks()) {
        const auto lanes = active_view_.automation_lanes_for(track);
        for (std::size_t anchor = 0; anchor < lanes.size(); ++anchor) {
            bool seen_group = false;
            for (std::size_t previous = 0; previous < anchor; ++previous)
                seen_group = seen_group || same_capacity_group(lanes[anchor], lanes[previous]);
            if (seen_group)
                continue;
            std::uint64_t candidate_count = 0;
            std::uint64_t mandatory_count = 0;
            for (std::size_t index = 0; index < lanes.size(); ++index) {
                if (!same_capacity_group(lanes[anchor], lanes[index]))
                    continue;
                const auto* state =
                    find_lane_state(lane_state_, {track.id}, {lanes[index].lane_id});
                candidate_count += state->event_count;
                for (std::uint32_t event = 0; event < state->event_count; ++event)
                    mandatory_count +=
                        state->scratch[event].transition != AutomationTransition::LinearRamp;
            }
            const auto optional_count = candidate_count - mandatory_count;
            const auto selected_optional = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                optional_count, limits.max_events_per_device_per_block - mandatory_count));
            std::uint64_t optional_rank = 0;
            std::uint32_t optional_selection = 0;
            walk_group(track, lanes, anchor,
                       [&](ProgramWireLaneState& state, AutomationBlockEvent event) noexcept {
                           const bool mandatory =
                               event.transition != AutomationTransition::LinearRamp;
                           bool selected = mandatory;
                           if (!mandatory) {
                               const auto wanted =
                                   selected_optional == 0
                                       ? std::numeric_limits<std::uint64_t>::max()
                                       : wire_selected_rank(optional_selection, selected_optional,
                                                            optional_count);
                               selected = optional_selection < selected_optional &&
                                          optional_rank == wanted;
                               if (selected)
                                   ++optional_selection;
                               ++optional_rank;
                           }
                           if (selected)
                               output[state.output_index].events[state.write_position++] = event;
                       });
        }
    }

    output_index = 0;
    for (const auto& track : active_view_.tracks()) {
        for (const auto& lane : active_view_.automation_lanes_for(track)) {
            auto* state = find_lane_state(lane_state_, {track.id}, {lane.lane_id});
            auto& destination = output[output_index++];
            destination.track_id = {track.id};
            destination.lane_id = {lane.lane_id};
            destination.result = state->cursor_result;
            destination.result.emitted_events = state->selected_count;
            if (state->group_coalesced)
                destination.result.code = AutomationCursorCode::Coalesced;
            state->cursor = state->next_cursor;
            ++result.rendered_lanes;
        }
    }
    return result;
}

} // namespace pulp::playback
