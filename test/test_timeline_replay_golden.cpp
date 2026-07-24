#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/note_renderer.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/timeline/document_session.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timebase;
using namespace pulp::timeline;

namespace {

template <typename T, typename E> T take(runtime::Result<T, E> result) {
    REQUIRE(result);
    return std::move(result).value();
}

std::shared_ptr<const CompiledTempoMap> tempo_map() {
    const std::array points{TempoPoint{{0}, 120.0}};
    return std::make_shared<const CompiledTempoMap>(
        take(CompiledTempoMap::compile(points, RationalRate{48'000, 1})));
}

TickPosition tick_at_sample(const CompiledTempoMap& map, std::int64_t sample) {
    return map.samples_to_ticks({sample});
}

Project make_checkpoint(const CompiledTempoMap& map) {
    auto audio_clip = take(Clip::create({11}, {0}, TickDuration{tick_at_sample(map, 24).value},
                                        MediaRef{{30}, {0}, 24}));
    auto audio_track = take(Track::create({10}, "audio", {std::move(audio_clip)}));

    const auto note_start = tick_at_sample(map, 5);
    const auto note_end = tick_at_sample(map, 19);
    auto notes =
        take(NoteContent::create({{{22}, note_start, note_end - note_start, 0x8000, 64, 2}}));
    auto note_clip = take(
        Clip::create({21}, {0}, TickDuration{tick_at_sample(map, 32).value}, std::move(notes)));
    auto note_track = take(Track::create({20}, "notes", {std::move(note_clip)}));

    auto sequence = take(Sequence::create({2}, "root", TickDuration{tick_at_sample(map, 32).value},
                                          {std::move(audio_track), std::move(note_track)}));
    const auto content_hash = ContentHash::from_hex(std::string(64, 'a'));
    REQUIRE(content_hash);
    MediaAsset asset{
        {30}, "golden.wav", 24, {48'000, 1}, *content_hash, AssetStoragePolicy::External,
        {},   {},           {}};
    return take(Project::create(ProjectInput{
        {1}, "journal replay golden", 31, {2}, {std::move(asset)}, {std::move(sequence)}}));
}

std::shared_ptr<const DecodedAudioAssetPool> make_audio_pool() {
    auto data = std::make_shared<audio::AudioFileData>();
    data->sample_rate = 48'000;
    data->channels.emplace_back(24, 1.0f);
    return take(DecodedAudioAssetPool::create({{{30}, std::move(data)}}));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        output.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::string hex(std::span<const std::uint8_t> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

struct RenderTrace {
    std::vector<std::uint8_t> bytes;
    std::vector<float> audio_samples;
    std::uint64_t fade_in_frames = 0;
    std::uint64_t fade_out_frames = 0;
};

RenderTrace render_trace(std::shared_ptr<const Project> project,
                         const std::shared_ptr<const CompiledTempoMap>& map,
                         const std::shared_ptr<const DecodedAudioAssetPool>& assets,
                         std::span<const std::uint32_t> block_sizes) {
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = std::move(project);
    request.sequence_id = {2};
    request.tempo_map = map;
    request.document_revision = 1;
    request.dirty.all = true;
    request.audio_assets = assets;
    REQUIRE(compiler.submit(std::move(request)));
    while (compiler.status().busy)
        executor.run_for(std::chrono::seconds(1), 64);
    REQUIRE_FALSE(compiler.status().has_error);
    REQUIRE(store.has_value());

    RenderTrace trace;
    {
        auto compiled = store.read();
        REQUIRE(compiled);
        const auto* track = compiled->find_track({10});
        REQUIRE(track != nullptr);
        REQUIRE(track->audio_program() != nullptr);
        REQUIRE(track->audio_program()->clips().size() == 1);
        trace.fade_in_frames = track->audio_program()->clips()[0].fade_in_frames;
        trace.fade_out_frames = track->audio_program()->clips()[0].fade_out_frames;
    }

    MasterTransport transport;
    REQUIRE(transport.prepare(*map, {.max_buffer_size = 9, .initially_playing = true}) ==
            TransportError::None);
    ArrangementNoteRenderer notes({20});
    REQUIRE(notes.prepare(8));
    PlaybackProgramBlockLatch latch;

    std::uint32_t absolute_sample = 0;
    for (const auto frames : block_sizes) {
        TransportSnapshot snapshot;
        REQUIRE(transport.begin_block(frames, snapshot) == TransportError::None);
        auto block = latch.begin_block(store);
        REQUIRE(block);

        std::vector<float> samples(frames, -99.0f);
        float* channel = samples.data();
        audio::BufferView<float> output(&channel, 1, frames);
        REQUIRE(ArrangementAudioRenderer::process(*block.program(), snapshot, output) ==
                AudioRenderStatus::Rendered);
        const auto note_result = notes.process(block, snapshot);
        REQUIRE(note_result.code == NoteRenderCode::Ok);

        append_u32(trace.bytes, frames);
        for (const auto sample : samples) {
            trace.audio_samples.push_back(sample);
            append_u32(trace.bytes, std::bit_cast<std::uint32_t>(sample));
        }
        append_u32(trace.bytes, static_cast<std::uint32_t>(notes.events().size()));
        for (const auto& event : notes.events()) {
            append_u32(trace.bytes,
                       absolute_sample + static_cast<std::uint32_t>(event.sample_offset));
            REQUIRE(event.size() == 3);
            trace.bytes.insert(trace.bytes.end(), event.data(), event.data() + event.size());
        }
        absolute_sample += frames;
    }
    return trace;
}

std::string read_fixture(std::string_view name) {
    std::ifstream stream(std::string(PULP_TIMELINE_FIXTURE_DIR) + "/v1/" + std::string(name),
                         std::ios::binary);
    REQUIRE(stream.good());
    std::string value((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        value.pop_back();
    return value;
}

} // namespace

TEST_CASE("journal replay reproduces byte-identical audio and MIDI golden streams") {
    const auto map = tempo_map();
    const auto checkpoint = make_checkpoint(*map);
    auto session = take(DocumentSession::create(checkpoint));
    auto writer = take(session->register_writer());

    const auto fade_in_ticks = tick_at_sample(*map, 4).value;
    const auto fade_out_ticks = tick_at_sample(*map, 6).value;
    REQUIRE(map->ticks_to_samples({fade_in_ticks}) == SamplePosition{4});
    REQUIRE(map->ticks_to_samples({fade_out_ticks}) == SamplePosition{6});
    const ClipPlaybackProperties audible{0.25f, static_cast<std::uint64_t>(fade_in_ticks),
                                         static_cast<std::uint64_t>(fade_out_ticks)};
    Transaction transaction;
    transaction.id = writer.allocate_transaction_id();
    transaction.expected_revision = {};
    transaction.commands = {
        {writer.allocate_command_id(), SetClipPlaybackProperties{{2}, {10}, {11}, {}, audible}},
        {writer.allocate_command_id(), SetNoteVelocity{{2}, {20}, {21}, {22}, 0x8000, 0x6000}},
    };
    REQUIRE(session->submit(writer, std::move(transaction)));

    auto replayed = session->journal().replay(checkpoint, {});
    REQUIRE(replayed);
    const auto* replayed_audio = replayed->find_sequence({2})->find_track({10})->find_clip({11});
    REQUIRE(replayed_audio->playback_properties() == audible);
    const auto* replayed_notes = replayed->find_sequence({2})->find_track({20})->find_clip({21});
    REQUIRE(std::get<NoteContent>(replayed_notes->content()).notes()[0].velocity == 0x6000);

    constexpr std::array schedule{3u, 7u, 5u, 9u, 8u};
    const auto assets = make_audio_pool();
    const auto committed_stream = render_trace(session->snapshot(), map, assets, schedule);
    const auto replayed_stream = render_trace(
        std::make_shared<const Project>(std::move(replayed).value()), map, assets, schedule);
    REQUIRE(replayed_stream.bytes == committed_stream.bytes);
    REQUIRE(committed_stream.fade_in_frames == 4);
    REQUIRE(committed_stream.fade_out_frames == 6);
    REQUIRE(committed_stream.audio_samples.size() == 32);
    REQUIRE(committed_stream.audio_samples[0] == Catch::Approx(0.0f));
    REQUIRE(committed_stream.audio_samples[1] == Catch::Approx(0.0625f));
    REQUIRE(committed_stream.audio_samples[4] == Catch::Approx(0.25f));
    REQUIRE(committed_stream.audio_samples[18] == Catch::Approx(0.25f * 5.0f / 6.0f));
    REQUIRE(committed_stream.audio_samples[22] == Catch::Approx(0.25f / 6.0f));
    REQUIRE(committed_stream.audio_samples[23] == Catch::Approx(0.0f));
    REQUIRE(committed_stream.audio_samples[24] == Catch::Approx(0.0f));
    const auto checkpoint_stream =
        render_trace(std::make_shared<const Project>(checkpoint), map, assets, schedule);
    REQUIRE(checkpoint_stream.bytes != committed_stream.bytes);

    Transaction remove_fades;
    remove_fades.id = writer.allocate_transaction_id();
    remove_fades.expected_revision = session->revision();
    remove_fades.commands = {{
        writer.allocate_command_id(),
        SetClipPlaybackProperties{{2}, {10}, {11}, audible, {0.25f, 0, 0}},
    }};
    REQUIRE(session->submit(writer, std::move(remove_fades)));
    const auto no_fades_stream = render_trace(session->snapshot(), map, assets, schedule);
    REQUIRE(no_fades_stream.fade_in_frames == 0);
    REQUIRE(no_fades_stream.fade_out_frames == 0);
    REQUIRE(no_fades_stream.audio_samples[0] == Catch::Approx(0.25f));
    REQUIRE(no_fades_stream.audio_samples[23] == Catch::Approx(0.25f));
    REQUIRE(no_fades_stream.bytes != committed_stream.bytes);

    const auto actual = hex(committed_stream.bytes);
    INFO("actual replay-render golden: " << actual);
    REQUIRE(actual == read_fixture("replay-render.golden"));
}

TEST_CASE("audio and note render trace rejects blocks above the prepared maximum") {
    const auto map = tempo_map();
    MasterTransport transport;
    REQUIRE(transport.prepare(*map, {.max_buffer_size = 9}) == TransportError::None);
    TransportSnapshot untouched;
    REQUIRE(transport.begin_block(10, untouched) == TransportError::InvalidFrameCount);
}
