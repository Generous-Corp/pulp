#include "playback_audio_renderer_test_support.hpp"

#include <pulp/playback/offline_stretch_artifact.hpp>

#include <bit>
#include <deque>
#include <limits>

namespace {

struct StretchFixture {
    Clip clip;
    std::shared_ptr<const Project> project;
    std::shared_ptr<const CompiledTempoMap> map;
    DecodedAudioAsset decoded;
};

ContentHash fixture_content_hash(char digit = 'a') {
    const auto hash = ContentHash::from_hex(std::string(64, digit));
    REQUIRE(hash);
    return *hash;
}

DecodedAudioAsset sealed_decoded(std::shared_ptr<const audio::AudioFileData> audio,
                                 ContentHash content_hash = fixture_content_hash()) {
    auto sealed = take(DecodedAudioAssetPool::create(
        {DecodedAudioAsset{{3}, std::move(audio), content_hash, {}}}));
    const auto* decoded = sealed->find({3});
    REQUIRE(decoded);
    return *decoded;
}

class QueueExecutor final : public CompileExecutor {
  public:
    bool submit(std::unique_ptr<CompileTask> task, std::chrono::steady_clock::time_point) override {
        tasks.push_back(std::move(task));
        return true;
    }

    void run_one() {
        REQUIRE_FALSE(tasks.empty());
        auto task = std::move(tasks.front());
        tasks.pop_front();
        if (task->run_slice({std::chrono::steady_clock::now() + std::chrono::seconds(1), 1}) ==
            CompileTaskStatus::Pending)
            tasks.push_front(std::move(task));
    }

    std::deque<std::unique_ptr<CompileTask>> tasks;
};

StretchFixture stretch_fixture(std::uint32_t source_rate = 4'800,
                               std::uint32_t timeline_rate = 4'800) {
    const auto source_frames = static_cast<std::uint64_t>(source_rate / 2u);
    std::vector<float> source(static_cast<std::size_t>(source_frames));
    for (std::size_t frame = 0; frame < source.size(); ++frame)
        source[frame] = 0.35f * std::sin(static_cast<float>(frame) * 0.071f) +
                        0.1f * std::cos(static_cast<float>(frame) * 0.193f);
    auto clip =
        musical_media_clip(100, 0, kTicksPerQuarter, 3, source_frames, {}, TimeConform::Stretch);
    auto track = take(Track::create({10}, "offline stretch", {clip}));
    auto project =
        project_with_tracks({track}, {{3, "finite.wav", source_frames, {source_rate, 1}}});
    const std::array points{TempoPoint{{0}, 120.0}};
    return {clip,
            project,
            shared_compiled_tempo_map(points, RationalRate{timeline_rate, 1}),
            sealed_decoded(audio_data({std::move(source)}, source_rate))};
}

StretchFixture stretch_ramp_fixture() {
    constexpr std::uint32_t sample_rate = 4'800;
    constexpr std::uint64_t source_frames = 2'400;
    constexpr std::int64_t duration = kTicksPerQuarter / 2;
    std::vector<float> source(source_frames);
    for (std::size_t frame = 0; frame < source.size(); ++frame)
        source[frame] = std::sin(static_cast<float>(frame) * 0.071f);
    auto clip = musical_media_clip(100, 0, duration, 3, source_frames, {}, TimeConform::Stretch);
    auto track = take(Track::create({10}, "offline ramp stretch", {clip}));
    const std::array points{
        TempoPoint{{0}, 60.0, TempoCurve::LinearInTicks},
        TempoPoint{{kTicksPerQuarter}, 180.0, TempoCurve::Constant},
    };
    auto sequence = take(Sequence::create({2}, "root", std::nullopt, std::nullopt,
                                          std::vector<Track>{std::move(track)}));
    ProjectInput input;
    input.id = {1};
    input.name = "offline ramp stretch";
    input.next_item_id = 1'000;
    input.root_sequence_id = {2};
    input.assets = {{3, "finite-ramp.wav", source_frames, {sample_rate, 1},
                     fixture_content_hash()}};
    input.sequences.push_back(std::move(sequence));
    input.tempo_map = take(TempoMap::create(points));
    auto project = std::make_shared<const Project>(take(Project::create(std::move(input))));
    return {clip,
            project,
            shared_compiled_tempo_map(points, RationalRate{sample_rate, 1}),
            sealed_decoded(audio_data({std::move(source)}, sample_rate))};
}

std::shared_ptr<const OfflineStretchArtifact>
compile_stretch(const StretchFixture& fixture, std::uint32_t work_block_frames,
                OfflineStretchArtifactCache* cache = nullptr, OfflineStretchLimits limits = {}) {
    OfflineStretchCompileJob job;
    auto status = job.begin(fixture.clip, *fixture.project, *fixture.map, fixture.decoded, limits,
                            {}, work_block_frames, cache);
    for (std::size_t guard = 0; guard < 100'000 && status == OfflineStretchCompileStatus::Progress;
         ++guard)
        status = job.step();
    INFO("offline stretch error " << static_cast<int>(job.error().code));
    REQUIRE(status == OfflineStretchCompileStatus::Complete);
    auto artifact = job.take();
    REQUIRE(artifact);
    return artifact;
}

void require_bit_identical(const audio::AudioFileData& left, const audio::AudioFileData& right) {
    REQUIRE(left.sample_rate == right.sample_rate);
    REQUIRE(left.channels.size() == right.channels.size());
    for (std::size_t channel = 0; channel < left.channels.size(); ++channel) {
        REQUIRE(left.channels[channel].size() == right.channels[channel].size());
        for (std::size_t frame = 0; frame < left.channels[channel].size(); ++frame) {
            REQUIRE(std::isfinite(left.channels[channel][frame]));
            REQUIRE(std::isfinite(right.channels[channel][frame]));
            REQUIRE(std::bit_cast<std::uint32_t>(left.channels[channel][frame]) ==
                    std::bit_cast<std::uint32_t>(right.channels[channel][frame]));
        }
    }
}

} // namespace

