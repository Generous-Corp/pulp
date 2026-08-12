#include <pulp/playback/automation_cursor.hpp>
#include <pulp/playback/chord_pattern_renderer.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/playback/program_wire.hpp>
#include <pulp/playback/track_automation_program.hpp>
#include <pulp/playback/track_automation_renderer.hpp>

#include "harness/scoped_rt_process_probe.hpp"
#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timebase;
using namespace pulp::timeline;

namespace {

template <typename T, typename E> T take(runtime::Result<T, E> result) {
    if (!result)
        std::abort();
    return std::move(result).value();
}

const std::array kTempoPoints{TempoPoint{{0}, 120.0, TempoCurve::LinearInTicks},
                              TempoPoint{{kTicksPerQuarter * 4}, 96.5}};

/// Fixed rather than minted, so the byte golden stays deterministic. A real
/// producer uses program_wire_process_epoch() or its own store-lifetime value.
constexpr std::uint64_t kEpoch = 0xA11CE'0000'0007ull;

std::shared_ptr<const CompiledTempoMap> wire_tempo_map() {
    return shared_compiled_tempo_map(kTempoPoints, RationalRate{48'000, 1});
}

AutomationLane device_lane(std::uint64_t lane_id, std::uint64_t placement_id,
                           std::uint32_t param_id, float value) {
    auto curve = take(AutomationCurve::create(
        {AutomationPoint{
             {lane_id * 10 + 1}, {0}, value, AutomationInterpolation::Continuous, 0.25f},
         AutomationPoint{{lane_id * 10 + 2},
                         {kTicksPerQuarter},
                         value + 0.125f,
                         AutomationInterpolation::Hold,
                         0.0f}}));
    return take(AutomationLane::create({lane_id}, DeviceParameterTarget{{placement_id}, param_id},
                                       std::move(curve)));
}

AutomationLane mixer_lane(std::uint64_t lane_id, TrackMixerParameter parameter, float value) {
    auto curve = take(AutomationCurve::create(
        {AutomationPoint{{lane_id * 10 + 1}, {0}, value, AutomationInterpolation::Continuous, 0.0f},
         AutomationPoint{{lane_id * 10 + 2},
                         {kTicksPerQuarter * 2},
                         value * 0.5f,
                         AutomationInterpolation::Continuous,
                         -0.5f}}));
    return take(AutomationLane::create({lane_id}, TrackMixerTarget{parameter}, std::move(curve)));
}

/// Exercises every section the wire carries at once: notes, note modifiers,
/// clip and device-placement ordering, a device-parameter lane, a track-mixer
/// lane superseding an authored constant, and a bare second track whose ranges
/// are all empty, so every `(first, count)` range has both a populated and an
/// empty case in one payload.
std::shared_ptr<const Project> wire_project(float first_lane_value = 0.25f) {
    std::vector<NoteEvent> notes{
        {{30}, {0}, {kTicksPerQuarter}, 0xffff, 60, 0},
        {{31}, {kTicksPerQuarter}, {kTicksPerQuarter / 2}, 0x4000, 67, 3},
    };
    std::vector<NoteModifier> modifiers;
    NoteModifier chance;
    chance.note_id = {30};
    chance.probability = 0x8000;
    chance.ratchet_count = 3;
    modifiers.push_back(chance);
    NoteModifier every_other;
    every_other.note_id = {31};
    every_other.condition = NoteConditionKind::EveryNth;
    every_other.condition_period = 4;
    every_other.condition_offset = 2;
    modifiers.push_back(every_other);

    auto content = take(MidiContent::create(std::move(notes), std::move(modifiers), 0xC0FFEEull));
    TrackInput automated;
    automated.id = {10};
    automated.name = "automated";
    automated.clips.push_back(
        take(Clip::create({20}, {0}, {kTicksPerQuarter * 4}, std::move(content))));
    automated.device_chain = {{{40}}, {{41}}};
    automated.automation_lanes.push_back(device_lane(50, 41, 7, first_lane_value));
    automated.automation_lanes.push_back(device_lane(51, 40, 9, 0.75f));
    automated.automation_lanes.push_back(mixer_lane(52, TrackMixerParameter::Gain, 0.5f));
    automated.mixer = {0.75f, -0.25f};

    auto plain = take(Track::create({11}, "plain", {}));
    auto sequence = take(Sequence::create(
        {2}, "root", std::nullopt,
        std::vector<Track>{take(Track::create(std::move(automated))), std::move(plain)}));
    ProjectInput input;
    input.id = {1};
    input.name = "wire";
    input.next_item_id = 1000;
    input.root_sequence_id = {2};
    input.sequences.push_back(std::move(sequence));
    return std::make_shared<const Project>(take(Project::create(std::move(input))));
}

SchemaRegistry production_schemas() {
    SchemaRegistryBuilder builder;
    REQUIRE(register_chord_pattern_content_schema(builder));
    return take(std::move(builder).build());
}

std::shared_ptr<const Project> nondefault_production_project(const SchemaRegistry& schemas) {
    auto content = take(create_chord_pattern_content(
        {.seed = 0, .step = {120}, .gate = {90}, .octave = 4, .velocity = 32000}, schemas));
    auto clip = take(Clip::create({20}, {0}, {480}, std::move(content)));
    auto track = take(Track::create({10}, "best effort", {std::move(clip)}));
    SequenceInput sequence;
    sequence.id = {2};
    sequence.name = "production";
    sequence.tracks = {std::move(track)};
    sequence.chord_scale_lane =
        take(ChordScaleLane::create({{{0}, ChordQuality::Major, 0, ScaleMode::Major, 0}}));
    ProjectInput project;
    project.id = {1};
    project.name = "nondefault production";
    project.next_item_id = 100;
    project.root_sequence_id = {2};
    project.sequences = {take(Sequence::create(std::move(sequence)))};
    return std::make_shared<const Project>(take(Project::create(std::move(project))));
}

std::shared_ptr<const CompileContextRegistry>
nondefault_production_registry(const SchemaRegistry& schemas) {
    CompileContextRegistry declared;
    REQUIRE_FALSE(declare_chord_pattern_renderer(declared, schemas));
    const auto* found =
        declared.find({kChordPatternContentType, kChordPatternContentSchemaVersion});
    REQUIRE(found != nullptr);
    auto registration = *found;
    registration.production.reproducibility = ReproducibilityClass::BestEffort;
    auto result = std::make_shared<CompileContextRegistry>();
    REQUIRE_FALSE(result->declare(std::move(registration), schemas));
    return result;
}

/// Six automation lanes on one track. Six is not arbitrary: six lane records at
/// this version's 56 bytes is 336, which divides exactly by the previous
/// version's 48, so it is the smallest count at which a version 1 reader's
/// stride arithmetic produces a whole number of records instead of a remainder.
/// That is the case the version gate has to stop, so it is the case to build.
std::shared_ptr<const Project> six_lane_project() {
    TrackInput automated;
    automated.id = {10};
    automated.name = "six";
    automated.device_chain = {{{40}}, {{41}}};
    for (std::uint32_t i = 0; i < 6; ++i)
        automated.automation_lanes.push_back(
            device_lane(50 + i, (i % 2 == 0) ? 40 : 41, 7 + i, 0.25f));

    auto sequence = take(Sequence::create(
        {2}, "root", std::nullopt, std::vector<Track>{take(Track::create(std::move(automated)))}));
    ProjectInput input;
    input.id = {1};
    input.name = "six";
    input.next_item_id = 1000;
    input.root_sequence_id = {2};
    input.sequences.push_back(std::move(sequence));
    return std::make_shared<const Project>(take(Project::create(std::move(input))));
}

/// One track playing one audio clip, which is the program shape this version of
/// the wire deliberately does not represent.
std::shared_ptr<const Project> audio_project(std::uint64_t frames) {
    const auto hash = ContentHash::from_hex(std::string(64, 'a'));
    REQUIRE(hash);
    MediaAsset asset;
    asset.id = {60};
    asset.name = "tone";
    asset.frame_count = frames;
    asset.sample_rate = {48'000, 1};
    asset.content_hash = *hash;

    auto clip =
        take(Clip::create_absolute({21}, {0}, frames, {48'000, 1}, MediaRef{{60}, {0}, frames}));
    auto track = take(Track::create({12}, "audio", {std::move(clip)}));
    auto sequence = take(Sequence::create({2}, "root", std::nullopt, std::nullopt,
                                          std::vector<Track>{std::move(track)}));
    ProjectInput input;
    input.id = {1};
    input.name = "audio";
    input.next_item_id = 1000;
    input.root_sequence_id = {2};
    input.assets.push_back(std::move(asset));
    input.sequences.push_back(std::move(sequence));
    return std::make_shared<const Project>(take(Project::create(std::move(input))));
}

std::shared_ptr<const DecodedAudioAssetPool> audio_pool(std::uint64_t frames) {
    auto data = std::make_shared<audio::AudioFileData>();
    data->sample_rate = 48'000;
    data->channels.assign(1, std::vector<float>(frames, 0.5f));
    const auto hash = ContentHash::from_hex(std::string(64, 'a'));
    REQUIRE(hash);
    return take(DecodedAudioAssetPool::create({DecodedAudioAsset{{60}, std::move(data), *hash}}));
}

/// Compiles a program and keeps it alive for the duration of a test, since a
/// decoded view borrows nothing from the program but every comparison does.
struct CompiledProgram {
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler{store, executor, std::chrono::microseconds(0)};

    /// `tempo_map` defaults to a fresh map. Pass one to make two compiles share
    /// it: AutomationCursor refuses a program whose map is not the transport's,
    /// so a case that judges two separately-compiled programs with one cursor
    /// has to hand them one map or it never gets past TempoMapMismatch.
    explicit CompiledProgram(
        std::shared_ptr<const Project> project,
        std::shared_ptr<const DecodedAudioAssetPool> assets = nullptr,
        std::shared_ptr<const CompiledTempoMap> tempo_map = nullptr,
        std::shared_ptr<const CompileContextRegistry> content_compilers = nullptr) {
        ProgramCompileRequest request;
        request.project = std::move(project);
        request.sequence_id = {2};
        request.tempo_map = tempo_map ? std::move(tempo_map) : wire_tempo_map();
        request.sample_rate = request.tempo_map->sample_rate();
        request.document_revision = 7;
        request.dirty = {.all = true};
        request.audio_assets = std::move(assets);
        request.content_compilers = std::move(content_compilers);
        REQUIRE(compiler.submit(std::move(request)));
        while (compiler.status().busy)
            executor.run_for(std::chrono::seconds(1), 64);
        REQUIRE_FALSE(compiler.status().has_error);
        REQUIRE(store.has_value());
    }
};

/// An eight-byte-aligned byte buffer, which is what the format requires of any
/// producer or consumer that wants typed spans borrowed out of it.
class WireBuffer {
  public:
    explicit WireBuffer(std::size_t bytes)
        : storage_(new (std::align_val_t{kProgramWireAlignment}) std::byte[bytes]), size_(bytes) {}
    ~WireBuffer() {
        ::operator delete[](storage_, std::align_val_t{kProgramWireAlignment});
    }
    WireBuffer(const WireBuffer&) = delete;
    WireBuffer& operator=(const WireBuffer&) = delete;

    std::span<std::byte> span() noexcept {
        return {storage_, size_};
    }
    std::span<const std::byte> span() const noexcept {
        return {storage_, size_};
    }

  private:
    std::byte* storage_;
    std::size_t size_;
};

/// Encodes the shared fixture once, so a corruption test can restore the byte
/// it damaged and prove the payload was otherwise sound.
struct EncodedFixture {
    CompiledProgram compiled;
    PlaybackProgramStore::ReadGuard program = compiled.store.read();
    std::size_t size = take(program_wire_encoded_size(*program, kTempoPoints));
    WireBuffer buffer{size};

    explicit EncodedFixture(std::shared_ptr<const CompiledTempoMap> tempo_map = nullptr)
        : compiled(wire_project(), nullptr, std::move(tempo_map)) {
        REQUIRE(take(encode_program_wire(*program, kTempoPoints, kEpoch, buffer.span())) == size);
        REQUIRE(decode_program_wire(buffer.span()));
    }

    std::span<std::byte> bytes() noexcept {
        return buffer.span();
    }
    std::span<const std::byte> bytes() const noexcept {
        return buffer.span();
    }

    /// Overwrites one field, asserts the decoder's specific typed rejection,
    /// restores the original bytes, and asserts the payload decodes again. The
    /// restore is the negative control: without it a "rejection" could be a
    /// payload that never decoded in the first place.
    template <typename Field>
    void corrupt(std::size_t offset, Field replacement, ProgramWireErrorCode expected) {
        Field original{};
        std::memcpy(&original, bytes().data() + offset, sizeof(Field));
        std::memcpy(bytes().data() + offset, &replacement, sizeof(Field));
        auto rejected = decode_program_wire(bytes());
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.error().code == expected);
        std::memcpy(bytes().data() + offset, &original, sizeof(Field));
        REQUIRE(decode_program_wire(bytes()));
    }
};

std::unique_ptr<WireBuffer>
encode_project_bytes(std::shared_ptr<const Project> project,
                     const std::shared_ptr<const CompiledTempoMap>& tempo_map,
                     std::uint64_t epoch = kEpoch) {
    CompiledProgram compiled{std::move(project), nullptr, tempo_map};
    const auto program = compiled.store.read();
    const auto size = take(program_wire_encoded_size(*program, kTempoPoints));
    auto buffer = std::make_unique<WireBuffer>(size);
    REQUIRE(take(encode_program_wire(*program, kTempoPoints, epoch, buffer->span())) == size);
    return buffer;
}

constexpr std::size_t kDirectoryAt = sizeof(ProgramWireHeader);
constexpr std::size_t kPayloadAt = kDirectoryAt + 9 * sizeof(ProgramWireSectionEntry);

/// Rewrites the header's checksum over the current directory and payload, for
/// tests that mean to corrupt a field rather than the checksum guarding it.
void reseal(std::span<std::byte> bytes) {
    ProgramWireHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    header.body_checksum = program_wire_checksum(bytes.subspan(kDirectoryAt));
    std::memcpy(bytes.data(), &header, sizeof(header));
}

std::size_t section_entry_at(std::size_t index) {
    return kDirectoryAt + index * sizeof(ProgramWireSectionEntry);
}

/// One section's payload offset and byte length, read out of the directory
/// rather than recomputed from record sizes, so a test that reaches into a
/// section keeps targeting it when an earlier section's width changes.
std::pair<std::size_t, std::size_t> section_span(std::span<const std::byte> bytes,
                                                 ProgramWireSection section) {
    ProgramWireHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    for (std::size_t i = 0; i < header.section_count; ++i) {
        ProgramWireSectionEntry entry;
        std::memcpy(&entry, bytes.data() + section_entry_at(i), sizeof(entry));
        if (entry.id == static_cast<std::uint32_t>(section))
            return {kPayloadAt + static_cast<std::size_t>(entry.offset),
                    static_cast<std::size_t>(entry.bytes)};
    }
    std::abort();
}

std::size_t lane_token_at(std::span<const std::byte> bytes, std::size_t lane) {
    return section_span(bytes, ProgramWireSection::AutomationLanes).first +
           lane * sizeof(ProgramWireAutomationLaneRecord) +
           offsetof(ProgramWireAutomationLaneRecord, instance_token);
}

/// What normalize_instance_tokens() writes into lane `index`.
///
/// A high sentinel rather than the ordinal, and the difference matters: in a
/// fresh process the fixture's three lanes really are tokens 1, 2 and 3, so
/// normalising to ordinals there is a no-op — and a normalisation that wrote to
/// the wrong offset entirely would be indistinguishable from one that worked.
/// The global counter would need ~1.8e19 compiles to reach these values, so
/// observing them can only mean the write landed in the right field. Nonzero,
/// so the normalised payload still decodes.
constexpr std::uint64_t normalized_token(std::size_t index) noexcept {
    return 0x7E57'0000'0000'0001ull + index;
}

/// Rewrites every lane's instance token to its sentinel and reseals. The token
/// is minted per compile, so it is the one field a fixed document does not fix;
/// a digest over a fixed document has to pin it or it would record how many
/// programs this process happened to compile first. A value is written rather
/// than the bytes skipped so the field's offset and width stay inside the
/// digest's coverage.
void normalize_instance_tokens(std::span<std::byte> bytes) {
    const auto [offset, length] = section_span(bytes, ProgramWireSection::AutomationLanes);
    for (std::size_t i = 0; i < length / sizeof(ProgramWireAutomationLaneRecord); ++i) {
        const auto sentinel = normalized_token(i);
        std::memcpy(bytes.data() + lane_token_at(bytes, i), &sentinel, sizeof(sentinel));
    }
    reseal(bytes);
}

} // namespace

TEST_CASE("program wire round trips a compiled program to a structurally equal view",
          "[playback][wire]") {
    EncodedFixture fixture;
    auto decoded = decode_program_wire(fixture.bytes());
    REQUIRE(decoded);
    const auto& view = decoded.value();

    REQUIRE(program_wire_matches(view, *fixture.program, kTempoPoints, kEpoch));

    // Spot-check the structure the equality walk covers, so a matches() that
    // silently returned true would still be caught here.
    REQUIRE(view.header().version == kProgramWireVersion);
    REQUIRE(view.producer_epoch() == kEpoch);
    REQUIRE(view.program().generation == fixture.program->generation());
    REQUIRE(view.program().document_revision == 7);
    REQUIRE(view.sample_rate() == RationalRate{48'000, 1});
    REQUIRE(view.automation_limits() == fixture.program->automation_limits());
    REQUIRE(view.tempo_points().size() == kTempoPoints.size());
    REQUIRE(view.tempo_points()[1].bpm_bits == std::bit_cast<std::uint64_t>(96.5));
    REQUIRE(view.tracks().size() == 2);

    const auto& automated = view.tracks()[0];
    REQUIRE(automated.id == 10);
    REQUIRE((automated.flags & kProgramWireTrackHasAutomationProgram) != 0);
    REQUIRE(view.clip_ids_for(automated).size() == 1);
    REQUIRE(view.clip_ids_for(automated)[0].value == 20);
    // Note 30 ratchets three times, so it lowers to three on/off pairs.
    REQUIRE(view.note_events_for(automated).size() == 8);
    REQUIRE(view.note_modifiers_for(automated).size() == 2);
    REQUIRE(view.device_placement_ids_for(automated).size() == 2);
    REQUIRE(view.automation_lanes_for(automated).size() == 3);
    REQUIRE(automated.mixer_pan == -0.25f);
    // The authored gain constant travels alongside the lane that supersedes it,
    // so a reader can tell a superseded constant from an absent one.
    REQUIRE(automated.mixer_gain_linear == 0.75f);
    REQUIRE(automated.mixer_gain_lane != kProgramWireNoLane);
    REQUIRE(automated.mixer_pan_lane == kProgramWireNoLane);

    const auto& gain_lane = view.automation_lanes_for(automated)[automated.mixer_gain_lane];
    REQUIRE(gain_lane.target_kind == static_cast<std::uint8_t>(ProgramWireTargetKind::TrackMixer));
    REQUIRE(gain_lane.mixer_parameter == static_cast<std::uint8_t>(TrackMixerParameter::Gain));
    REQUIRE(gain_lane.evaluation_rate ==
            static_cast<std::uint8_t>(AutomationEvaluationRate::SampleAccurate));
    REQUIRE_FALSE(view.segments_for(gain_lane).empty());

    // A lane that drives a device parameter carries both domains, which is what
    // lets the adopting realm place a sample-accurate event without the map.
    const ProgramWireAutomationLaneRecord* device = nullptr;
    for (const auto& lane : view.automation_lanes_for(automated))
        if (lane.target_kind == static_cast<std::uint8_t>(ProgramWireTargetKind::DeviceParameter))
            device = &lane;
    REQUIRE(device != nullptr);
    REQUIRE(device->device_placement_id != 0);
    const auto segments = view.segments_for(*device);
    REQUIRE_FALSE(segments.empty());
    REQUIRE(segments.front().end_sample >= segments.front().start_sample);
    REQUIRE(segments.front().end_tick >= segments.front().start_tick);

    // A track with no lanes still carries an automation program, just an empty
    // one. The wire keeps "empty" and "absent" distinguishable rather than
    // folding them into a lane count of zero, because a producer other than
    // this compiler can publish either.
    const auto& plain = view.tracks()[1];
    REQUIRE(plain.id == 11);
    REQUIRE((plain.flags & kProgramWireTrackHasAutomationProgram) != 0);
    REQUIRE(view.automation_lanes_for(plain).empty());
    REQUIRE(view.note_events_for(plain).empty());
    REQUIRE(view.clip_ids_for(plain).empty());
    REQUIRE(plain.mixer_gain_lane == kProgramWireNoLane);
}

TEST_CASE("program wire encoding is canonical and byte stable", "[playback][wire]") {
    EncodedFixture fixture;

    // The layout is derivable from the record sizes and the counts, so a size
    // that drifts is a layout change rather than a recorded accident.
    const auto counted =
        kPayloadAt + sizeof(ProgramWireProgramRecord) + 2 * sizeof(ProgramWireTempoPointRecord) +
        2 * sizeof(ProgramWireTrackRecord) + 1 * sizeof(ProgramWireIdRecord) +
        8 * sizeof(ProgramWireNoteEventRecord) + 2 * sizeof(ProgramWireNoteModifierRecord) +
        2 * sizeof(ProgramWireIdRecord) + 3 * sizeof(ProgramWireAutomationLaneRecord) +
        6 * sizeof(ProgramWireAutomationSegmentRecord);
    REQUIRE(fixture.size == counted);

    // The header's first bytes are asserted by hand rather than by digest, so
    // the golden below is anchored to a layout someone decided rather than to
    // whatever the encoder emitted.
    const auto bytes = fixture.bytes();
    REQUIRE(std::to_integer<char>(bytes[0]) == 'P');
    REQUIRE(std::to_integer<char>(bytes[1]) == 'L');
    REQUIRE(std::to_integer<char>(bytes[2]) == 'P');
    REQUIRE(std::to_integer<char>(bytes[3]) == 'W');
    ProgramWireHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    REQUIRE(header.magic == kProgramWireMagic);
    REQUIRE(header.version == kProgramWireVersion);
    REQUIRE(header.min_reader_version == kProgramWireMinReaderVersion);
    REQUIRE(header.header_bytes == sizeof(ProgramWireHeader));
    REQUIRE(header.section_count == 9);
    REQUIRE(header.payload_bytes == fixture.size - kPayloadAt);
    REQUIRE(header.reserved0 == 0);
    REQUIRE(header.body_checksum == program_wire_checksum(bytes.subspan(kDirectoryAt)));

    // Sections appear in ascending id order and tile the payload exactly.
    std::uint64_t tiled = 0;
    for (std::size_t i = 0; i < 9; ++i) {
        ProgramWireSectionEntry entry;
        std::memcpy(&entry, bytes.data() + section_entry_at(i), sizeof(entry));
        REQUIRE(entry.id == i + 1);
        REQUIRE(entry.flags == 0);
        REQUIRE(entry.offset == tiled);
        REQUIRE(entry.offset % kProgramWireAlignment == 0);
        tiled = entry.offset + entry.bytes;
    }
    REQUIRE(tiled == header.payload_bytes);

    // Re-encoding the same program produces the identical byte range: there is
    // exactly one encoding of one program, which is what makes the digest below
    // a guard rather than a record.
    WireBuffer again(fixture.size);
    REQUIRE(take(encode_program_wire(*fixture.program, kTempoPoints, kEpoch, again.span())) ==
            fixture.size);
    REQUIRE(std::memcmp(again.span().data(), bytes.data(), fixture.size) == 0);

    // A whole-payload digest for a fixed input. It moves only when the layout,
    // the field order, or the compiler's lowering does; any of those is a
    // deliberate change that has to be re-agreed here rather than silently
    // between the encoder and the decoder.
    //
    // One field of a fixed document is not fixed: each lane's instance token is
    // minted per compile, so it counts how many programs this process built
    // first and would make the digest depend on test order. It is pinned to a
    // sentinel — not dropped, so its offset and width stay covered.
    WireBuffer canonical(fixture.size);
    std::memcpy(canonical.span().data(), bytes.data(), fixture.size);
    normalize_instance_tokens(canonical.span());
    // Control on the normalisation itself: read the tokens back through the
    // decoder and require the sentinel. A wrong offset would leave the raw
    // tokens in place and the digest below would fail with no explanation. The
    // sentinel is checked rather than "the bytes changed" because when this case
    // runs alone the raw tokens are 1, 2, 3 and a change is not guaranteed.
    auto normalized = decode_program_wire(canonical.span());
    REQUIRE(normalized);
    const auto normalized_lanes = normalized.value().automation_lanes();
    REQUIRE(normalized_lanes.size() == 3);
    for (std::size_t i = 0; i < normalized_lanes.size(); ++i)
        REQUIRE(normalized_lanes[i].instance_token == normalized_token(i));

    // Normalising the token's VALUE takes it out of the digest's reach, so the
    // digest alone no longer guards where that field sits. Pin the offset and
    // width here instead, or moving the token within the record — or widening
    // it — would pass both the golden and every assertion above.
    REQUIRE(offsetof(ProgramWireAutomationLaneRecord, instance_token) == 16);
    REQUIRE(sizeof(ProgramWireAutomationLaneRecord::instance_token) == 8);
    REQUIRE(lane_token_at(canonical.span(), 0) ==
            section_span(canonical.span(), ProgramWireSection::AutomationLanes).first + 16);
    REQUIRE(program_wire_checksum(canonical.span()) == 0xd4f6b232ebb9d781ull);
}

TEST_CASE("program wire rejects a malformed generation header", "[playback][wire]") {
    EncodedFixture fixture;

    fixture.corrupt(offsetof(ProgramWireHeader, magic), std::uint32_t{0xDEADBEEF},
                    ProgramWireErrorCode::BadMagic);
    fixture.corrupt(offsetof(ProgramWireHeader, min_reader_version),
                    static_cast<std::uint16_t>(kProgramWireVersion + 1),
                    ProgramWireErrorCode::UnsupportedVersion);
    fixture.corrupt(offsetof(ProgramWireHeader, header_bytes), std::uint32_t{48},
                    ProgramWireErrorCode::BadHeaderSize);
    fixture.corrupt(offsetof(ProgramWireHeader, body_checksum), std::uint64_t{0},
                    ProgramWireErrorCode::ChecksumMismatch);
    fixture.corrupt(offsetof(ProgramWireHeader, payload_bytes),
                    static_cast<std::uint64_t>(fixture.size), ProgramWireErrorCode::ShortBuffer);

    // The writer's own version may run ahead of this reader, as long as the
    // writer states this reader can still read the bytes. That asymmetry is the
    // whole migration story, so it is asserted rather than assumed.
    const auto ahead = static_cast<std::uint16_t>(kProgramWireVersion + 1);
    std::memcpy(fixture.bytes().data() + offsetof(ProgramWireHeader, version), &ahead,
                sizeof(ahead));
    REQUIRE(decode_program_wire(fixture.bytes()));
    REQUIRE(decode_program_wire(fixture.bytes()).value().header().version == ahead);
}

TEST_CASE("program wire rejects truncation and trailing data", "[playback][wire]") {
    EncodedFixture fixture;

    for (const std::size_t kept : {std::size_t{0}, std::size_t{16}, sizeof(ProgramWireHeader),
                                   kPayloadAt, fixture.size - 8, fixture.size - 1}) {
        auto rejected = decode_program_wire(fixture.bytes().first(kept));
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.error().code == ProgramWireErrorCode::ShortBuffer);
    }

    WireBuffer longer(fixture.size + 8);
    std::memcpy(longer.span().data(), fixture.bytes().data(), fixture.size);
    std::memset(longer.span().data() + fixture.size, 0, 8);
    auto trailing = decode_program_wire(longer.span());
    REQUIRE_FALSE(trailing);
    REQUIRE(trailing.error().code == ProgramWireErrorCode::TrailingData);

    // Negative control for the whole case: the untruncated payload decodes.
    REQUIRE(decode_program_wire(fixture.bytes()));
}

TEST_CASE("program wire rejects an inconsistent section directory", "[playback][wire]") {
    EncodedFixture fixture;
    const auto bytes = fixture.bytes();

    const auto read_entry = [&](std::size_t index) {
        ProgramWireSectionEntry entry;
        std::memcpy(&entry, bytes.data() + section_entry_at(index), sizeof(entry));
        return entry;
    };
    const auto write_entry = [&](std::size_t index, const ProgramWireSectionEntry& entry) {
        std::memcpy(bytes.data() + section_entry_at(index), &entry, sizeof(entry));
    };

    // Patches one or two adjacent directory entries, asserts the specific typed
    // rejection, then restores and asserts the payload decodes again.
    const auto with_entries = [&](std::size_t index, ProgramWireSectionEntry first,
                                  const ProgramWireSectionEntry* second,
                                  ProgramWireErrorCode expected) {
        const auto original_first = read_entry(index);
        const auto original_second = second ? read_entry(index + 1) : ProgramWireSectionEntry{};
        write_entry(index, first);
        if (second)
            write_entry(index + 1, *second);
        reseal(bytes);
        auto rejected = decode_program_wire(bytes);
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.error().code == expected);
        write_entry(index, original_first);
        if (second)
            write_entry(index + 1, original_second);
        reseal(bytes);
        REQUIRE(decode_program_wire(bytes));
    };

    // Index 2 is the Tracks section; index 3 is ClipIds, which follows it.
    const auto tracks = read_entry(2);
    const auto clips = read_entry(3);

    auto beyond = tracks;
    beyond.bytes = std::uint64_t{0} - 8;
    with_entries(2, beyond, nullptr, ProgramWireErrorCode::SectionOutOfBounds);

    // A length that is not a whole number of records is caught on the entry
    // itself, before the tiling break it would cause in the next one.
    auto overlong = tracks;
    overlong.bytes += 8;
    with_entries(2, overlong, nullptr, ProgramWireErrorCode::BadRecordSize);

    auto gapped = tracks;
    gapped.offset += 8;
    with_entries(2, gapped, nullptr, ProgramWireErrorCode::SectionsNotTiled);

    // Dropping a whole record off the last section leaves every entry
    // individually well formed and the payload short by one record.
    auto short_tail = read_entry(8);
    short_tail.bytes -= sizeof(ProgramWireAutomationSegmentRecord);
    with_entries(8, short_tail, nullptr, ProgramWireErrorCode::SectionsNotTiled);

    auto duplicated = tracks;
    duplicated.id = 2;
    with_entries(2, duplicated, nullptr, ProgramWireErrorCode::DuplicateSection);

    auto unknown = tracks;
    unknown.id = 4242;
    with_entries(2, unknown, nullptr, ProgramWireErrorCode::UnknownSection);

    // ClipIds and DevicePlacementIds have the same record size. Exchanging
    // only their ids therefore survives size and tiling checks and reaches the
    // canonical known-section ordering guard.
    const auto clips_original = read_entry(3);
    const auto placements_original = read_entry(6);
    auto clips_as_placements = clips_original;
    auto placements_as_clips = placements_original;
    clips_as_placements.id = placements_original.id;
    placements_as_clips.id = clips_original.id;
    write_entry(3, clips_as_placements);
    write_entry(6, placements_as_clips);
    reseal(bytes);
    auto reordered = decode_program_wire(bytes);
    REQUIRE_FALSE(reordered);
    REQUIRE(reordered.error().code == ProgramWireErrorCode::NonCanonicalSectionOrder);
    write_entry(3, clips_original);
    write_entry(6, placements_original);
    reseal(bytes);
    REQUIRE(decode_program_wire(bytes));

    // Only an unknown optional section can carry a length that is not a
    // multiple of eight, so that is the one way a following section's offset
    // can land unaligned while the payload still tiles.
    auto opaque = tracks;
    opaque.id = 9002;
    opaque.flags = kProgramWireSectionOptional;
    opaque.bytes -= 4;
    auto shifted_clips = clips;
    shifted_clips.offset -= 4;
    shifted_clips.bytes += 4;
    with_entries(2, opaque, &shifted_clips, ProgramWireErrorCode::SectionMisaligned);
}

TEST_CASE("program wire skips an unknown section only when the writer marked it optional",
          "[playback][wire]") {
    EncodedFixture fixture;
    const auto bytes = fixture.bytes();

    // The empty note-modifier-free second track leaves no zero-length section
    // to borrow, so the clip-id section stands in: renaming it to an unknown id
    // is exactly the shape a future writer's added section has.
    ProgramWireSectionEntry original;
    std::memcpy(&original, bytes.data() + section_entry_at(3), sizeof(original));

    auto optional_unknown = original;
    optional_unknown.id = 9001;
    optional_unknown.flags = kProgramWireSectionOptional;
    std::memcpy(bytes.data() + section_entry_at(3), &optional_unknown, sizeof(optional_unknown));
    reseal(bytes);
    auto skipped = decode_program_wire(bytes);
    // Skipping is not the same as accepting a program with a section missing:
    // the id it displaced is a known one, so the payload is still incomplete.
    REQUIRE_FALSE(skipped);
    REQUIRE(skipped.error().code == ProgramWireErrorCode::MissingSection);
    REQUIRE(skipped.error().section == static_cast<std::uint32_t>(ProgramWireSection::ClipIds));

    auto required_unknown = original;
    required_unknown.id = 9001;
    required_unknown.flags = 0;
    std::memcpy(bytes.data() + section_entry_at(3), &required_unknown, sizeof(required_unknown));
    reseal(bytes);
    auto refused = decode_program_wire(bytes);
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error().code == ProgramWireErrorCode::UnknownSection);

    std::memcpy(bytes.data() + section_entry_at(3), &original, sizeof(original));
    reseal(bytes);
    REQUIRE(decode_program_wire(bytes));
}

TEST_CASE("optional unknown sections participate in canonical directory order",
          "[playback][wire]") {
    EncodedFixture fixture;
    WireBuffer expanded(fixture.size + sizeof(ProgramWireSectionEntry));
    ProgramWireHeader header;
    std::memcpy(&header, fixture.bytes().data(), sizeof(header));
    ++header.section_count;
    const auto expanded_payload_at =
        sizeof(ProgramWireHeader) + header.section_count * sizeof(ProgramWireSectionEntry);
    std::memcpy(expanded.span().data() + sizeof(ProgramWireHeader),
                fixture.bytes().data() + sizeof(ProgramWireHeader),
                9 * sizeof(ProgramWireSectionEntry));
    ProgramWireSectionEntry optional;
    optional.id = 0;
    optional.flags = kProgramWireSectionOptional;
    optional.offset = header.payload_bytes;
    std::memcpy(expanded.span().data() + sizeof(ProgramWireHeader) +
                    9 * sizeof(ProgramWireSectionEntry),
                &optional, sizeof(optional));
    std::memcpy(expanded.span().data() + expanded_payload_at, fixture.bytes().data() + kPayloadAt,
                header.payload_bytes);
    header.body_checksum = program_wire_checksum(expanded.span().subspan(sizeof(header)));
    std::memcpy(expanded.span().data(), &header, sizeof(header));
    auto descending = decode_program_wire(expanded.span());
    REQUIRE_FALSE(descending);
    REQUIRE(descending.error().code == ProgramWireErrorCode::NonCanonicalSectionOrder);

    optional.id = 10;
    std::memcpy(expanded.span().data() + sizeof(ProgramWireHeader) +
                    9 * sizeof(ProgramWireSectionEntry),
                &optional, sizeof(optional));
    header.body_checksum = program_wire_checksum(expanded.span().subspan(sizeof(header)));
    std::memcpy(expanded.span().data(), &header, sizeof(header));
    REQUIRE(decode_program_wire(expanded.span()));
}

TEST_CASE("program wire rejects records that index outside their section", "[playback][wire]") {
    EncodedFixture fixture;
    const auto bytes = fixture.bytes();

    const std::size_t track_section =
        kPayloadAt + sizeof(ProgramWireProgramRecord) + 2 * sizeof(ProgramWireTempoPointRecord);
    const auto field = [&](std::size_t member) { return track_section + member; };

    const auto with_field = [&](std::size_t offset, auto replacement,
                                ProgramWireErrorCode expected) {
        decltype(replacement) original{};
        std::memcpy(&original, bytes.data() + offset, sizeof(replacement));
        std::memcpy(bytes.data() + offset, &replacement, sizeof(replacement));
        reseal(bytes);
        auto rejected = decode_program_wire(bytes);
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.error().code == expected);
        std::memcpy(bytes.data() + offset, &original, sizeof(original));
        reseal(bytes);
        REQUIRE(decode_program_wire(bytes));
    };

    with_field(field(offsetof(ProgramWireTrackRecord, note_event_count)),
               std::uint32_t{0xFFFF'FFF0}, ProgramWireErrorCode::RangeOutOfBounds);
    with_field(field(offsetof(ProgramWireTrackRecord, automation_lane_first)), std::uint32_t{99},
               ProgramWireErrorCode::RangeOutOfBounds);
    with_field(track_section + sizeof(ProgramWireTrackRecord) +
                   offsetof(ProgramWireTrackRecord, clip_first),
               std::uint32_t{0}, ProgramWireErrorCode::NonCanonicalRangeOwnership);
    with_field(track_section + sizeof(ProgramWireTrackRecord) +
                   offsetof(ProgramWireTrackRecord, id),
               std::uint64_t{10}, ProgramWireErrorCode::NonCanonicalRangeOwnership);
    with_field(field(offsetof(ProgramWireTrackRecord, mixer_gain_lane)), std::uint32_t{7},
               ProgramWireErrorCode::RangeOutOfBounds);
    with_field(field(offsetof(ProgramWireTrackRecord, provider_selected)), std::uint8_t{9},
               ProgramWireErrorCode::InvalidEnum);
    with_field(field(offsetof(ProgramWireTrackRecord, state_policy)), std::uint8_t{4},
               ProgramWireErrorCode::InvalidEnum);
    with_field(field(offsetof(ProgramWireTrackRecord, flags)), std::uint8_t{0x80},
               ProgramWireErrorCode::InvalidEnum);
    with_field(field(offsetof(ProgramWireTrackRecord, flags)), std::uint8_t{0},
               ProgramWireErrorCode::NonCanonicalRangeOwnership);

    // A record can be individually in range and still describe something no
    // renderer can act on. These are the two that reach past their own record:
    // the period the note renderer divides by, and the endpoints an automation
    // segment is interpolated between.
    const std::size_t modifier_section =
        kPayloadAt + sizeof(ProgramWireProgramRecord) + 2 * sizeof(ProgramWireTempoPointRecord) +
        2 * sizeof(ProgramWireTrackRecord) + 1 * sizeof(ProgramWireIdRecord) +
        8 * sizeof(ProgramWireNoteEventRecord);
    with_field(modifier_section + offsetof(ProgramWireNoteModifierRecord, ratchet_count),
               std::uint16_t{0}, ProgramWireErrorCode::MalformedNoteModifier);
    // The second modifier is the conditional one, so zeroing its period is the
    // divisor the note renderer would otherwise take a modulo against.
    with_field(modifier_section + sizeof(ProgramWireNoteModifierRecord) +
                   offsetof(ProgramWireNoteModifierRecord, condition_period),
               std::uint16_t{0}, ProgramWireErrorCode::MalformedNoteModifier);

    const std::size_t segment_section =
        modifier_section + 2 * sizeof(ProgramWireNoteModifierRecord) +
        2 * sizeof(ProgramWireIdRecord) + 3 * sizeof(ProgramWireAutomationLaneRecord);
    with_field(segment_section + offsetof(ProgramWireAutomationSegmentRecord, curvature),
               std::numeric_limits<float>::quiet_NaN(), ProgramWireErrorCode::MalformedSegment);
    with_field(segment_section + offsetof(ProgramWireAutomationSegmentRecord, end_sample),
               std::int64_t{-1}, ProgramWireErrorCode::MalformedSegment);
    with_field(segment_section + offsetof(ProgramWireAutomationSegmentRecord, end_value), 0.333f,
               ProgramWireErrorCode::MalformedSegment);

    const auto [lane_section, lane_bytes] =
        section_span(bytes, ProgramWireSection::AutomationLanes);
    REQUIRE(lane_bytes >= 2 * sizeof(ProgramWireAutomationLaneRecord));
    with_field(lane_section + offsetof(ProgramWireAutomationLaneRecord, device_placement_id),
               std::uint64_t{999}, ProgramWireErrorCode::AutomationTargetUnresolved);
    const auto second_lane = lane_section + sizeof(ProgramWireAutomationLaneRecord);
    std::uint64_t original_placement = 0;
    std::uint32_t original_param = 0;
    std::memcpy(&original_placement,
                bytes.data() + second_lane +
                    offsetof(ProgramWireAutomationLaneRecord, device_placement_id),
                sizeof(original_placement));
    std::memcpy(&original_param,
                bytes.data() + second_lane +
                    offsetof(ProgramWireAutomationLaneRecord, device_param_id),
                sizeof(original_param));
    ProgramWireAutomationLaneRecord first_lane_record;
    std::memcpy(&first_lane_record, bytes.data() + lane_section, sizeof(first_lane_record));
    const auto duplicate_placement = first_lane_record.device_placement_id;
    const auto duplicate_param = first_lane_record.device_param_id;
    std::memcpy(bytes.data() + second_lane +
                    offsetof(ProgramWireAutomationLaneRecord, device_placement_id),
                &duplicate_placement, sizeof(duplicate_placement));
    std::memcpy(bytes.data() + second_lane +
                    offsetof(ProgramWireAutomationLaneRecord, device_param_id),
                &duplicate_param, sizeof(duplicate_param));
    reseal(bytes);
    auto duplicate_target = decode_program_wire(bytes);
    REQUIRE_FALSE(duplicate_target);
    REQUIRE(duplicate_target.error().code == ProgramWireErrorCode::DuplicateAutomationTarget);
    std::memcpy(bytes.data() + second_lane +
                    offsetof(ProgramWireAutomationLaneRecord, device_placement_id),
                &original_placement, sizeof(original_placement));
    std::memcpy(bytes.data() + second_lane +
                    offsetof(ProgramWireAutomationLaneRecord, device_param_id),
                &original_param, sizeof(original_param));
    reseal(bytes);
    REQUIRE(decode_program_wire(bytes));

    const std::size_t program_section = kPayloadAt;
    with_field(program_section + offsetof(ProgramWireProgramRecord, sample_rate_denominator),
               std::uint64_t{0}, ProgramWireErrorCode::InvalidSampleRate);
    with_field(program_section + offsetof(ProgramWireProgramRecord, max_lanes_per_track),
               std::uint32_t{0}, ProgramWireErrorCode::InvalidLimits);
    with_field(program_section + offsetof(ProgramWireProgramRecord, max_points_per_lane),
               std::uint32_t{0xFFFF'FFFF}, ProgramWireErrorCode::InvalidLimits);
    with_field(program_section + offsetof(ProgramWireProgramRecord, max_lanes_per_track),
               std::uint32_t{2}, ProgramWireErrorCode::InvalidLimits);
    with_field(program_section + offsetof(ProgramWireProgramRecord, max_points_per_lane),
               std::uint32_t{1}, ProgramWireErrorCode::InvalidLimits);
}

TEST_CASE("program wire encoder refuses what it cannot represent", "[playback][wire]") {
    CompiledProgram compiled{wire_project()};
    const auto program = compiled.store.read();

    // Tempo points that did not compile this map are refused rather than
    // written, because the wire carries the points and a reader would compile a
    // different map from them without ever noticing.
    // The exclusion that matters most: a track carrying audio is refused, not
    // encoded thinner. A wire that silently dropped it would be
    // indistinguishable downstream from a project that has no audio at all.
    constexpr std::uint64_t kAudioFrames = 4'800;
    CompiledProgram with_audio{audio_project(kAudioFrames), audio_pool(kAudioFrames)};
    const auto audio = with_audio.store.read();
    REQUIRE(audio->find_track({12}) != nullptr);
    REQUIRE(audio->find_track({12})->audio_program() != nullptr);
    auto refused = program_wire_encoded_size(*audio, kTempoPoints);
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error().code == ProgramWireErrorCode::AudioProgramUnsupported);
    REQUIRE(refused.error().detail == 12);

    const auto schemas = production_schemas();
    CompiledProgram with_nondefault_production{nondefault_production_project(schemas), nullptr,
                                               nullptr, nondefault_production_registry(schemas)};
    const auto nondefault = with_nondefault_production.store.read();
    REQUIRE(nondefault->find_track({10}) != nullptr);
    REQUIRE(nondefault->find_track({10})->arrangement_production().reproducibility ==
            ReproducibilityClass::BestEffort);
    auto production_refused = program_wire_encoded_size(*nondefault, kTempoPoints);
    REQUIRE_FALSE(production_refused);
    REQUIRE(production_refused.error().code ==
            ProgramWireErrorCode::ProductionDeclarationUnsupported);
    REQUIRE(production_refused.error().detail == 10);
    // And the refusal is the encoder's, not this fixture's: the same call on a
    // program without audio succeeds.
    REQUIRE(program_wire_encoded_size(*program, kTempoPoints));

    const std::array wrong{TempoPoint{{0}, 140.0}};
    auto mismatched = program_wire_encoded_size(*program, wrong);
    REQUIRE_FALSE(mismatched);
    REQUIRE(mismatched.error().code == ProgramWireErrorCode::TempoMapMismatch);
    auto empty = program_wire_encoded_size(*program, {});
    REQUIRE_FALSE(empty);
    REQUIRE(empty.error().code == ProgramWireErrorCode::TempoMapMismatch);

    const auto size = take(program_wire_encoded_size(*program, kTempoPoints));

    WireBuffer small(size - 1);
    auto cramped = encode_program_wire(*program, kTempoPoints, kEpoch, small.span());
    REQUIRE_FALSE(cramped);
    REQUIRE(cramped.error().code == ProgramWireErrorCode::InsufficientCapacity);
    REQUIRE(cramped.error().detail == size);

    WireBuffer aligned(size + kProgramWireAlignment);
    auto skewed =
        encode_program_wire(*program, kTempoPoints, kEpoch, aligned.span().subspan(4, size));
    REQUIRE_FALSE(skewed);
    REQUIRE(skewed.error().code == ProgramWireErrorCode::MisalignedBuffer);

    // Negative control: the same program at the same size on an aligned buffer
    // encodes, so the three refusals above are the specific defects and not a
    // program the encoder could never write.
    WireBuffer exact(size);
    REQUIRE(take(encode_program_wire(*program, kTempoPoints, kEpoch, exact.span())) == size);

    auto misread = decode_program_wire(aligned.span().subspan(4, size));
    REQUIRE_FALSE(misread);
    REQUIRE(misread.error().code == ProgramWireErrorCode::MisalignedBuffer);
}

TEST_CASE("program wire carries producer identity alongside the generation", "[playback][wire]") {
    EncodedFixture fixture;

    // The defect this closes: `generation` is minted per store and restarts, so
    // a producer that is torn down and recreated republishes generation 1 while
    // a surviving consumer sits at N. Two producers of one document are
    // therefore indistinguishable on `(project_id, sequence_id, generation)`
    // alone — every field below matches — and only the epoch separates them.
    CompiledProgram rebuilt{wire_project()};
    const auto restarted = rebuilt.store.read();
    REQUIRE(restarted->generation() == fixture.program->generation());
    REQUIRE(restarted->project_id() == fixture.program->project_id());
    REQUIRE(restarted->sequence_id() == fixture.program->sequence_id());

    constexpr std::uint64_t kOtherEpoch = 0xB0B0'0000'0009ull;
    WireBuffer other(fixture.size);
    REQUIRE(take(encode_program_wire(*restarted, kTempoPoints, kOtherEpoch, other.span())) ==
            fixture.size);

    auto first = decode_program_wire(fixture.bytes());
    auto second = decode_program_wire(other.span());
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(first.value().producer_epoch() != second.value().producer_epoch());
    REQUIRE(first.value().program().generation == second.value().program().generation);

    // The two payloads differ only in the epoch, which is what makes the epoch
    // the sole thing standing between "same publication" and "new producer".
    REQUIRE(std::memcmp(fixture.bytes().data(), other.span().data(), fixture.size) != 0);
    ProgramWireProgramRecord a{};
    ProgramWireProgramRecord b{};
    std::memcpy(&a, fixture.bytes().data() + kPayloadAt, sizeof(a));
    std::memcpy(&b, other.span().data() + kPayloadAt, sizeof(b));
    a.producer_epoch = b.producer_epoch;
    REQUIRE(std::memcmp(&a, &b, sizeof(a)) == 0);

    // A zero epoch would compare equal to every other zero, so it is refused at
    // both ends rather than acting as a wildcard.
    WireBuffer unstamped(fixture.size);
    auto unidentified = encode_program_wire(*fixture.program, kTempoPoints, 0, unstamped.span());
    REQUIRE_FALSE(unidentified);
    REQUIRE(unidentified.error().code == ProgramWireErrorCode::InvalidProducerEpoch);
    // Resealed rather than raw-corrupted: the epoch lives in the checksummed
    // payload, so without re-sealing this would prove ChecksumMismatch fires
    // and never reach the epoch check at all.
    const auto bytes = fixture.bytes();
    const std::size_t epoch_at = kPayloadAt + offsetof(ProgramWireProgramRecord, producer_epoch);
    const std::uint64_t zero = 0;
    std::memcpy(bytes.data() + epoch_at, &zero, sizeof(zero));
    reseal(bytes);
    auto zeroed = decode_program_wire(bytes);
    REQUIRE_FALSE(zeroed);
    REQUIRE(zeroed.error().code == ProgramWireErrorCode::InvalidProducerEpoch);
    std::memcpy(bytes.data() + epoch_at, &kEpoch, sizeof(kEpoch));
    reseal(bytes);
    REQUIRE(decode_program_wire(bytes));

    // The default source of identity is usable without a caller inventing one:
    // never zero, and stable within a process so repeated publishes from one
    // producer are not mistaken for different producers.
    REQUIRE(program_wire_process_epoch() != 0);
    REQUIRE(program_wire_process_epoch() == program_wire_process_epoch());
}

TEST_CASE("program wire equality compares values rather than shared ownership",
          "[playback][wire]") {
    EncodedFixture fixture;
    auto decoded = decode_program_wire(fixture.bytes());
    REQUIRE(decoded);
    REQUIRE(program_wire_matches(decoded.value(), *fixture.program, kTempoPoints, kEpoch));

    // Recompiling the same document yields a program that shares no track,
    // automation, or tempo ownership with the first. The equality walk must not
    // notice any of that — it compares values, not the pointers the wire exists
    // to leave behind — and it must still notice the one thing that genuinely
    // differs, which is each lane's per-compile instance token.
    CompiledProgram rebuilt{wire_project()};
    const auto other = rebuilt.store.read();
    REQUIRE(other.get() != fixture.program.get());
    REQUIRE(other->find_track({10}) != fixture.program->find_track({10}));
    REQUIRE(&other->tempo_map() != &fixture.program->tempo_map());
    REQUIRE_FALSE(program_wire_matches(decoded.value(), *other, kTempoPoints, kEpoch));

    // Which of those two it is matters, so it is proven rather than assumed:
    // restamp only the lane tokens with the rebuilt program's, and every other
    // compared field must already agree. A match here says the walk is blind to
    // ownership; the REQUIRE_FALSE above says it is not blind to identity. The
    // original of this case asserted the match without the restamp, which could
    // not tell those two apart.
    const auto* rebuilt_automation = other->find_track({10})->automation_program();
    REQUIRE(rebuilt_automation != nullptr);
    const auto rebuilt_lanes = rebuilt_automation->programs();
    REQUIRE(rebuilt_lanes.size() == 3);
    WireBuffer restamped(fixture.size);
    std::memcpy(restamped.span().data(), fixture.bytes().data(), fixture.size);
    for (std::size_t i = 0; i < rebuilt_lanes.size(); ++i) {
        const auto token = rebuilt_lanes[i]->instance_token().value;
        REQUIRE(token != 0);
        std::memcpy(restamped.span().data() + lane_token_at(restamped.span(), i), &token,
                    sizeof(token));
    }
    reseal(restamped.span());
    auto reidentified = decode_program_wire(restamped.span());
    REQUIRE(reidentified);
    REQUIRE(program_wire_matches(reidentified.value(), *other, kTempoPoints, kEpoch));
    // ...and the restamped payload is a different byte range from the one it was
    // copied from, so the match above is not the trivial one.
    REQUIRE(std::memcmp(restamped.span().data(), fixture.bytes().data(), fixture.size) != 0);

    // And a real difference is seen: the equality walk is not vacuously true.
    WireBuffer swapped(fixture.size);
    std::memcpy(swapped.span().data(), fixture.bytes().data(), fixture.size);
    const std::size_t track_section =
        kPayloadAt + sizeof(ProgramWireProgramRecord) + 2 * sizeof(ProgramWireTempoPointRecord);
    const std::uint64_t renamed = 999;
    std::memcpy(swapped.span().data() + track_section + offsetof(ProgramWireTrackRecord, id),
                &renamed, sizeof(renamed));
    reseal(swapped.span());
    auto altered = decode_program_wire(swapped.span());
    REQUIRE(altered);
    REQUIRE_FALSE(program_wire_matches(altered.value(), *fixture.program, kTempoPoints, kEpoch));

    const std::array shifted{TempoPoint{{0}, 120.0, TempoCurve::LinearInTicks},
                             TempoPoint{{kTicksPerQuarter * 4}, 96.0}};
    REQUIRE_FALSE(program_wire_matches(decoded.value(), *fixture.program, shifted, kEpoch));
}

// `producer_epoch` distinguishes PRODUCERS; `instance_token` distinguishes
// PROGRAMS FROM ONE PRODUCER. The wire needs both, because the two answer
// different questions and only the first was carried. The producer-restart case
// is covered above; this is the single producer recompiling, which is the more
// common one — `generation` is caller-supplied rather than minted per compile,
// so nothing else in the payload moves.
//
// The first half is a control with a known answer: two compiles of the same lane
// at the same generation must NOT read as Unchanged in process. If that ever
// stops holding, the premise is gone and this case says so rather than passing
// quietly on a wire comparison that no longer means anything.
TEST_CASE("one producer's successive programs are distinguishable on the wire",
          "[playback][wire]") {
    // The two halves below judge ONE pair of programs: the cursor half and the
    // byte half must concern the same two, or the case proves two unconnected
    // things — that some pair is distinguishable in process, and that some other
    // pair is byte-identical — while claiming to have bridged them. The pair is
    // therefore taken out of the two PlaybackPrograms that actually get encoded,
    // rather than compiled separately alongside them.
    //
    // Both compiles share one tempo map, because AutomationCursor refuses a
    // program whose map is not the transport's. Without sharing, one cursor
    // cannot judge both, which is what pushed the two halves onto different
    // pairs to begin with.
    const auto map = wire_tempo_map();
    EncodedFixture fixture{map};
    CompiledProgram rebuilt{wire_project(), nullptr, map};
    const auto second = rebuilt.store.read();
    REQUIRE(second->generation() == fixture.program->generation());

    const auto* first_automation = fixture.program->find_track({10})->automation_program();
    const auto* second_automation = second->find_track({10})->automation_program();
    REQUIRE(first_automation != nullptr);
    REQUIRE(second_automation != nullptr);
    const auto& first_program = *first_automation->programs()[0];
    const auto& second_program = *second_automation->programs()[0];
    REQUIRE(&first_program.tempo_map() == &second_program.tempo_map());

    // Same lane, same generation — and a fresh token per compile, because
    // next_instance_token() is a process-global monotonic counter.
    REQUIRE(first_program.lane_id() == second_program.lane_id());
    REQUIRE(first_program.generation() == second_program.generation());
    REQUIRE(first_program.instance_token() != second_program.instance_token());

    MasterTransport clock;
    MasterTransportConfig config;
    config.max_buffer_size = 64;
    config.initially_playing = true;
    REQUIRE(clock.prepare(*map, config) == TransportError::None);

    std::array<AutomationBlockEvent, 32> events{};
    AutomationCursor cursor;
    TransportSnapshot snapshot;
    REQUIRE(clock.begin_block(32, snapshot) == TransportError::None);
    REQUIRE(cursor.process(first_program, snapshot, events).adoption ==
            AutomationProgramAdoption::Adopted);
    REQUIRE(clock.begin_block(32, snapshot) == TransportError::None);
    // The control, and it is over the very lane the payloads below carry: in
    // process the cursor tells these two apart, because Unchanged requires the
    // instance token to match as well as the lane key.
    REQUIRE(cursor.process(second_program, snapshot, events).adoption !=
            AutomationProgramAdoption::Unchanged);

    // Across a realm the consumer must reach that same answer about that same
    // pair. Both encode at one epoch, and their lane ids, generations and epochs
    // all match — so before the token was carried the bytes matched too, and a
    // consumer computing Unchanged from them contradicted the cursor above. That
    // is a silently wrong render rather than a decode error.

    WireBuffer again(fixture.size);
    REQUIRE(take(encode_program_wire(*second, kTempoPoints, kEpoch, again.span())) == fixture.size);
    REQUIRE(fixture.size > 0);
    REQUIRE(std::memcmp(fixture.bytes().data(), again.span().data(), fixture.size) != 0);

    // The mirror of that assertion, and it is what stops the `!=` above from
    // being vacuous: an inequality passes trivially against a comparison that
    // can only ever report "different", exactly as the old equality assertion
    // passed against one that could only report "same". Encoding ONE program
    // twice — same tokens, same everything — must still compare EQUAL. Without
    // this, "the payloads differ" is equally consistent with a nonce leaking
    // into every field and no two encodings ever matching again.
    WireBuffer repeated(fixture.size);
    REQUIRE(take(encode_program_wire(*second, kTempoPoints, kEpoch, repeated.span())) ==
            fixture.size);
    REQUIRE(std::memcmp(again.span().data(), repeated.span().data(), fixture.size) == 0);

    // Negative control for the difference itself: it must be the tokens and
    // nothing else. Normalising them collapses the two payloads to the same
    // bytes, which says the rest of the encoding is as identical as it always
    // was — the fix added an identity rather than perturbing the content.
    WireBuffer flattened_first(fixture.size);
    WireBuffer flattened_second(fixture.size);
    std::memcpy(flattened_first.span().data(), fixture.bytes().data(), fixture.size);
    std::memcpy(flattened_second.span().data(), again.span().data(), fixture.size);
    normalize_instance_tokens(flattened_first.span());
    normalize_instance_tokens(flattened_second.span());
    REQUIRE(std::memcmp(flattened_first.span().data(), flattened_second.span().data(),
                        fixture.size) == 0);

    // And the difference is per lane, not one blanket stamp: each lane carries
    // its own token, so a consumer can re-adopt the lanes that moved and keep
    // its cursor state for the rest.
    auto first_view = decode_program_wire(fixture.bytes());
    auto second_view = decode_program_wire(again.span());
    REQUIRE(first_view);
    REQUIRE(second_view);
    const auto first_lanes = first_view.value().automation_lanes();
    const auto second_lanes = second_view.value().automation_lanes();
    REQUIRE(first_lanes.size() == 3);
    REQUIRE(second_lanes.size() == first_lanes.size());
    for (std::size_t i = 0; i < first_lanes.size(); ++i) {
        REQUIRE(first_lanes[i].lane_id == second_lanes[i].lane_id);
        REQUIRE(first_lanes[i].generation == second_lanes[i].generation);
        REQUIRE(first_lanes[i].instance_token != 0);
        REQUIRE(second_lanes[i].instance_token != 0);
        REQUIRE(first_lanes[i].instance_token != second_lanes[i].instance_token);
    }

    // Positive control for the comparison itself: the same two programs encoded
    // under DIFFERENT epochs must also differ. Without it, "the bytes differed"
    // is equally consistent with a comparison that cannot report a match.
    WireBuffer separated(fixture.size);
    REQUIRE(take(encode_program_wire(*second, kTempoPoints, kEpoch + 1, separated.span())) ==
            fixture.size);
    REQUIRE(std::memcmp(again.span().data(), separated.span().data(), fixture.size) != 0);
}

// Growing a record is not a compatibility gap, it is a silent-corruption
// hazard, and the two need different guards. A reader that MISSES a field
// renders less than the writer meant; a reader that misreads the stride
// fabricates records that were never written. This case pins the second, and it
// is worth having whether or not the token is the field that grew.
//
// The decoder derives its record count from a compile-time `sizeof` — see
// `records<>()` and the section walk in program_wire.cpp, which compute
// `bytes / sizeof(Record)` and reject only a non-zero remainder. A remainder is
// therefore the ONLY thing that makes a stride mismatch visible, and a count
// whose byte length happens to divide by the old size has no remainder to
// notice. `min_reader_version` is the entire defence, not a courtesy.
TEST_CASE("a version 1 reader is refused rather than left to misread the lane stride",
          "[playback][wire]") {
    CompiledProgram compiled{six_lane_project()};
    const auto program = compiled.store.read();
    const auto size = take(program_wire_encoded_size(*program, kTempoPoints));
    WireBuffer buffer{size};
    REQUIRE(take(encode_program_wire(*program, kTempoPoints, kEpoch, buffer.span())) == size);

    auto decoded = decode_program_wire(buffer.span());
    REQUIRE(decoded);
    REQUIRE(decoded.value().automation_lanes().size() == 6);

    const auto [lane_offset, lane_bytes] =
        section_span(buffer.span(), ProgramWireSection::AutomationLanes);
    REQUIRE(lane_bytes == 6 * sizeof(ProgramWireAutomationLaneRecord));

    // The hazard, made concrete rather than asserted. A version 1 reader sized
    // this section by its own 48-byte lane record. That divides 336 exactly, so
    // it would find no remainder to reject and would hand its caller SEVEN
    // records of shifted garbage — six lanes' worth of bytes reinterpreted at
    // the wrong stride, every field past `generation` read from the wrong
    // offset, and no error anywhere.
    constexpr std::size_t kVersion1LaneRecordBytes = 48;
    REQUIRE(lane_bytes % kVersion1LaneRecordBytes == 0);
    REQUIRE(lane_bytes / kVersion1LaneRecordBytes == 7);
    REQUIRE(lane_bytes / kVersion1LaneRecordBytes != decoded.value().automation_lanes().size());

    // And the guard that stops it, which is the only one there is: the payload
    // states a minimum reader version above 1, so a version 1 reader refuses at
    // the header and never reaches the stride arithmetic at all.
    constexpr std::uint16_t kVersion1Reader = 1;
    ProgramWireHeader header;
    std::memcpy(&header, buffer.span().data(), sizeof(header));
    REQUIRE(header.min_reader_version > kVersion1Reader);

    // Proven the same way the decoder proves it, rather than by reading the
    // constant back: a payload demanding a reader newer than this build is
    // refused, so the check the version rides on demonstrably fires.
    const auto beyond = static_cast<std::uint16_t>(kProgramWireVersion + 1);
    std::memcpy(buffer.span().data() + offsetof(ProgramWireHeader, min_reader_version), &beyond,
                sizeof(beyond));
    auto refused = decode_program_wire(buffer.span());
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error().code == ProgramWireErrorCode::UnsupportedVersion);
}

// Zero is what an unwritten lane record holds, so it would compare equal to
// every other unwritten lane and report Unchanged for a program never adopted —
// the same wildcard failure a zero producer epoch would cause, one level down.
TEST_CASE("program wire refuses a lane with no instance token", "[playback][wire]") {
    EncodedFixture fixture;
    const auto bytes = fixture.bytes();
    const auto token_at = lane_token_at(bytes, 1);

    std::uint64_t original = 0;
    std::memcpy(&original, bytes.data() + token_at, sizeof(original));
    REQUIRE(original != 0);

    // Resealed rather than raw-corrupted: the token lives in the checksummed
    // payload, so without re-sealing this would prove ChecksumMismatch fires and
    // never reach the token check at all.
    const std::uint64_t zero = 0;
    std::memcpy(bytes.data() + token_at, &zero, sizeof(zero));
    reseal(bytes);
    auto rejected = decode_program_wire(bytes);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ProgramWireErrorCode::InvalidInstanceToken);
    REQUIRE(rejected.error().section ==
            static_cast<std::uint32_t>(ProgramWireSection::AutomationLanes));
    REQUIRE(rejected.error().detail == 1);

    // The restore is the negative control: without it the rejection above is
    // equally consistent with a payload that never decoded in the first place.
    std::memcpy(bytes.data() + token_at, &original, sizeof(original));
    reseal(bytes);
    REQUIRE(decode_program_wire(bytes));
}

TEST_CASE("wire automation consumer matches every direct production cursor lane",
          "[playback][wire][consumer]") {
    const auto map = wire_tempo_map();
    CompiledProgram direct{wire_project(), nullptr, map};
    const auto program = direct.store.read();
    const auto* automation = program->find_track({10})->automation_program();
    REQUIRE(automation != nullptr);
    REQUIRE(automation->programs().size() == 3);

    MasterTransport clock;
    MasterTransportConfig config;
    config.max_buffer_size = 512;
    config.initially_playing = true;
    REQUIRE(clock.prepare(*map, config) == TransportError::None);
    TransportSnapshot snapshot;
    REQUIRE(clock.begin_block(512, snapshot) == TransportError::None);

    std::array<AutomationCursor, 3> direct_cursors;
    std::array<std::array<AutomationBlockEvent, 1'024>, 3> direct_events;
    std::array<AutomationCursorResult, 3> direct_results;
    for (std::size_t index = 0; index < direct_cursors.size(); ++index)
        direct_results[index] = direct_cursors[index].process(*automation->programs()[index],
                                                              snapshot, direct_events[index]);

    auto bytes = encode_project_bytes(wire_project(), map);
    std::array<ProgramWireLaneState, 3> state;
    ProgramWireAutomationConsumer consumer{state};
    auto adopted = consumer.adopt(ProgramWireBytePin{bytes->span(), 1}, *map, kTempoPoints);
    REQUIRE(adopted.adoption == ProgramWireAdoption::Adopted);
    REQUIRE_FALSE(adopted.returned_pin);

    std::array<std::array<AutomationBlockEvent, 1'024>, 3> wire_events;
    std::array<ProgramWireAutomationLaneOutput, 3> output;
    for (std::size_t index = 0; index < output.size(); ++index)
        output[index].events = wire_events[index];
    const auto rendered = consumer.render(snapshot, output);
    REQUIRE(rendered.code == ProgramWireConsumerCode::Ok);
    REQUIRE(rendered.rendered_lanes == 3);
    for (std::size_t index = 0; index < output.size(); ++index) {
        REQUIRE(output[index].track_id == ItemId{10});
        REQUIRE(output[index].lane_id == automation->programs()[index]->lane_id());
        REQUIRE(output[index].result.code == direct_results[index].code);
        REQUIRE(output[index].result.emitted_events == direct_results[index].emitted_events);
        REQUIRE(std::equal(wire_events[index].begin(),
                           wire_events[index].begin() + output[index].result.emitted_events,
                           direct_events[index].begin()));
    }
}

TEST_CASE("wire automation consumer bounds tracks without automation",
          "[playback][wire][consumer][capacity]") {
    const auto map = wire_tempo_map();
    auto bytes = encode_project_bytes(wire_project(), map);
    REQUIRE(take(decode_program_wire(bytes->span())).tracks().size() == 2);

    std::array<ProgramWireLaneState, 3> state;
    ProgramWireAutomationConsumer consumer{state, 1};
    auto rejected = consumer.adopt(ProgramWireBytePin{bytes->span(), 70}, *map, kTempoPoints);
    REQUIRE(rejected.code == ProgramWireConsumerCode::StateCapacityExceeded);
    REQUIRE(rejected.adoption == ProgramWireAdoption::Rejected);
    REQUIRE(rejected.wire_error.code == ProgramWireErrorCode::InvalidLimits);
    REQUIRE(rejected.wire_error.section ==
            static_cast<std::uint32_t>(ProgramWireSection::Tracks));
    REQUIRE(rejected.wire_error.detail == 2);
    REQUIRE(rejected.returned_pin.owner_token() == 70);
    REQUIRE_FALSE(consumer.active_bytes().data());
}

TEST_CASE("wire automation uses the production per-device event ceiling",
          "[playback][wire][consumer][capacity]") {
    const auto map = wire_tempo_map();
    CompiledProgram direct{wire_project(), nullptr, map};
    const auto direct_program = direct.store.read();
    const auto* direct_track = direct_program->find_track({10});
    REQUIRE(direct_track != nullptr);
    auto limits = direct_program->automation_limits();
    limits.max_events_per_device_per_block = 2;
    auto native =
        take(TrackAutomationRenderer::create(direct_track->automation_program_owner(), limits));

    auto bytes = encode_project_bytes(wire_project(), map);
    const auto max_events_at =
        kPayloadAt + offsetof(ProgramWireProgramRecord, max_events_per_device_per_block);
    std::memcpy(bytes->span().data() + max_events_at, &limits.max_events_per_device_per_block,
                sizeof(limits.max_events_per_device_per_block));
    reseal(bytes->span());
    REQUIRE(decode_program_wire(bytes->span()));

    MasterTransport clock;
    MasterTransportConfig config;
    config.max_buffer_size = 64;
    config.initially_playing = true;
    REQUIRE(clock.prepare(*map, config) == TransportError::None);
    TransportSnapshot snapshot;
    REQUIRE(clock.begin_block(64, snapshot) == TransportError::None);
    const auto native_result = native.process(snapshot);
    REQUIRE(native_result.code == TrackAutomationRendererCode::Coalesced);

    std::array<ProgramWireLaneState, 3> state;
    ProgramWireAutomationConsumer consumer{state};
    REQUIRE(consumer.adopt(ProgramWireBytePin{bytes->span(), 71}, *map, kTempoPoints).adoption ==
            ProgramWireAdoption::Adopted);
    std::array<std::array<AutomationBlockEvent, 64>, 3> event_storage;
    std::array<ProgramWireAutomationLaneOutput, 3> output;
    for (std::size_t index = 0; index < output.size(); ++index)
        output[index].events = event_storage[index];
    REQUIRE(consumer.render(snapshot, output).code == ProgramWireConsumerCode::Ok);
    REQUIRE(output[0].result.code == AutomationCursorCode::Coalesced);
    REQUIRE(output[1].result.code == AutomationCursorCode::Coalesced);
    REQUIRE(output[0].result.emitted_events == 2);
    REQUIRE(output[1].result.emitted_events == 2);

    for (const auto& batch : native.batches()) {
        REQUIRE(batch.coalesced);
        REQUIRE(batch.events.size() == 2);
        for (std::size_t event = 0; event < batch.events.size(); ++event) {
            const auto found = std::find_if(output.begin(), output.end(), [&](const auto& lane) {
                return lane.lane_id == batch.events[event].lane_id;
            });
            REQUIRE(found != output.end());
            const auto lane_index = static_cast<std::size_t>(found - output.begin());
            const auto expected_block_offset =
                batch.events[event].sample_offset + batch.events[event].ramp_duration_sample_frames;
            REQUIRE(event_storage[lane_index][event].sample_offset == expected_block_offset);
            REQUIRE(event_storage[lane_index][event].value == batch.events[event].value);
            REQUIRE(event_storage[lane_index][event].transition ==
                    (batch.events[event].ramp_duration_sample_frames == 0
                         ? AutomationTransition::Seed
                         : AutomationTransition::LinearRamp));
        }
    }
}

TEST_CASE("wire automation publication identity re-adopts by instance token",
          "[playback][wire][consumer][negative-control]") {
    const auto map = wire_tempo_map();
    auto first = encode_project_bytes(wire_project(0.25f), map);
    auto replacement = encode_project_bytes(wire_project(-0.5f), map);
    auto first_view = take(decode_program_wire(first->span()));
    auto replacement_view = take(decode_program_wire(replacement->span()));
    REQUIRE(first_view.program().generation == replacement_view.program().generation);
    REQUIRE(first_view.automation_lanes().size() == replacement_view.automation_lanes().size());

    std::array<std::uint64_t, 3> replacement_tokens{};
    for (std::size_t index = 0; index < replacement_tokens.size(); ++index) {
        replacement_tokens[index] = replacement_view.automation_lanes()[index].instance_token;
        const auto ignored_token = first_view.automation_lanes()[index].instance_token;
        std::memcpy(replacement->span().data() + lane_token_at(replacement->span(), index),
                    &ignored_token, sizeof(ignored_token));
    }
    reseal(replacement->span());

    MasterTransport clock;
    MasterTransportConfig config;
    config.max_buffer_size = 64;
    config.initially_playing = true;
    REQUIRE(clock.prepare(*map, config) == TransportError::None);
    TransportSnapshot snapshot;
    REQUIRE(clock.begin_block(64, snapshot) == TransportError::None);

    std::array<ProgramWireLaneState, 3> state;
    ProgramWireAutomationConsumer consumer{state};
    REQUIRE(consumer.adopt(ProgramWireBytePin{first->span(), 11}, *map, kTempoPoints).adoption ==
            ProgramWireAdoption::Adopted);
    std::array<std::array<AutomationBlockEvent, 64>, 3> event_storage;
    std::array<ProgramWireAutomationLaneOutput, 3> output;
    for (std::size_t index = 0; index < output.size(); ++index)
        output[index].events = event_storage[index];
    REQUIRE(consumer.render(snapshot, output).code == ProgramWireConsumerCode::Ok);
    const auto first_seed = event_storage[0][0].value;

    // Negative control: flattening the replacement's tokens makes its full
    // publication identity look unchanged. The candidate is returned and the
    // active bytes therefore stale-render the first curve.
    auto stale = consumer.adopt(ProgramWireBytePin{replacement->span(), 22}, *map, kTempoPoints);
    REQUIRE(stale.adoption == ProgramWireAdoption::Unchanged);
    REQUIRE(stale.returned_pin.owner_token() == 22);
    REQUIRE(consumer.render(snapshot, output).code == ProgramWireConsumerCode::Ok);
    REQUIRE(event_storage[0][0].value == first_seed);
    REQUIRE(event_storage[0][0].value != -0.5f);

    // Restore the bytes that carry identity. The same candidate now re-adopts,
    // returns the retired first pin, and renders the changed curve. Poisoning
    // that retired buffer proves the active view no longer borrows from it.
    for (std::size_t index = 0; index < replacement_tokens.size(); ++index)
        std::memcpy(replacement->span().data() + lane_token_at(replacement->span(), index),
                    &replacement_tokens[index], sizeof(replacement_tokens[index]));
    reseal(replacement->span());
    auto restored = consumer.adopt(std::move(stale.returned_pin), *map, kTempoPoints);
    REQUIRE(restored.adoption == ProgramWireAdoption::Adopted);
    REQUIRE(restored.returned_pin.owner_token() == 11);
    std::fill(first->span().begin(), first->span().end(), std::byte{0xA5});
    REQUIRE(consumer.render(snapshot, output).code == ProgramWireConsumerCode::Ok);
    REQUIRE(event_storage[0][0].value == -0.5f);
}

TEST_CASE("wire automation preserves stable lane cursors across outer generations",
          "[playback][wire][consumer]") {
    const auto map = wire_tempo_map();
    auto first = encode_project_bytes(wire_project(), map);
    auto first_view = take(decode_program_wire(first->span()));
    WireBuffer next(first->span().size());
    std::memcpy(next.span().data(), first->span().data(), first->span().size());
    const auto next_program_generation = first_view.program().generation + 1u;
    std::memcpy(next.span().data() + kPayloadAt + offsetof(ProgramWireProgramRecord, generation),
                &next_program_generation, sizeof(next_program_generation));
    const auto [track_section, track_bytes] = section_span(next.span(), ProgramWireSection::Tracks);
    REQUIRE(track_bytes >= sizeof(ProgramWireTrackRecord));
    const auto next_track_generation = first_view.tracks().front().generation + 1u;
    std::memcpy(next.span().data() + track_section + offsetof(ProgramWireTrackRecord, generation),
                &next_track_generation, sizeof(next_track_generation));
    reseal(next.span());
    REQUIRE(decode_program_wire(next.span()));

    std::array<ProgramWireLaneState, 3> state;
    ProgramWireAutomationConsumer consumer{state};
    REQUIRE(consumer.adopt(ProgramWireBytePin{first->span(), 61}, *map, kTempoPoints).adoption ==
            ProgramWireAdoption::Adopted);
    MasterTransport clock;
    MasterTransportConfig config;
    config.max_buffer_size = 64;
    config.initially_playing = false;
    REQUIRE(clock.prepare(*map, config) == TransportError::None);
    std::array<std::array<AutomationBlockEvent, 64>, 3> event_storage;
    std::array<ProgramWireAutomationLaneOutput, 3> output;
    for (std::size_t index = 0; index < output.size(); ++index)
        output[index].events = event_storage[index];
    TransportSnapshot snapshot;
    REQUIRE(clock.begin_block(64, snapshot) == TransportError::None);
    REQUIRE(consumer.render(snapshot, output).code == ProgramWireConsumerCode::Ok);
    REQUIRE(output[0].result.emitted_events == 1);

    auto adopted = consumer.adopt(ProgramWireBytePin{next.span(), 62}, *map, kTempoPoints);
    REQUIRE(adopted.adoption == ProgramWireAdoption::Adopted);
    REQUIRE(adopted.returned_pin.owner_token() == 61);
    REQUIRE(clock.begin_block(64, snapshot) == TransportError::None);
    REQUIRE(consumer.render(snapshot, output).code == ProgramWireConsumerCode::Ok);
    REQUIRE(output[0].result.emitted_events == 0);
}

TEST_CASE("wire automation atomically rejects a regressed lane generation",
          "[playback][wire][consumer]") {
    const auto map = wire_tempo_map();
    auto active = encode_project_bytes(wire_project(), map);
    const auto [lane_section, lane_bytes] =
        section_span(active->span(), ProgramWireSection::AutomationLanes);
    REQUIRE(lane_bytes >= sizeof(ProgramWireAutomationLaneRecord));
    const ProgramGeneration active_lane_generation = 2;
    std::memcpy(active->span().data() + lane_section +
                    offsetof(ProgramWireAutomationLaneRecord, generation),
                &active_lane_generation, sizeof(active_lane_generation));
    reseal(active->span());
    REQUIRE(decode_program_wire(active->span()));

    WireBuffer stale(active->span().size());
    std::memcpy(stale.span().data(), active->span().data(), active->span().size());
    auto active_view = take(decode_program_wire(active->span()));
    const auto newer_outer_generation = active_view.program().generation + 1u;
    std::memcpy(stale.span().data() + kPayloadAt + offsetof(ProgramWireProgramRecord, generation),
                &newer_outer_generation, sizeof(newer_outer_generation));
    const ProgramGeneration stale_lane_generation = 1;
    std::memcpy(stale.span().data() + lane_section +
                    offsetof(ProgramWireAutomationLaneRecord, generation),
                &stale_lane_generation, sizeof(stale_lane_generation));
    reseal(stale.span());
    REQUIRE(decode_program_wire(stale.span()));

    std::array<ProgramWireLaneState, 3> state;
    ProgramWireAutomationConsumer consumer{state};
    REQUIRE(consumer.adopt(ProgramWireBytePin{active->span(), 81}, *map, kTempoPoints).adoption ==
            ProgramWireAdoption::Adopted);
    auto rejected = consumer.adopt(ProgramWireBytePin{stale.span(), 82}, *map, kTempoPoints);
    REQUIRE(rejected.code == ProgramWireConsumerCode::StalePublication);
    REQUIRE(rejected.adoption == ProgramWireAdoption::Rejected);
    REQUIRE(rejected.wire_error.code == ProgramWireErrorCode::StaleLaneGeneration);
    REQUIRE(rejected.returned_pin.owner_token() == 82);
    REQUIRE(consumer.active_bytes().data() == active->span().data());

    std::fill(stale.span().begin(), stale.span().end(), std::byte{0x3C});
    MasterTransport clock;
    MasterTransportConfig config;
    config.max_buffer_size = 64;
    config.initially_playing = true;
    REQUIRE(clock.prepare(*map, config) == TransportError::None);
    TransportSnapshot snapshot;
    REQUIRE(clock.begin_block(64, snapshot) == TransportError::None);
    std::array<std::array<AutomationBlockEvent, 64>, 3> event_storage;
    std::array<ProgramWireAutomationLaneOutput, 3> output;
    for (std::size_t index = 0; index < output.size(); ++index)
        output[index].events = event_storage[index];
    REQUIRE(consumer.render(snapshot, output).code == ProgramWireConsumerCode::Ok);
    REQUIRE(output[0].result.emitted_events != 0);
}

TEST_CASE("wire decode adoption and production cursor render are realtime safe",
          "[playback][wire][consumer][rt-safety]") {
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<ProgramWireBytePin>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<ProgramWireBytePin>);
    STATIC_REQUIRE(std::is_move_constructible_v<ProgramWireBytePin>);
    const auto map = wire_tempo_map();
    // encode_project_bytes returns only bytes; its PlaybackProgram and every
    // source automation object have already been destroyed here.
    auto bytes = encode_project_bytes(wire_project(), map);
    std::array<ProgramWireLaneState, 3> state;
    ProgramWireAutomationConsumer consumer{state};
    MasterTransport clock;
    MasterTransportConfig config;
    config.max_buffer_size = 64;
    config.initially_playing = true;
    REQUIRE(clock.prepare(*map, config) == TransportError::None);
    TransportSnapshot snapshot;
    REQUIRE(clock.begin_block(64, snapshot) == TransportError::None);
    std::array<std::array<AutomationBlockEvent, 64>, 3> event_storage;
    std::array<ProgramWireAutomationLaneOutput, 3> output;
    for (std::size_t index = 0; index < output.size(); ++index)
        output[index].events = event_storage[index];

    ProgramWireAdoptionResult adopted;
    ProgramWireAutomationRenderResult rendered;
    std::size_t allocations = 0;
    {
        pulp::test::ScopedRtProcessProbe probe;
        adopted = consumer.adopt(ProgramWireBytePin{bytes->span(), 33}, *map, kTempoPoints);
        rendered = consumer.render(snapshot, output);
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
    REQUIRE(adopted.adoption == ProgramWireAdoption::Adopted);
    REQUIRE(rendered.code == ProgramWireConsumerCode::Ok);
    REQUIRE(rendered.rendered_lanes == 3);

    auto rejected_bytes = encode_project_bytes(wire_project(-0.75f), map);
    auto replacement_bytes = encode_project_bytes(wire_project(-0.5f), map);
    ProgramWireAdoptionResult rejected;
    ProgramWireAdoptionResult replacement;
    ProgramWireAutomationRenderResult after_rejection;
    ProgramWireAutomationRenderResult after_replacement;
    std::size_t retry_allocations = 0;
    {
        pulp::test::ScopedRtProcessProbe probe;
        rejected = consumer.adopt(
            ProgramWireBytePin{rejected_bytes->span().first(rejected_bytes->span().size() - 1u),
                               44},
            *map, kTempoPoints);
        std::fill(rejected_bytes->span().begin(), rejected_bytes->span().end(), std::byte{0x5A});
        after_rejection = consumer.render(snapshot, output);
        replacement =
            consumer.adopt(ProgramWireBytePin{replacement_bytes->span(), 55}, *map, kTempoPoints);
        std::fill(bytes->span().begin(), bytes->span().end(), std::byte{0xA5});
        after_replacement = consumer.render(snapshot, output);
        retry_allocations = probe.allocation_count();
    }
    REQUIRE(retry_allocations == 0);
    REQUIRE(rejected.adoption == ProgramWireAdoption::Rejected);
    REQUIRE(rejected.returned_pin.owner_token() == 44);
    REQUIRE(after_rejection.code == ProgramWireConsumerCode::Ok);
    REQUIRE(replacement.adoption == ProgramWireAdoption::Adopted);
    REQUIRE(replacement.returned_pin.owner_token() == 33);
    REQUIRE(after_replacement.code == ProgramWireConsumerCode::Ok);
    REQUIRE(event_storage[0][0].value == -0.5f);
}
