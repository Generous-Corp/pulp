#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/program_compiler.hpp>

#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <limits>
#include <memory>
#include <vector>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timeline;
using namespace pulp::timebase;

namespace {

template <typename T, typename E> T take(runtime::Result<T, E> result) {
    REQUIRE(result);
    return std::move(result).value();
}

ContentHash hash_of(char digit) {
    return *ContentHash::from_hex(std::string(64, digit));
}

std::shared_ptr<const audio::AudioFileData> mono(std::vector<float> samples) {
    auto data = std::make_shared<audio::AudioFileData>();
    data->sample_rate = 48'000;
    data->channels = {std::move(samples)};
    return data;
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

struct Output {
    explicit Output(std::size_t frames) : samples(frames, -1.0f), pointers{samples.data()} {}
    audio::BufferView<float> view() {
        return {pointers.data(), 1, samples.size()};
    }
    std::vector<float> samples;
    std::array<float*, 1> pointers;
};

} // namespace

TEST_CASE("Frozen track playback selects one rendered artifact and hides authored sources") {
    auto arrangement =
        take(Clip::create_absolute({11}, {0}, 8, {48'000, 1}, MediaRef{{20}, {0}, 8}));
    TrackFreeze freeze{MediaRef{{21}, {1}, 4}, {2}, {48'000, 1}, hash_of('c')};
    auto track = take(Track::create(TrackInput{.id = {10},
                                               .name = "track",
                                               .clips = {arrangement},
                                               .device_chain = {{{30}}},
                                               .freeze = freeze}));
    auto sequence = take(Sequence::create({2}, "sequence", std::nullopt, std::nullopt, {track}));
    std::vector<MediaAsset> assets{
        {{20},
         "arrangement.wav",
         8,
         {48'000, 1},
         hash_of('a'),
         AssetStoragePolicy::External,
         {},
         {}},
        {{21}, "freeze.wav", 6, {48'000, 1}, hash_of('b'), AssetStoragePolicy::External, {}, {}}};
    auto project = std::make_shared<const Project>(take(
        Project::create(ProjectInput{{1}, "project", 31, {2}, std::move(assets), {sequence}})));
    auto decoded =
        take(DecodedAudioAssetPool::create({{{20}, mono(std::vector<float>(8, 1.0f))},
                                            {{21}, mono({8.0f, 4.0f, 4.0f, 4.0f, 4.0f, 8.0f})}}));
    const std::array tempo_points{TempoPoint{{0}, 120.0}};
    auto tempo = shared_compiled_tempo_map(tempo_points, {48'000, 1});

    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = project;
    request.sequence_id = {2};
    request.tempo_map = tempo;
    request.document_revision = 1;
    request.dirty.all = true;
    request.audio_assets = decoded;
    REQUIRE(compiler.submit(std::move(request)));
    auto program = store.read();
    REQUIRE(program);
    const auto* compiled_track = program->find_track({10});
    REQUIRE(compiled_track);
    REQUIRE(compiled_track->ordered_clip_ids().empty());
    REQUIRE(compiled_track->arrangement_note_events().empty());
    REQUIRE(compiled_track->ordered_device_placement_ids().empty());
    REQUIRE(compiled_track->automation_program() == nullptr);
    REQUIRE(compiled_track->audio_program());
    REQUIRE(compiled_track->audio_program()->clips().size() == 1);
    const auto& artifact = compiled_track->audio_program()->clips()[0];
    REQUIRE(artifact.source_kind == AudioClipRendererProgram::SourceKind::FrozenTrack);
    REQUIRE(artifact.id == ItemId{10});
    REQUIRE(artifact.asset_id == ItemId{21});
    REQUIRE(artifact.source_start == 1);
    REQUIRE(artifact.timeline_start == 2);

    TransportSnapshot transport;
    transport.tempo_map = &program->tempo_map();
    transport.sample_rate = {48'000, 1};
    transport.frame_count = 8;
    transport.is_playing = true;
    transport.range_count = 1;
    transport.ranges[0].frame_count = 8;
    transport.ranges[0].timeline_sample_start = {0};
    Output output(8);
    REQUIRE(ArrangementAudioRenderer::process(*program, transport, output.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(output.samples == std::vector<float>{0.0f, 0.0f, 4.0f, 4.0f, 4.0f, 4.0f, 0.0f, 0.0f});

    auto thawed_track = take(Track::create(
        TrackInput{.id = {10}, .name = "track", .clips = {arrangement}, .device_chain = {{{30}}}}));
    auto thawed_sequence =
        take(Sequence::create({2}, "sequence", std::nullopt, std::nullopt, {thawed_track}));
    std::vector<MediaAsset> thawed_assets{
        {{20},
         "arrangement.wav",
         8,
         {48'000, 1},
         hash_of('a'),
         AssetStoragePolicy::External,
         {},
         {}},
        {{21}, "freeze.wav", 6, {48'000, 1}, hash_of('b'), AssetStoragePolicy::External, {}, {}}};
    ProgramCompileRequest thaw;
    thaw.project = std::make_shared<const Project>(take(Project::create(
        ProjectInput{{1}, "project", 31, {2}, std::move(thawed_assets), {thawed_sequence}})));
    thaw.sequence_id = {2};
    thaw.tempo_map = tempo;
    thaw.document_revision = 2;
    thaw.dirty.tracks = {{10}};
    thaw.audio_assets = decoded;
    REQUIRE(compiler.submit(std::move(thaw)));
    auto thawed_program = store.read();
    REQUIRE(thawed_program);
    const auto* thawed = thawed_program->find_track({10});
    REQUIRE(thawed);
    REQUIRE(thawed->audio_program()->clips().size() == 1);
    REQUIRE(thawed->audio_program()->clips()[0].source_kind ==
            AudioClipRendererProgram::SourceKind::ArrangementClip);
    REQUIRE(thawed->ordered_device_placement_ids().size() == 1);
    REQUIRE(thawed->ordered_device_placement_ids()[0] == ItemId{30});
    REQUIRE(thawed->automation_program() != nullptr);
}

TEST_CASE("Frozen track playback rejects an undecoded artifact before publication") {
    TrackFreeze freeze{MediaRef{{21}, {0}, 4}, {0}, {48'000, 1}, hash_of('c')};
    auto track = take(Track::create(TrackInput{.id = {10}, .name = "track", .freeze = freeze}));
    auto sequence = take(Sequence::create({2}, "sequence", std::nullopt, std::nullopt, {track}));
    auto project = std::make_shared<const Project>(take(Project::create(ProjectInput{
        {1},
        "project",
        30,
        {2},
        {{{21}, "freeze.wav", 4, {48'000, 1}, hash_of('b'), AssetStoragePolicy::External, {}, {}}},
        {sequence}})));
    const std::array tempo_points{TempoPoint{{0}, 120.0}};
    auto tempo = shared_compiled_tempo_map(tempo_points, {48'000, 1});
    auto decoded = take(DecodedAudioAssetPool::create({{{20}, mono(std::vector<float>(4, 1.0f))}}));

    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = project;
    request.sequence_id = {2};
    request.tempo_map = tempo;
    request.document_revision = 1;
    request.dirty.all = true;
    request.audio_assets = decoded;
    REQUIRE(compiler.submit(std::move(request)));
    REQUIRE_FALSE(store.has_value());
    REQUIRE(compiler.status().last_error.audio_detail ==
            AudioRendererErrorCode::MissingDecodedAsset);
}

TEST_CASE("Frozen track playback accounts for capacity and rejects coordinate overflow") {
    const std::array tempo_points{TempoPoint{{0}, 120.0}};
    const auto tempo = shared_compiled_tempo_map(tempo_points, {48'000, 1});
    const auto decoded = take(DecodedAudioAssetPool::create(
        {{{21}, mono(std::vector<float>(4, 1.0f))}, {{22}, mono(std::vector<float>(4, 2.0f))}}));
    auto first = take(Track::create(
        TrackInput{.id = {10},
                   .name = "first",
                   .freeze = TrackFreeze{MediaRef{{21}, {0}, 4}, {0}, {48'000, 1}, hash_of('c')}}));
    auto second = take(Track::create(
        TrackInput{.id = {11},
                   .name = "second",
                   .freeze = TrackFreeze{MediaRef{{22}, {0}, 4}, {0}, {48'000, 1}, hash_of('d')}}));
    auto sequence =
        take(Sequence::create({2}, "sequence", std::nullopt, std::nullopt, {first, second}));
    auto project = std::make_shared<const Project>(take(Project::create(ProjectInput{
        {1},
        "project",
        30,
        {2},
        {{{21}, "first.wav", 4, {48'000, 1}, hash_of('a'), AssetStoragePolicy::External, {}, {}},
         {{22}, "second.wav", 4, {48'000, 1}, hash_of('b'), AssetStoragePolicy::External, {}, {}}},
        {sequence}})));

    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = project;
    request.sequence_id = {2};
    request.tempo_map = tempo;
    request.document_revision = 1;
    request.dirty.all = true;
    request.audio_assets = decoded;
    request.audio_limits.max_clips = 1;
    REQUIRE(compiler.submit(std::move(request)));
    REQUIRE_FALSE(store.has_value());
    REQUIRE(compiler.status().last_error.audio_detail == AudioRendererErrorCode::CapacityExceeded);

    auto overflow = take(
        Track::create(TrackInput{.id = {12},
                                 .name = "overflow",
                                 .freeze = TrackFreeze{MediaRef{{21}, {0}, 4},
                                                       {std::numeric_limits<std::int64_t>::max()},
                                                       {48'000, 1},
                                                       hash_of('e')}}));
    const auto compiled = compile_track_freeze_program(overflow, *project, *tempo, *decoded, {});
    REQUIRE_FALSE(compiled);
    REQUIRE(compiled.error().code == AudioRendererErrorCode::InvalidClipRange);
}
