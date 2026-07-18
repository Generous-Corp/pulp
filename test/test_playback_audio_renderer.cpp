#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/program_compiler.hpp>

#include "harness/scoped_rt_process_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <vector>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timeline;
using namespace pulp::timebase;
using Catch::Matchers::WithinAbs;

namespace {

template <typename T, typename E> T take(runtime::Result<T, E> result) {
    REQUIRE(result);
    return std::move(result).value();
}

std::shared_ptr<const audio::AudioFileData>
audio_data(std::initializer_list<std::vector<float>> channels, std::uint32_t sample_rate = 48'000) {
    auto value = std::make_shared<audio::AudioFileData>();
    value->sample_rate = sample_rate;
    value->channels.assign(channels.begin(), channels.end());
    return value;
}

std::shared_ptr<const CompiledTempoMap> map_120() {
    const std::array points{TempoPoint{{0}, 120.0}};
    return std::make_shared<const CompiledTempoMap>(points, RationalRate{48'000, 1});
}

Clip absolute_media_clip(std::uint64_t id, std::int64_t start, std::uint64_t duration,
                         std::uint64_t asset_id, std::uint64_t source_start,
                         std::uint64_t source_count, ClipPlaybackProperties playback = {}) {
    return take(Clip::create_absolute(
        {id}, {start}, duration, {48'000, 1},
        MediaRef{{asset_id}, {static_cast<std::int64_t>(source_start)}, source_count}, playback));
}

Clip musical_media_clip(std::uint64_t id, std::int64_t start, std::int64_t duration,
                        std::uint64_t asset_id, std::uint64_t source_count,
                        ClipPlaybackProperties playback = {}) {
    return take(
        Clip::create({id}, {start}, {duration}, MediaRef{{asset_id}, {0}, source_count}, playback));
}

std::shared_ptr<const Project> project_with_tracks(std::vector<Track> tracks,
                                                   std::vector<MediaAsset> assets) {
    const auto test_hash = ContentHash::from_hex(std::string(64, 'a'));
    REQUIRE(test_hash);
    for (auto& asset : assets)
        if (!asset.content_hash.valid())
            asset.content_hash = *test_hash;
    auto sequence =
        take(Sequence::create({2}, "root", std::nullopt, std::nullopt, std::move(tracks)));
    ProjectInput input;
    input.id = {1};
    input.name = "audio renderer";
    input.next_item_id = 1'000;
    input.root_sequence_id = {2};
    input.assets = std::move(assets);
    input.sequences.push_back(std::move(sequence));
    return std::make_shared<const Project>(take(Project::create(std::move(input))));
}

std::shared_ptr<const DecodedAudioAssetPool> pool(std::vector<DecodedAudioAsset> assets,
                                                  AudioRendererLimits limits = {}) {
    return take(DecodedAudioAssetPool::create(std::move(assets), limits));
}

class InlineExecutor final : public CompileExecutor {
  public:
    bool submit(std::unique_ptr<CompileTask> task, std::chrono::steady_clock::time_point) override {
        while (task->run_slice({std::chrono::steady_clock::now() + std::chrono::seconds(1), 1}) ==
               CompileTaskStatus::Pending) {
        }
        return true;
    }
};

struct CompiledFixture {
    PlaybackProgramStore store;
    InlineExecutor executor;
    std::unique_ptr<PlaybackProgramCompiler> compiler;

    CompiledFixture(std::shared_ptr<const Project> project,
                    std::shared_ptr<const CompiledTempoMap> map,
                    std::shared_ptr<const DecodedAudioAssetPool> assets,
                    AudioRendererLimits limits = {}) {
        compiler = std::make_unique<PlaybackProgramCompiler>(store, executor,
                                                             std::chrono::microseconds(0));
        ProgramCompileRequest request;
        request.project = std::move(project);
        request.sequence_id = {2};
        request.tempo_map = std::move(map);
        request.document_revision = 1;
        request.dirty.all = true;
        request.audio_assets = std::move(assets);
        request.audio_limits = limits;
        REQUIRE(compiler->submit(std::move(request)));
        INFO("compile error code " << static_cast<int>(compiler->status().last_error.code)
                                   << " item " << compiler->status().last_error.item.value
                                   << " audio detail "
                                   << static_cast<int>(compiler->status().last_error.audio_detail));
        REQUIRE(store.has_value());
    }
};

struct Output {
    explicit Output(std::size_t channels, std::size_t frames)
        : storage(channels, std::vector<float>(frames, 99.0f)), pointers(channels) {
        for (std::size_t i = 0; i < channels; ++i)
            pointers[i] = storage[i].data();
    }
    audio::BufferView<float> view() {
        return {pointers.data(), pointers.size(), storage.front().size()};
    }
    std::vector<std::vector<float>> storage;
    std::vector<float*> pointers;
};

TransportSnapshot snapshot(const PlaybackProgram& program, std::uint32_t frames,
                           std::int64_t sample_start = 0) {
    TransportSnapshot value;
    value.tempo_map = &program.tempo_map();
    value.sample_rate = program.tempo_map().sample_rate();
    value.frame_count = frames;
    value.is_playing = true;
    value.range_count = 1;
    value.ranges[0].frame_count = frames;
    value.ranges[0].timeline_sample_start = {sample_start};
    return value;
}

std::vector<std::uint8_t> mono_pcm16_wav(std::span<const std::int16_t> samples) {
    const auto data_bytes = static_cast<std::uint32_t>(samples.size() * 2u);
    std::vector<std::uint8_t> bytes(44u + data_bytes, 0);
    auto put16 = [&](std::size_t offset, std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8u);
    };
    auto put32 = [&](std::size_t offset, std::uint32_t value) {
        put16(offset, static_cast<std::uint16_t>(value));
        put16(offset + 2, static_cast<std::uint16_t>(value >> 16u));
    };
    std::memcpy(bytes.data(), "RIFF", 4);
    put32(4, 36u + data_bytes);
    std::memcpy(bytes.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 1);
    put32(24, 48'000);
    put32(28, 96'000);
    put16(32, 2);
    put16(34, 16);
    std::memcpy(bytes.data() + 36, "data", 4);
    put32(40, data_bytes);
    for (std::size_t index = 0; index < samples.size(); ++index)
        put16(44 + index * 2, static_cast<std::uint16_t>(samples[index]));
    return bytes;
}

} // namespace

static_assert(ArrangementAudioRenderer::process_rt_safety_class ==
              audio::RtSafetyClass::AudioCallbackSafeWithImmutableInputs);
static_assert(!ArrangementAudioRenderer::carries_mutable_state);

TEST_CASE("audio renderer decodes WAV bytes into the immutable asset pool") {
    const std::array<std::int16_t, 4> samples{0, 16'384, -16'384, 32'767};
    const auto bytes = mono_pcm16_wav(samples);
    auto decoded = DecodedAudioAssetPool::decode_wav({3}, bytes);
    REQUIRE(decoded);
    auto assets = DecodedAudioAssetPool::create({std::move(decoded).value()});
    REQUIRE(assets);
    REQUIRE(assets.value()->find({3}));
    REQUIRE(assets.value()->find({3})->audio->num_frames() == 4);
    REQUIRE_THAT(assets.value()->find({3})->audio->channels[0][1], WithinAbs(0.5f, 1e-5f));
}

TEST_CASE("audio renderer honors source range gain fades and varied block boundaries") {
    std::vector<float> source(2'050, 1.0f);
    source[0] = 99.0f;
    source[1] = 99.0f;
    const auto data = audio_data({source});
    auto clip =
        absolute_media_clip(100, 0, 2'048, 3, 2, 2'048,
                            {.gain_linear = 0.5f, .fade_in_duration = 4, .fade_out_duration = 4});
    auto track = take(Track::create({10}, "one", {clip}));
    auto project = project_with_tracks({track}, {{3, "tone", 2'050, {48'000, 1}}});
    const auto map = map_120();
    CompiledFixture compiled(project, map, pool({{3, data}}));
    auto program = compiled.store.read();

    for (const auto frames : {1u, 64u, 257u, 1024u}) {
        Output output(1, frames);
        const auto state = snapshot(*program, frames);
        REQUIRE(ArrangementAudioRenderer::process(*program, state, output.view()) ==
                AudioRenderStatus::Rendered);
        REQUIRE_THAT(output.storage[0][0], WithinAbs(0.0f, 1e-7f));
        if (frames > 4)
            REQUIRE_THAT(output.storage[0][4], WithinAbs(0.5f, 1e-7f));
    }

    Output tail(1, 4);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 4, 2'044),
                                              tail.view()) == AudioRenderStatus::Rendered);
    REQUIRE_THAT(tail.storage[0][0], WithinAbs(0.375f, 1e-7f));
    REQUIRE_THAT(tail.storage[0][3], WithinAbs(0.0f, 1e-7f));
}

TEST_CASE("tempo mapped musical audio placement resolves to exact samples") {
    const auto data = audio_data({std::vector<float>(128, 0.25f)});
    auto clip = musical_media_clip(100, kTicksPerQuarter, kTicksPerQuarter, 3, 128);
    auto track = take(Track::create({10}, "musical", {clip}));
    auto project = project_with_tracks({track}, {{3, "tone", 128, {48'000, 1}}});
    const auto map = map_120();
    CompiledFixture compiled(project, map, pool({{3, data}}));
    auto program = compiled.store.read();

    Output before(1, 64);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 64, 23'936),
                                              before.view()) == AudioRenderStatus::Rendered);
    REQUIRE(before.storage[0][63] == 0.0f);
    Output at_clip(1, 64);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 64, 24'000),
                                              at_clip.view()) == AudioRenderStatus::Rendered);
    REQUIRE_THAT(at_clip.storage[0][0], WithinAbs(0.25f, 1e-7f));
}

TEST_CASE("one bounded compiler pass publishes note and audio payloads for a mixed track") {
    const auto map = map_120();
    auto notes = take(NoteContent::create({{{101}, {0}, {kTicksPerQuarter / 4}, 0x8000, 64, 1}}));
    auto note_clip = take(Clip::create({100}, {0}, {kTicksPerQuarter}, std::move(notes)));
    auto audio_clip =
        musical_media_clip(102, 2 * kTicksPerQuarter, kTicksPerQuarter, 3, 128,
                           {.gain_linear = 0.5f, .fade_in_duration = 2, .fade_out_duration = 3});
    auto track = take(Track::create({10}, "mixed", {note_clip, audio_clip}));
    auto project = project_with_tracks({track}, {{3, "tone", 128, {48'000, 1}}});
    CompiledFixture compiled(project, map,
                             pool({{3, audio_data({std::vector<float>(128, 1.0f)})}}));

    const auto program = compiled.store.read();
    const auto* compiled_track = program->find_track({10});
    REQUIRE(compiled_track != nullptr);
    REQUIRE(compiled_track->ordered_clip_ids().size() == 2);
    REQUIRE(compiled_track->arrangement_note_events().size() == 2);
    REQUIRE(compiled_track->arrangement_note_events()[0].note_id == ItemId{101});
    REQUIRE(compiled_track->audio_program() != nullptr);
    REQUIRE(compiled_track->audio_program()->clips().size() == 1);
    REQUIRE(compiled_track->audio_program()->clips()[0].id == ItemId{102});
    REQUIRE(compiled_track->audio_program()->clips()[0].gain_linear == 0.5f);
}

TEST_CASE("audio renderer projects absolute anchors and source rates on reprepare") {
    const auto data = audio_data({std::vector<float>(441, 0.75f)}, 44'100);
    auto clip =
        take(Clip::create_absolute({100}, {441}, 441, {44'100, 1}, MediaRef{{3}, {0}, 441}));
    auto track = take(Track::create({10}, "absolute rate", {clip}));
    auto project = project_with_tracks({track}, {{3, "44k", 441, {44'100, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, data}}));
    auto program = compiled.store.read();
    REQUIRE(program->find_track({10})->audio_program()->clips()[0].timeline_start == 480);
    REQUIRE(program->find_track({10})->audio_program()->clips()[0].timeline_frame_count == 480);

    Output before(1, 1);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 1, 479),
                                              before.view()) == AudioRenderStatus::Rendered);
    REQUIRE(before.storage[0][0] == 0.0f);
    Output converted(1, 480);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 480, 480),
                                              converted.view()) == AudioRenderStatus::Rendered);
    REQUIRE_THAT(converted.storage[0].front(), WithinAbs(0.75f, 1e-7f));
    REQUIRE_THAT(converted.storage[0].back(), WithinAbs(0.75f, 1e-7f));

    const std::array points{TempoPoint{{0}, 120.0}};
    auto map_96 = std::make_shared<const CompiledTempoMap>(points, RationalRate{96'000, 1});
    CompiledFixture reprepared(project, map_96, pool({{3, data}}));
    auto next = reprepared.store.read();
    REQUIRE(next->find_track({10})->audio_program()->clips()[0].timeline_start == 960);
    REQUIRE(next->find_track({10})->audio_program()->clips()[0].timeline_frame_count == 960);
}

TEST_CASE("audio renderer linearly interpolates a 44k ramp across split blocks") {
    std::vector<float> ramp(441);
    for (std::size_t frame = 0; frame < ramp.size(); ++frame)
        ramp[frame] = static_cast<float>(frame);
    const auto data = audio_data({ramp}, 44'100);
    auto clip = take(Clip::create_absolute({100}, {0}, 441, {44'100, 1}, MediaRef{{3}, {0}, 441}));
    auto track = take(Track::create({10}, "ramp", {clip}));
    auto project = project_with_tracks({track}, {{3, "ramp", 441, {44'100, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, data}}));
    auto program = compiled.store.read();

    Output whole(1, 480);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 480), whole.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE_THAT(whole.storage[0][0], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(whole.storage[0][1], WithinAbs(0.91875f, 1e-6f));
    REQUIRE_THAT(whole.storage[0][2], WithinAbs(1.8375f, 1e-6f));
    REQUIRE_THAT(whole.storage[0][137], WithinAbs(125.86875f, 1e-5f));
    REQUIRE_THAT(whole.storage[0][479], WithinAbs(440.0f, 1e-6f));

    Output first(1, 137);
    Output second(1, 343);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 137), first.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 343, 137),
                                              second.view()) == AudioRenderStatus::Rendered);
    std::vector<float> split = first.storage[0];
    split.insert(split.end(), second.storage[0].begin(), second.storage[0].end());
    REQUIRE(split == whole.storage[0]);
}

TEST_CASE("audio renderer downsamples a 96k impulse at exact source indices") {
    const auto data = audio_data({{0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f}}, 96'000);
    auto clip = take(Clip::create_absolute({100}, {0}, 6, {96'000, 1}, MediaRef{{3}, {0}, 6}));
    auto track = take(Track::create({10}, "impulse", {clip}));
    auto project = project_with_tracks({track}, {{3, "impulse", 6, {96'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, data}}));
    auto program = compiled.store.read();
    Output output(1, 3);

    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 3), output.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(output.storage[0] == std::vector<float>{0.0f, 1.0f, 0.0f});
}

TEST_CASE("absolute sample-rate projection preserves adjacent clip boundaries") {
    const auto data = audio_data({{1.0f, 2.0f}}, 44'100);
    auto first = take(Clip::create_absolute({100}, {0}, 1, {44'100, 1}, MediaRef{{3}, {0}, 1}));
    auto second = take(Clip::create_absolute({101}, {1}, 1, {44'100, 1}, MediaRef{{3}, {1}, 1}));
    auto track = take(Track::create({10}, "adjacent", {first, second}));
    auto project = project_with_tracks({track}, {{3, "44k", 2, {44'100, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, data}}));
    auto program = compiled.store.read();
    const auto clips = program->find_track({10})->audio_program()->clips();
    REQUIRE(clips.size() == 2);
    REQUIRE(clips[0].timeline_end() == clips[1].timeline_start);

    Output output(1, 2);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 2), output.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(output.storage[0] == std::vector<float>{1.0f, 2.0f});
}

TEST_CASE("audio renderer requires exact transport and program tempo map identity") {
    const auto data = audio_data({std::vector<float>(8, 1.0f)});
    auto clip = absolute_media_clip(100, 0, 8, 3, 0, 8);
    auto track = take(Track::create({10}, "identity", {clip}));
    auto project = project_with_tracks({track}, {{3, "tone", 8, {48'000, 1}}});
    const auto baseline = map_120();
    const std::array foreign_points{TempoPoint{{0}, 90.0}, TempoPoint{{kTicksPerQuarter}, 140.0}};
    const auto foreign =
        std::make_shared<const CompiledTempoMap>(foreign_points, RationalRate{48'000, 1});
    CompiledFixture compiled(project, baseline, pool({{3, data}}));
    auto program = compiled.store.read();
    auto mismatched = snapshot(*program, 8);
    mismatched.tempo_map = foreign.get();
    Output rejected(1, 8);

    REQUIRE(ArrangementAudioRenderer::process(*program, mismatched, rejected.view()) ==
            AudioRenderStatus::InvalidTransport);
    REQUIRE(rejected.storage[0] == std::vector<float>(8, 0.0f));

    CompiledFixture reprepared(project, foreign, pool({{3, data}}));
    auto paired_program = reprepared.store.read();
    Output accepted(1, 8);
    REQUIRE(ArrangementAudioRenderer::process(*paired_program, snapshot(*paired_program, 8),
                                              accepted.view()) == AudioRenderStatus::Rendered);
    REQUIRE(accepted.storage[0] == std::vector<float>(8, 1.0f));
}

TEST_CASE("audio renderer follows transport loop splits and seeks without stale cursors") {
    std::vector<float> ramp(128);
    for (std::size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = static_cast<float>(i);
    const auto data = audio_data({ramp});
    auto clip = absolute_media_clip(100, 0, 128, 3, 0, 128);
    auto track = take(Track::create({10}, "loop", {clip}));
    auto project = project_with_tracks({track}, {{3, "ramp", 128, {48'000, 1}}});
    const auto map = map_120();
    CompiledFixture compiled(project, map, pool({{3, data}}));
    auto program = compiled.store.read();

    MasterTransport transport;
    const auto loop_end = map->samples_to_ticks({64});
    REQUIRE(transport.prepare(*map, {.max_buffer_size = 32,
                                     .loop = {true, {0}, loop_end},
                                     .initial_position = map->samples_to_ticks({48}),
                                     .initially_playing = true}) == TransportError::None);
    TransportSnapshot state;
    REQUIRE(transport.begin_block(32, state) == TransportError::None);
    REQUIRE(state.range_count == 2);
    Output looped(1, 32);
    REQUIRE(ArrangementAudioRenderer::process(*program, state, looped.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(looped.storage[0][0] == 48.0f);
    REQUIRE(looped.storage[0][15] == 63.0f);
    REQUIRE(looped.storage[0][16] == 0.0f);
    REQUIRE(looped.storage[0][31] == 15.0f);

    REQUIRE(transport.seek(map->samples_to_ticks({32})) == TransportError::None);
    REQUIRE(transport.begin_block(16, state) == TransportError::None);
    REQUIRE(state.reset_requested);
    Output seeked(1, 16);
    REQUIRE(ArrangementAudioRenderer::process(*program, state, seeked.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(seeked.storage[0][0] == 32.0f);
}

TEST_CASE("audio renderer mixes tracks and maps mono and stereo deterministically") {
    const auto mono = audio_data({std::vector<float>(64, 1.0f)});
    const auto stereo = audio_data({std::vector<float>(64, 2.0f), std::vector<float>(64, 4.0f)});
    auto mono_track =
        take(Track::create({10}, "mono", {absolute_media_clip(100, 0, 64, 3, 0, 64)}));
    auto stereo_track =
        take(Track::create({11}, "stereo", {absolute_media_clip(101, 0, 64, 4, 0, 64)}));
    auto project = project_with_tracks(
        {stereo_track, mono_track}, {{3, "mono", 64, {48'000, 1}}, {4, "stereo", 64, {48'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{4, stereo}, {3, mono}}));
    auto program = compiled.store.read();

    Output first(2, 64);
    Output second(2, 64);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 64), first.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 64), second.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(first.storage == second.storage);
    REQUIRE(first.storage[0][0] == 3.0f);
    REQUIRE(first.storage[1][0] == 5.0f);

    Output mono_output(1, 64);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 64),
                                              mono_output.view()) == AudioRenderStatus::Rendered);
    REQUIRE(mono_output.storage[0][0] == 4.0f);
}

TEST_CASE("audio renderer zero fills after source EOF and while stopped") {
    const auto data = audio_data({{1.0f, 2.0f, 3.0f, 4.0f}});
    auto clip = absolute_media_clip(100, 0, 8, 3, 0, 4);
    auto track = take(Track::create({10}, "short", {clip}));
    auto project = project_with_tracks({track}, {{3, "short", 4, {48'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, data}}));
    auto program = compiled.store.read();
    Output output(1, 8);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 8), output.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(output.storage[0] == std::vector<float>{1, 2, 3, 4, 0, 0, 0, 0});

    auto stopped = snapshot(*program, 8);
    stopped.is_playing = false;
    REQUIRE(ArrangementAudioRenderer::process(*program, stopped, output.view()) ==
            AudioRenderStatus::Silent);
    REQUIRE(output.storage[0] == std::vector<float>(8, 0.0f));
}

TEST_CASE("audio compiler rejects invalid assets sample rates and capacities") {
    const auto good = audio_data({std::vector<float>(8, 1.0f)});
    auto clip = absolute_media_clip(100, 0, 8, 3, 0, 8);
    auto track = take(Track::create({10}, "bad", {clip}));
    auto project = project_with_tracks({track}, {{3, "declared", 8, {48'000, 1}}});
    const auto map = map_120();

    auto mismatched = pool({{3, audio_data({std::vector<float>(7, 1.0f)})}});
    auto direct = compile_audio_clip_program(clip, *project, *map, *mismatched, {});
    REQUIRE_FALSE(direct);
    REQUIRE(direct.error().code == AudioRendererErrorCode::AssetMetadataMismatch);

    auto wrong_rate = pool({{3, audio_data({std::vector<float>(8, 1.0f)}, 44'100)}});
    direct = compile_audio_clip_program(clip, *project, *map, *wrong_rate, {});
    REQUIRE_FALSE(direct);
    REQUIRE(direct.error().code == AudioRendererErrorCode::AssetMetadataMismatch);

    auto compiled_clip =
        take(compile_audio_clip_program(clip, *project, *map, *pool({{3, good}}), {}));
    auto linked = link_audio_track_program({10}, {compiled_clip}, {.max_clips = 0});
    REQUIRE_FALSE(linked);
    REQUIRE(linked.error().code == AudioRendererErrorCode::CapacityExceeded);

    std::vector<std::vector<float>> excessive_channels(65, std::vector<float>(1, 0.0f));
    auto excessive = std::make_shared<audio::AudioFileData>();
    excessive->sample_rate = 48'000;
    excessive->channels = std::move(excessive_channels);
    auto invalid_pool = DecodedAudioAssetPool::create({{{3}, excessive}});
    REQUIRE_FALSE(invalid_pool);
    REQUIRE(invalid_pool.error().code == AudioRendererErrorCode::InvalidAsset);

    REQUIRE_FALSE(Clip::create_absolute({200}, {0}, 8, {48'000, 1}, MediaRef{{3}, {-1}, 1}));
    REQUIRE_FALSE(Clip::create_absolute({200}, {0}, 8, {48'000, 1}, MediaRef{{3}, {0}, 1},
                                        {.fade_in_duration = 9}));
    REQUIRE_FALSE(Clip::create({200}, {0}, {30}, MediaRef{{3}, {0}, 1}, {.fade_in_duration = 31}));
}

TEST_CASE("audio compiler enforces whole program track capacity") {
    const auto data = audio_data({std::vector<float>(8, 1.0f)});
    auto first = take(Track::create({10}, "first", {absolute_media_clip(100, 0, 8, 3, 0, 8)}));
    auto second = take(Track::create({11}, "second", {absolute_media_clip(101, 0, 8, 3, 0, 8)}));
    auto project = project_with_tracks({first, second}, {{3, "tone", 8, {48'000, 1}}});
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = project;
    request.sequence_id = {2};
    request.tempo_map = map_120();
    request.document_revision = 1;
    request.dirty.all = true;
    request.audio_assets = pool({{3, data}});
    request.audio_limits.max_tracks = 1;
    REQUIRE(compiler.submit(std::move(request)));
    REQUIRE_FALSE(store.has_value());
    REQUIRE(compiler.status().has_error);
    REQUIRE(compiler.status().last_error.code == CompileErrorCode::AudioProgramInvalid);
    REQUIRE(compiler.status().last_error.audio_detail == AudioRendererErrorCode::CapacityExceeded);
}

TEST_CASE("audio renderer enforces runtime clip capacity across the whole program") {
    const auto data = audio_data({std::vector<float>(8, 1.0f)});
    auto first = take(Track::create({10}, "first", {absolute_media_clip(100, 0, 8, 3, 0, 8)}));
    auto second = take(Track::create({11}, "second", {absolute_media_clip(101, 0, 8, 3, 0, 8)}));
    auto project = project_with_tracks({first, second}, {{3, "tone", 8, {48'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, data}}));
    auto program = compiled.store.read();
    Output output(1, 8);

    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 8), output.view(),
                                              {.max_clips = 1}) ==
            AudioRenderStatus::CapacityExceeded);
}

TEST_CASE("audio renderer capacity preflight prevents partial output from a later track") {
    const auto data = audio_data({std::vector<float>(8, 1.0f)});
    auto first = take(Track::create({10}, "first", {absolute_media_clip(100, 0, 8, 3, 0, 8)}));
    auto later = take(Track::create({11}, "later", {absolute_media_clip(101, 0, 8, 3, 0, 8)}));
    auto project = project_with_tracks({first, later}, {{3, "tone", 8, {48'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, data}}));
    auto program = compiled.store.read();
    Output output(2, 8);

    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 8), output.view(),
                                              {.max_clips = 1}) ==
            AudioRenderStatus::CapacityExceeded);
    REQUIRE(output.storage[0] == std::vector<float>(8, 0.0f));
    REQUIRE(output.storage[1] == std::vector<float>(8, 0.0f));
}

TEST_CASE("audio render entry point is allocation free and fails closed") {
    const auto data = audio_data({std::vector<float>(64, 1.0f)});
    auto clip = absolute_media_clip(100, 0, 64, 3, 0, 64);
    auto track = take(Track::create({10}, "rt", {clip}));
    auto project = project_with_tracks({track}, {{3, "tone", 64, {48'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, data}}));
    auto program = compiled.store.read();
    Output output(2, 64);
    auto state = snapshot(*program, 64);
    std::size_t allocations = 1;
    AudioRenderStatus status = AudioRenderStatus::InvalidProgram;
    {
        test::ScopedRtProcessProbe probe;
        status = ArrangementAudioRenderer::process(*program, state, output.view());
        allocations = probe.allocation_count();
    }
    REQUIRE(status == AudioRenderStatus::Rendered);
    REQUIRE(allocations == 0);

    state.ranges[0].frame_count = 65;
    REQUIRE(ArrangementAudioRenderer::process(*program, state, output.view()) ==
            AudioRenderStatus::InvalidTransport);
    REQUIRE(output.storage[0] == std::vector<float>(64, 0.0f));
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 64), output.view(),
                                              {.max_channels = 1}) ==
            AudioRenderStatus::CapacityExceeded);
    REQUIRE(output.storage[1] == std::vector<float>(64, 0.0f));

    CompiledFixture narrow(project, map_120(), pool({{3, data}}), {.max_channels = 1});
    auto narrow_program = narrow.store.read();
    REQUIRE(ArrangementAudioRenderer::process(*narrow_program, snapshot(*narrow_program, 64),
                                              output.view()) ==
            AudioRenderStatus::CapacityExceeded);
}