TEST_CASE("offline Stretch publishes exact immutable timeline-rate audio",
          "[playback][offline-stretch]") {
    const auto fixture = stretch_fixture(4'410, 4'800);
    const auto artifact = compile_stretch(fixture, 37);
    REQUIRE(artifact->audio->sample_rate == 4'800);
    REQUIRE(artifact->audio->num_frames() == 2'400);
    REQUIRE(artifact->key.timeline_input_frame_count == 2'400);
    REQUIRE(artifact->key.target_frame_count == 2'400);
    REQUIRE(artifact->key.source_sample_rate == RationalRate{4'410, 1});
    REQUIRE(artifact->key.timeline_sample_rate == RationalRate{4'800, 1});
}

TEST_CASE("offline Stretch output and cache identity ignore compiler work block size",
          "[playback][offline-stretch]") {
    const auto fixture = stretch_fixture(4'410, 4'800);
    const auto tiny = compile_stretch(fixture, 37);
    const auto repeated = compile_stretch(fixture, 37);
    const auto wide = compile_stretch(fixture, 251);
    REQUIRE(tiny->key == wide->key);
    require_bit_identical(*tiny->audio, *repeated->audio);
    require_bit_identical(*tiny->audio, *wide->audio);

    OfflineStretchArtifactCache cache;
    auto inserted = compile_stretch(fixture, 37, &cache);
    OfflineStretchCompileJob hit;
    REQUIRE(hit.begin(fixture.clip, *fixture.project, *fixture.map, fixture.decoded, {}, {}, 251,
                      &cache) == OfflineStretchCompileStatus::Complete);
    REQUIRE(hit.cache_hit());
    auto reused = hit.take();
    REQUIRE(reused == inserted);

    const auto equivalent = musical_media_clip(
        101, 0, kTicksPerQuarter, 3, fixture.decoded.audio->num_frames(), {}, TimeConform::Stretch);
    OfflineStretchCompileJob other_clip;
    REQUIRE(other_clip.begin(equivalent, *fixture.project, *fixture.map, fixture.decoded, {}, {},
                             251, &cache) == OfflineStretchCompileStatus::Complete);
    REQUIRE(other_clip.cache_hit());
    REQUIRE(other_clip.take() == inserted);

    auto negative_key = inserted->key;
    ++negative_key.algorithm.version;
    REQUIRE_FALSE(negative_key == inserted->key);
    REQUIRE_FALSE(cache.find(negative_key));

    auto changed_audio = std::make_shared<audio::AudioFileData>(*fixture.decoded.audio);
    changed_audio->channels[0][0] += 0.25f;
    const auto changed_decoded = sealed_decoded(std::move(changed_audio));
    REQUIRE(changed_decoded.content_hash == fixture.decoded.content_hash);
    REQUIRE(changed_decoded.decoded_content_hash != fixture.decoded.decoded_content_hash);
    OfflineStretchCompileJob changed_source;
    auto changed_status = changed_source.begin(fixture.clip, *fixture.project, *fixture.map,
                                               changed_decoded, {}, {}, 251, &cache);
    REQUIRE_FALSE(changed_source.cache_hit());
    for (std::size_t guard = 0;
         guard < 100'000 && changed_status == OfflineStretchCompileStatus::Progress; ++guard)
        changed_status = changed_source.step();
    REQUIRE(changed_status == OfflineStretchCompileStatus::Complete);
    const auto changed_artifact = changed_source.take();
    REQUIRE(changed_artifact);
    REQUIRE(changed_artifact != inserted);
    REQUIRE(changed_artifact->key.decoded_content_hash == changed_decoded.decoded_content_hash);
    REQUIRE(cache.size() == 2);

    std::weak_ptr<const OfflineStretchArtifact> released = inserted;
    inserted.reset();
    reused.reset();
    OfflineStretchLimits constrained;
    constrained.max_cached_artifacts = 0;
    cache.constrain(constrained);
    REQUIRE(cache.size() == 0);
    REQUIRE(cache.retained_bytes() == 0);
    REQUIRE(released.expired());

    const auto reinserted = compile_stretch(fixture, 37, &cache);
    REQUIRE(reinserted);
    REQUIRE(cache.size() == 1);
    REQUIRE(cache.retained_bytes() > 0);
    cache.clear();
    REQUIRE(cache.size() == 0);
    REQUIRE(cache.retained_bytes() == 0);
}

TEST_CASE("offline Stretch rejects a compiled tempo map from different project points",
          "[playback][offline-stretch]") {
    const auto fixture = stretch_fixture();
    const std::array wrong_points{TempoPoint{{0}, 60.0}};
    const auto wrong_map =
        shared_compiled_tempo_map(wrong_points, fixture.map->sample_rate());
    REQUIRE_FALSE(wrong_map->matches(fixture.project->tempo_map().points()));
    REQUIRE(fixture.map->matches(fixture.project->tempo_map().points()));

    OfflineStretchArtifactCache cache;
    OfflineStretchCompileJob job;
    REQUIRE(job.begin(fixture.clip, *fixture.project, *wrong_map, fixture.decoded, {}, {}, 37,
                      &cache) == OfflineStretchCompileStatus::Failed);
    REQUIRE(job.error().code == OfflineStretchErrorCode::InvalidTempoSchedule);
    REQUIRE_FALSE(job.cache_hit());
    REQUIRE(cache.size() == 0);
}

TEST_CASE("offline Stretch cache enforces aggregate artifact count and byte limits",
          "[playback][offline-stretch]") {
    const auto fixture = stretch_fixture();
    auto changed_audio = std::make_shared<audio::AudioFileData>(*fixture.decoded.audio);
    changed_audio->channels[0][0] += 0.5f;
    const auto changed_decoded = sealed_decoded(std::move(changed_audio));
    REQUIRE(changed_decoded.content_hash == fixture.decoded.content_hash);
    REQUIRE(changed_decoded.decoded_content_hash != fixture.decoded.decoded_content_hash);

    const auto require_aggregate_rejection = [&](OfflineStretchArtifactCache& cache,
                                                  OfflineStretchLimits limits) {
        OfflineStretchCompileJob job;
        auto status = job.begin(fixture.clip, *fixture.project, *fixture.map, changed_decoded,
                                limits, {}, 37, &cache);
        REQUIRE_FALSE(job.cache_hit());
        for (std::size_t guard = 0;
             guard < 100'000 && status == OfflineStretchCompileStatus::Progress; ++guard)
            status = job.step();
        REQUIRE(status == OfflineStretchCompileStatus::Failed);
        REQUIRE(job.error().code == OfflineStretchErrorCode::CapacityExceeded);
        REQUIRE(cache.size() == 1);
    };

    OfflineStretchLimits count_limits;
    count_limits.max_cached_artifacts = 1;
    OfflineStretchArtifactCache count_cache;
    REQUIRE(compile_stretch(fixture, 37, &count_cache, count_limits));
    require_aggregate_rejection(count_cache, count_limits);

    OfflineStretchArtifactCache byte_cache;
    REQUIRE(compile_stretch(fixture, 37, &byte_cache));
    OfflineStretchLimits byte_limits;
    byte_limits.max_cache_bytes = byte_cache.retained_bytes();
    REQUIRE(byte_limits.max_cache_bytes > 0);
    require_aggregate_rejection(byte_cache, byte_limits);
}

TEST_CASE("offline Stretch tempo-ramp schedule is endpoint exact and work-block invariant",
          "[playback][offline-stretch]") {
    const auto fixture = stretch_ramp_fixture();
    const auto tiny = compile_stretch(fixture, 37);
    const auto repeated = compile_stretch(fixture, 37);
    const auto wide = compile_stretch(fixture, 251);
    REQUIRE(tiny->key == wide->key);
    require_bit_identical(*tiny->audio, *repeated->audio);
    require_bit_identical(*tiny->audio, *wide->audio);
    const auto start = fixture.map->ticks_to_samples(fixture.clip.start()).value;
    const auto end = fixture.map->ticks_to_samples(fixture.clip.end()).value;
    std::uint64_t exact_target = 0;
    REQUIRE(pulp::playback::detail::offline_stretch_frame_distance(start, end, exact_target));
    REQUIRE(tiny->audio->num_frames() == exact_target);
    const auto raw_target = fixture.map->fractional_ticks_to_samples(
                                static_cast<long double>(fixture.clip.end().value)) -
                            fixture.map->fractional_ticks_to_samples(
                                static_cast<long double>(fixture.clip.start().value));
    REQUIRE(std::abs(raw_target - static_cast<long double>(exact_target)) > 1.0e-6L);
}

TEST_CASE("offline Stretch rejects invalid cache entries and typed byte capacity",
          "[playback][offline-stretch]") {
    OfflineStretchArtifactCache cache;
    auto invalid = std::make_shared<OfflineStretchArtifact>();
    REQUIRE_FALSE(cache.insert(invalid, {}));
    invalid->audio = audio_data({{1.0f}}, 4'800);
    invalid->key.timeline_sample_rate = {4'800, 1};
    invalid->key.target_frame_count = 2;
    invalid->key.channel_count = 1;
    REQUIRE_FALSE(cache.insert(invalid, {}));
    REQUIRE(cache.size() == 0);

    const auto fixture = stretch_fixture();
    OfflineStretchLimits limits;
    limits.max_input_bytes = 1;
    OfflineStretchCompileJob job;
    REQUIRE(job.begin(fixture.clip, *fixture.project, *fixture.map, fixture.decoded, limits) ==
            OfflineStretchCompileStatus::Failed);
    REQUIRE(job.error().code == OfflineStretchErrorCode::CapacityExceeded);
    REQUIRE(cache.size() == 0);

    limits = {};
    limits.max_artifact_bytes = 1;
    OfflineStretchCompileJob artifact_limited;
    auto status = artifact_limited.begin(fixture.clip, *fixture.project, *fixture.map,
                                         fixture.decoded, limits);
    for (std::size_t guard = 0; guard < 100'000 && status == OfflineStretchCompileStatus::Progress;
         ++guard)
        status = artifact_limited.step();
    REQUIRE(status == OfflineStretchCompileStatus::Failed);
    REQUIRE(artifact_limited.error().code == OfflineStretchErrorCode::CapacityExceeded);

    limits = {};
    limits.max_output_bytes = 1;
    OfflineStretchCompileJob output_limited;
    REQUIRE(output_limited.begin(fixture.clip, *fixture.project, *fixture.map, fixture.decoded,
                                 limits) == OfflineStretchCompileStatus::Failed);
    REQUIRE(output_limited.error().code == OfflineStretchErrorCode::CapacityExceeded);

    limits = {};
    limits.max_scratch_allocation_bytes = 1;
    OfflineStretchCompileJob scratch_limited;
    status = scratch_limited.begin(fixture.clip, *fixture.project, *fixture.map, fixture.decoded,
                                   limits);
    for (std::size_t guard = 0; guard < 100'000 && status == OfflineStretchCompileStatus::Progress;
         ++guard)
        status = scratch_limited.step();
    REQUIRE(status == OfflineStretchCompileStatus::Failed);
    REQUIRE(scratch_limited.error().code == OfflineStretchErrorCode::CapacityExceeded);

    const auto resampled = stretch_fixture(4'410, 4'800);
    limits = {};
    limits.max_sample_rate_converter_bytes = 1;
    OfflineStretchCompileJob converter_limited;
    REQUIRE(converter_limited.begin(resampled.clip, *resampled.project, *resampled.map,
                                    resampled.decoded,
                                    limits) == OfflineStretchCompileStatus::Failed);
    REQUIRE(converter_limited.error().code == OfflineStretchErrorCode::CapacityExceeded);

    OfflineStretchCompileJob invalid_block;
    REQUIRE(invalid_block.begin(fixture.clip, *fixture.project, *fixture.map, fixture.decoded, {},
                                {}, 0) == OfflineStretchCompileStatus::Failed);
    REQUIRE(invalid_block.error().code == OfflineStretchErrorCode::InvalidAlgorithmConfig);

    OfflineStretchCompileJob invalid_algorithm;
    REQUIRE(invalid_algorithm.begin(fixture.clip, *fixture.project, *fixture.map, fixture.decoded,
                                    {}, {0, 16.0f}) == OfflineStretchCompileStatus::Failed);
    REQUIRE(invalid_algorithm.error().code == OfflineStretchErrorCode::InvalidAlgorithmConfig);

    OfflineStretchArtifactCache disabled_cache;
    limits = {};
    limits.max_cached_artifacts = 0;
    OfflineStretchCompileJob cache_limited;
    status = cache_limited.begin(fixture.clip, *fixture.project, *fixture.map, fixture.decoded,
                                 limits, {}, 37, &disabled_cache);
    for (std::size_t guard = 0; guard < 100'000 && status == OfflineStretchCompileStatus::Progress;
         ++guard)
        status = cache_limited.step();
    REQUIRE(status == OfflineStretchCompileStatus::Failed);
    REQUIRE(cache_limited.error().code == OfflineStretchErrorCode::CapacityExceeded);
    REQUIRE(disabled_cache.size() == 0);
}

TEST_CASE("offline Stretch frame distance is exact across signed boundaries",
          "[playback][offline-stretch]") {
    std::uint64_t distance = 0;
    REQUIRE(pulp::playback::detail::offline_stretch_frame_distance(-7, 5, distance));
    REQUIRE(distance == 12);
    REQUIRE(pulp::playback::detail::offline_stretch_frame_distance(
        std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max(), distance));
    REQUIRE(distance == std::numeric_limits<std::uint64_t>::max());
    REQUIRE_FALSE(pulp::playback::detail::offline_stretch_frame_distance(5, -7, distance));
    REQUIRE_FALSE(pulp::playback::detail::offline_stretch_frame_distance(5, 5, distance));
}

TEST_CASE("offline Stretch preserves typed finite-render failures",
          "[playback][offline-stretch]") {
    REQUIRE(pulp::playback::detail::offline_stretch_error_code(
                audio::FiniteTimeStretchFailure::InvalidRatio) ==
            OfflineStretchErrorCode::InvalidRatio);
    REQUIRE(
        pulp::playback::detail::offline_stretch_error_code(
            audio::FiniteTimeStretchFailure::OutputTooShort) ==
        OfflineStretchErrorCode::OutputTooShort);
    REQUIRE(pulp::playback::detail::offline_stretch_error_code(
                audio::FiniteTimeStretchFailure::OutputTooLong) ==
            OfflineStretchErrorCode::OutputTooLong);
}

TEST_CASE("synchronous audio clip compilation fails closed for offline Stretch",
          "[playback][offline-stretch]") {
    const auto fixture = stretch_fixture();
    const auto assets = pool({fixture.decoded});
    const auto compiled =
        compile_audio_clip_program(fixture.clip, *fixture.project, *fixture.map, *assets, {});
    REQUIRE_FALSE(compiled);
    REQUIRE(compiled.error().code == AudioRendererErrorCode::OfflineStretchRequired);
}

TEST_CASE("program compiler publishes and renders offline Stretch one-to-one",
          "[playback][offline-stretch]") {
    const auto fixture = stretch_fixture(4'410, 4'800);
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = fixture.project;
    request.sequence_id = {2};
    request.tempo_map = fixture.map;
    request.document_revision = 7;
    request.dirty.all = true;
    request.audio_assets = pool({fixture.decoded});
    REQUIRE(compiler.submit(std::move(request)));
    INFO("compile error " << static_cast<int>(compiler.status().last_error.code) << " offline "
                          << static_cast<int>(compiler.status().last_error.offline_stretch_detail));
    REQUIRE(store.has_value());

    const auto program = store.read();
    REQUIRE(program->document_revision() == 7);
    const auto* track = program->find_track({10});
    REQUIRE(track);
    REQUIRE(track->audio_program());
    REQUIRE(track->audio_program()->clips().size() == 1);
    const auto& compiled = track->audio_program()->clips().front();
    REQUIRE(compiled.source_time_mapping ==
            AudioClipRendererProgram::SourceTimeMapping::OfflineStretchArtifact);
    REQUIRE(compiled.offline_stretch_artifact);
    REQUIRE(compiled.offline_stretch_provenance);
    REQUIRE(compiled.offline_stretch_provenance->clip_id == fixture.clip.id());
    REQUIRE(compiled.offline_stretch_provenance->project_id == fixture.project->id());
    REQUIRE(compiled.offline_stretch_provenance->document_revision == 7);
    REQUIRE(compiled.offline_stretch_provenance->program_generation == program->generation());
    REQUIRE(compiled.offline_stretch_provenance->matches(
        fixture.clip.id(), fixture.project->id(), 7, program->generation()));
    REQUIRE(compiled.source_frames_per_timeline_frame == 1.0);
    REQUIRE(compiled.source_frame_count == compiled.timeline_frame_count);

    Output output(1, 128);
    {
        test::RtAllocationProbe probe;
        REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 128),
                                                  output.view()) ==
                AudioRenderStatus::Rendered);
        REQUIRE(probe.allocation_count() == 0);
    }
    for (std::size_t frame = 0; frame < output.storage[0].size(); ++frame)
        REQUIRE(std::bit_cast<std::uint32_t>(output.storage[0][frame]) ==
                std::bit_cast<std::uint32_t>(
                    compiled.offline_stretch_artifact->audio->channels[0][frame]));

    auto missing_provenance = compiled;
    missing_provenance.offline_stretch_provenance.reset();
    const auto missing_link = link_audio_track_program({10}, {missing_provenance}, {});
    REQUIRE_FALSE(missing_link);
    REQUIRE(missing_link.error().code == AudioRendererErrorCode::InvalidAsset);

    auto wrong_provenance = compiled;
    wrong_provenance.offline_stretch_provenance =
        std::make_shared<const OfflineStretchProvenance>(OfflineStretchProvenance{
            {999}, fixture.project->id(), 7, program->generation(), false});
    const auto wrong_link = link_audio_track_program({10}, {wrong_provenance}, {});
    REQUIRE_FALSE(wrong_link);
    REQUIRE(wrong_link.error().code == AudioRendererErrorCode::InvalidAsset);
    REQUIRE_FALSE(wrong_provenance.offline_stretch_provenance->matches(
        fixture.clip.id(), fixture.project->id(), 7, program->generation()));
    REQUIRE_FALSE(compiled.offline_stretch_provenance->matches(
        fixture.clip.id(), {999}, 7, program->generation()));
    REQUIRE_FALSE(compiled.offline_stretch_provenance->matches(
        fixture.clip.id(), fixture.project->id(), 8, program->generation()));
    REQUIRE_FALSE(compiled.offline_stretch_provenance->matches(
        fixture.clip.id(), fixture.project->id(), 7, program->generation() + 1));
}

TEST_CASE("incremental compilation refreshes unchanged offline Stretch provenance",
          "[playback][offline-stretch]") {
    const auto fixture = stretch_fixture();
    auto stretch_track = take(Track::create({10}, "offline stretch", {fixture.clip}));
    auto other_track = take(Track::create({11}, "independently dirty", {}));
    const auto project = project_with_tracks(
        {std::move(stretch_track), std::move(other_track)},
        {{3, "finite.wav", fixture.decoded.audio->num_frames(),
          {fixture.decoded.audio->sample_rate, 1}}});
    const auto assets = pool({fixture.decoded});
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto request = [&](std::uint64_t revision, DirtyTrackSet dirty) {
        ProgramCompileRequest value;
        value.project = project;
        value.sequence_id = {2};
        value.tempo_map = fixture.map;
        value.document_revision = revision;
        value.dirty = std::move(dirty);
        value.audio_assets = assets;
        return value;
    };

    REQUIRE(compiler.submit(request(1, {.all = true})));
    REQUIRE(store.has_value());
    const auto first = store.read();
    const auto* first_track = first->find_track({10});
    REQUIRE(first_track);
    REQUIRE(first_track->audio_program());
    const auto first_artifact =
        first_track->audio_program()->clips().front().offline_stretch_artifact;
    REQUIRE(first_artifact);

    REQUIRE(compiler.submit(request(2, {.tracks = {{11}}})));
    const auto second = store.read();
    REQUIRE(second->document_revision() == 2);
    REQUIRE(second->generation() != first->generation());
    const auto* refreshed_track = second->find_track({10});
    REQUIRE(refreshed_track);
    REQUIRE(refreshed_track != first_track);
    REQUIRE(refreshed_track->audio_program());
    const auto& refreshed_clip = refreshed_track->audio_program()->clips().front();
    REQUIRE(refreshed_clip.offline_stretch_artifact == first_artifact);
    REQUIRE(refreshed_clip.offline_stretch_provenance);
    REQUIRE(refreshed_clip.offline_stretch_provenance->cache_hit);
    REQUIRE(refreshed_clip.offline_stretch_provenance->matches(
        fixture.clip.id(), project->id(), 2, second->generation()));
}

TEST_CASE("superseded offline Stretch programs never publish stale revisions",
          "[playback][offline-stretch]") {
    const auto fixture = stretch_fixture();
    const auto assets = pool({fixture.decoded});
    PlaybackProgramStore store;
    QueueExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto request = [&](std::uint64_t revision) {
        ProgramCompileRequest value;
        value.project = fixture.project;
        value.sequence_id = {2};
        value.tempo_map = fixture.map;
        value.document_revision = revision;
        value.dirty.all = true;
        value.audio_assets = assets;
        return value;
    };

    REQUIRE(compiler.submit(request(1)));
    for (std::size_t slice = 0; slice < 10; ++slice) {
        executor.run_one();
        REQUIRE_FALSE(store.has_value());
    }
    REQUIRE(compiler.submit(request(2)));
    std::size_t guard = 0;
    while (!executor.tasks.empty() && guard++ < 200'000) {
        executor.run_one();
        if (store.has_value())
            REQUIRE(store.read()->document_revision() != 1);
    }
    REQUIRE(executor.tasks.empty());
    REQUIRE(store.has_value());
    const auto program = store.read();
    REQUIRE(program->document_revision() == 2);
    const auto* track = program->find_track({10});
    REQUIRE(track);
    REQUIRE(track->audio_program());
    const auto provenance = track->audio_program()->clips().front().offline_stretch_provenance;
    REQUIRE(provenance);
    REQUIRE(provenance->cache_hit);
}
