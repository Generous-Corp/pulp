#include "timebase_test_helpers.hpp"
#include "timeline_command_test_helpers.hpp"

#include <pulp/playback/midi_capture_materializer.hpp>
#include <pulp/playback/recording_commit.hpp>
#include <pulp/runtime/crypto.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <string>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timebase;
using namespace pulp::timeline;

namespace {

Project recording_project() {
    auto track = Track::create(TrackInput{.id = {10}, .name = "record"});
    REQUIRE(track);
    auto sequence = Sequence::create({3}, "sequence", TickDuration{8 * kTicksPerQuarter},
                                     {std::move(track).value()});
    REQUIRE(sequence);
    auto project = Project::create({{1}, "recording", 100, {3}, {}, {std::move(sequence).value()}});
    REQUIRE(project);
    return std::move(project).value();
}

RecordingTakeCommitRequest request() {
    return {
        .sequence_id = {3},
        .track_id = {10},
        .take_lane_id = {101},
        .asset_id = {100},
        .take_id = {102},
        .placement_start = {64},
        .sample_rate = {48'000, 1},
        .asset_name = "capture.wav",
        .create_take_lane = true,
        .take_lane_name = "recording",
    };
}

audio::BufferView<const float> read_view(const audio::Buffer<float>& buffer) {
    return buffer.view();
}

CompiledTempoMap midi_map() {
    const std::array points{TempoPoint{{0}, 120.0}};
    return require_compiled_tempo_map(points, RationalRate{100, 1});
}

CapturedMidiEvent at(midi::MidiEvent event, std::uint64_t frame) {
    return {event, frame};
}

} // namespace

TEST_CASE("sealed recording commands replay a content-hashed take without recapture") {
    audio::Buffer<float> audio(2, 4);
    audio.channel(0)[0] = -1.0f;
    audio.channel(0)[1] = -0.5f;
    audio.channel(0)[2] = 0.0f;
    audio.channel(0)[3] = 0.5f;
    audio.channel(1)[0] = 1.0f;
    audio.channel(1)[1] = 0.25f;
    audio.channel(1)[2] = -0.25f;
    audio.channel(1)[3] = 0.0f;

    auto sealed = seal_recording_take(read_view(audio), request());
    REQUIRE(sealed);
    REQUIRE(sealed->wav_bytes.size() == 44 + 2 * 4 * sizeof(float));
    REQUIRE(std::string(sealed->wav_bytes.begin(), sealed->wav_bytes.begin() + 4) == "RIFF");
    REQUIRE(sealed->asset.content_hash.to_hex() ==
            runtime::sha256_hex(sealed->wav_bytes.data(), sealed->wav_bytes.size()));
    REQUIRE(sealed->commands.size() == 2);
    REQUIRE(std::holds_alternative<CreateAsset>(sealed->commands[0]));
    REQUIRE(std::holds_alternative<InsertTakeLane>(sealed->commands[1]));

    const auto initial = recording_project();
    auto session = std::move(DocumentSession::create(initial)).value();
    auto writer = std::move(session->register_writer()).value();
    REQUIRE(
        session->submit(writer, timeline_test::session_transaction(writer, {}, sealed->commands)));
    const auto snapshot = session->snapshot();
    const auto* asset = snapshot->find_asset({100});
    REQUIRE(asset != nullptr);
    REQUIRE(asset->content_hash == sealed->asset.content_hash);
    const auto* lane = snapshot->find_sequence({3})->find_track({10})->find_take_lane({101});
    REQUIRE(lane != nullptr);
    REQUIRE(lane->takes().size() == 1);
    REQUIRE(lane->takes()[0].id() == ItemId{102});
    REQUIRE(lane->takes()[0].placement_start() == SamplePosition{64});

    auto replayed = session->journal().replay(initial, {});
    REQUIRE(replayed);
    REQUIRE(replayed->find_asset({100})->content_hash == sealed->asset.content_hash);
    REQUIRE(replayed->find_sequence({3})->find_track({10})->find_take_lane({101})->find_take(
                {102}) != nullptr);
}

TEST_CASE("retrospective sealing bit-equals live sealing over the same window") {
    audio::Buffer<float> source(2, 8);
    for (std::size_t frame = 0; frame < source.num_samples(); ++frame) {
        source.channel(0)[frame] =
            std::bit_cast<float>(static_cast<std::uint32_t>(0x3f000000u + frame));
        source.channel(1)[frame] = -source.channel(0)[frame];
    }
    audio::RollingAudioCaptureBuffer rolling;
    REQUIRE(rolling.prepare({2, 16}));
    rolling.append(read_view(source), source.num_samples());
    auto hold = rolling.hold_last(source.num_samples());
    REQUIRE(hold.valid());

    auto live = seal_recording_take(read_view(source), request());
    auto retrospective = seal_retrospective_take(rolling, hold, request());
    REQUIRE(live);
    REQUIRE(retrospective);
    REQUIRE(retrospective->wav_bytes == live->wav_bytes);
    REQUIRE(retrospective->asset.content_hash == live->asset.content_hash);
    REQUIRE(retrospective->commands.size() == live->commands.size());
    REQUIRE(equivalent(retrospective->commands[0], live->commands[0]));
    REQUIRE(equivalent(retrospective->commands[1], live->commands[1]));
}

TEST_CASE("recording sealing canonicalizes equivalent integral sample rates") {
    audio::Buffer<float> audio(1, 4);
    audio.channel(0)[0] = -1.0f;
    audio.channel(0)[1] = -0.5f;
    audio.channel(0)[2] = 0.5f;
    audio.channel(0)[3] = 1.0f;

    auto canonical = seal_recording_take(read_view(audio), request());
    auto equivalent_request = request();
    equivalent_request.sample_rate = {96'000, 2};
    auto equivalent = seal_recording_take(read_view(audio), equivalent_request);

    REQUIRE(canonical);
    REQUIRE(equivalent);
    REQUIRE(equivalent->wav_bytes == canonical->wav_bytes);
    REQUIRE(equivalent->asset.content_hash == canonical->asset.content_hash);
    REQUIRE(equivalent->asset.sample_rate == RationalRate{48'000, 1});
    REQUIRE(equivalent->take.sample_rate() == RationalRate{48'000, 1});
}

TEST_CASE("MIDI capture quantizes notes and trims expression outside active notes") {
    const auto map = midi_map();
    std::array events{
        at(midi::MidiEvent::note_on(2, 64, 100), 12),
        at(midi::MidiEvent::pitch_bend(2, 10'000), 14),
        at(midi::MidiEvent::cc(2, 74, 80), 20),
        at(midi::MidiEvent::note_off(2, 64), 38),
        at(midi::MidiEvent::pitch_bend(2, 8'192), 40),
    };
    MidiCaptureMaterializationConfig config;
    config.tempo_map = &map;
    config.frame_count = 50;
    config.quantize_grid = {kTicksPerQuarter / 2};
    config.minimum_note_duration = {kTicksPerQuarter / 8};
    config.next_item_id = 500;

    auto materialized = materialize_midi_capture(events, config);
    REQUIRE(materialized);
    REQUIRE(materialized->clip_start == map.samples_to_ticks({0}));
    REQUIRE(materialized->notes.notes().size() == 1);
    const auto& note = materialized->notes.notes()[0];
    REQUIRE(note.id == ItemId{500});
    REQUIRE(note.start == TickPosition{0});
    REQUIRE(note.duration == TickDuration{kTicksPerQuarter});
    REQUIRE(note.pitch == 64);
    REQUIRE(note.channel == 2);
    REQUIRE(note.velocity == 51'602);
    REQUIRE(materialized->mpe_expression.size() == 2);
    REQUIRE(materialized->mpe_expression[0].event.is_pitch_bend());
    REQUIRE(materialized->mpe_expression[1].event.is_cc());
    REQUIRE(materialized->next_item_id == 501);
}
