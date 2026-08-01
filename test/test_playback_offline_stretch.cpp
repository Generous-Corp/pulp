#include "playback_audio_renderer_test_support.hpp"

#include <pulp/playback/offline_stretch_artifact.hpp>
#include <pulp/playback/realtime_stretch_renderer.hpp>

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
                               std::uint32_t timeline_rate = 4'800,
                               std::int64_t duration_ticks = kTicksPerQuarter) {
    const auto source_frames =
        static_cast<std::uint64_t>(static_cast<long double>(source_rate) * duration_ticks /
                                   (2.0L * static_cast<long double>(kTicksPerQuarter)));
    std::vector<float> source(static_cast<std::size_t>(source_frames));
    for (std::size_t frame = 0; frame < source.size(); ++frame)
        source[frame] = 0.35f * std::sin(static_cast<float>(frame) * 0.071f) +
                        0.1f * std::cos(static_cast<float>(frame) * 0.193f);
    auto clip =
        musical_media_clip(100, 0, duration_ticks, 3, source_frames, {}, TimeConform::Stretch);
    auto track = take(Track::create({10}, "offline stretch", {clip}));
    auto project =
        project_with_tracks({track}, {{3, "finite.wav", source_frames, {source_rate, 1}}});
    const std::array points{TempoPoint{{0}, 120.0}};
    return {clip, project, shared_compiled_tempo_map(points, RationalRate{timeline_rate, 1}),
            sealed_decoded(audio_data({std::move(source)}, source_rate))};
}

AutomationLane stretch_gain_lane() {
    auto curve = take(AutomationCurve::create(
        {AutomationPoint{{41}, {0}, 0.2f, AutomationInterpolation::Continuous, 0.0f},
         AutomationPoint{
             {42}, {kTicksPerQuarter}, 0.8f, AutomationInterpolation::Continuous, 0.0f}}));
    return take(AutomationLane::create({40}, TrackMixerTarget{TrackMixerParameter::Gain},
                                       std::move(curve)));
}

StretchFixture stretch_ramp_fixture(bool automate_gain = false) {
    constexpr std::uint32_t sample_rate = 4'800;
    constexpr std::uint64_t source_frames = 2'400;
    constexpr std::int64_t duration = kTicksPerQuarter / 2;
    std::vector<float> source(source_frames);
    for (std::size_t frame = 0; frame < source.size(); ++frame)
        source[frame] = std::sin(static_cast<float>(frame) * 0.071f);
    auto clip = musical_media_clip(100, 0, duration, 3, source_frames, {}, TimeConform::Stretch);
    TrackInput track_input{.id = {10}, .name = "offline ramp stretch", .clips = {clip}};
    if (automate_gain)
        track_input.automation_lanes = {stretch_gain_lane()};
    auto track = take(Track::create(std::move(track_input)));
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
    input.assets = {
        {3, "finite-ramp.wav", source_frames, {sample_rate, 1}, fixture_content_hash()}};
    input.sequences.push_back(std::move(sequence));
    input.tempo_map = take(TempoMap::create(points));
    auto project = std::make_shared<const Project>(take(Project::create(std::move(input))));
    return {clip, project, shared_compiled_tempo_map(points, RationalRate{sample_rate, 1}),
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
    const auto wrong_map = shared_compiled_tempo_map(wrong_points, fixture.map->sample_rate());
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
        std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max(),
        distance));
    REQUIRE(distance == std::numeric_limits<std::uint64_t>::max());
    REQUIRE_FALSE(pulp::playback::detail::offline_stretch_frame_distance(5, -7, distance));
    REQUIRE_FALSE(pulp::playback::detail::offline_stretch_frame_distance(5, 5, distance));
}

TEST_CASE("offline Stretch preserves typed finite-render failures", "[playback][offline-stretch]") {
    REQUIRE(pulp::playback::detail::offline_stretch_error_code(
                audio::FiniteTimeStretchFailure::InvalidRatio) ==
            OfflineStretchErrorCode::InvalidRatio);
    REQUIRE(pulp::playback::detail::offline_stretch_error_code(
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
    request.sample_rate = request.tempo_map->sample_rate();
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
    REQUIRE(compiled.offline_stretch_provenance->matches(fixture.clip.id(), fixture.project->id(),
                                                         7, program->generation()));
    REQUIRE(compiled.source_frames_per_timeline_frame == 1.0);
    REQUIRE(compiled.source_frame_count == compiled.timeline_frame_count);

    Output output(1, 128);
    {
        test::RtAllocationProbe probe;
        REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 128),
                                                  output.view()) == AudioRenderStatus::Rendered);
        REQUIRE(probe.allocation_count() == 0);
    }
    for (std::size_t frame = 0; frame < output.storage[0].size(); ++frame)
        REQUIRE(std::bit_cast<std::uint32_t>(output.storage[0][frame]) ==
                std::bit_cast<std::uint32_t>(
                    compiled.offline_stretch_artifact->audio->channels[0][frame]));

    auto host_mapped = [&](std::int64_t sample_start) {
        auto state = snapshot(*program, 128, sample_start);
        auto& range = state.ranges[0];
        range.timeline_tick_start = program->tempo_map().samples_to_ticks({sample_start});
        range.timeline_tick_end = program->tempo_map().samples_to_ticks({sample_start + 128});
        range.host_beat_mapping = true;
        range.has_precise_host_ticks = true;
        range.host_tick_start = static_cast<double>(range.timeline_tick_start.value);
        range.host_tick_end = static_cast<double>(range.timeline_tick_end.value);
        return state;
    };
    Output live_output(1, 128);
    auto mapped = host_mapped(0);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, live_output.view()) ==
            AudioRenderStatus::RealtimeStretchStateRequired);
    REQUIRE(std::all_of(live_output.storage[0].begin(), live_output.storage[0].end(),
                        [](float sample) { return sample == 0.0f; }));

    ArrangementAudioTrackRenderer live_renderer({10});
    PlaybackProgramBlock block(program.get());
    REQUIRE(live_renderer.process(block, mapped, live_output.view()) ==
            AudioRenderStatus::RealtimeStretchStateRequired);
    RealtimeStretchProgramRuntime unprepared_runtime;
    REQUIRE(live_renderer.process(block, mapped, live_output.view(), {}, &unprepared_runtime) ==
            AudioRenderStatus::RealtimeStretchStateRequired);
    REQUIRE(std::all_of(live_output.storage[0].begin(), live_output.storage[0].end(),
                        [](float sample) { return sample == 0.0f; }));
    auto wrong_publication = std::make_shared<const PlaybackProgram>(*program);
    RealtimeStretchProgramRuntime wrong_runtime;
    REQUIRE(wrong_runtime.prepare(*wrong_publication, 4'800.0, 128, 1,
                                  wrong_publication->audio_limits()));
    REQUIRE(live_renderer.process(block, mapped, live_output.view(), {}, &wrong_runtime) ==
            AudioRenderStatus::RealtimeStretchStalePublication);
    REQUIRE(std::all_of(live_output.storage[0].begin(), live_output.storage[0].end(),
                        [](float sample) { return sample == 0.0f; }));

    auto old_track = take(Track::create({10}, "same id without Stretch", {}));
    CompiledFixture old_no_stretch(project_with_tracks({std::move(old_track)}, {}), fixture.map,
                                   pool({}));
    const auto old_program = old_no_stretch.store.read();
    RealtimeStretchProgramRuntime old_no_stretch_runtime;
    REQUIRE(
        old_no_stretch_runtime.prepare(*old_program, 4'800.0, 128, 1, old_program->audio_limits()));
    REQUIRE(live_renderer.process(block, mapped, live_output.view(), {}, &old_no_stretch_runtime) ==
            AudioRenderStatus::RealtimeStretchStalePublication);
    REQUIRE(std::all_of(live_output.storage[0].begin(), live_output.storage[0].end(),
                        [](float sample) { return sample == 0.0f; }));

    auto mixed_mapping = mapped;
    mixed_mapping.range_count = 2;
    mixed_mapping.ranges[0].frame_count = 64;
    mixed_mapping.ranges[0].timeline_tick_end = program->tempo_map().samples_to_ticks({64});
    mixed_mapping.ranges[0].host_tick_end =
        static_cast<double>(mixed_mapping.ranges[0].timeline_tick_end.value);
    mixed_mapping.ranges[1] = host_mapped(64).ranges[0];
    mixed_mapping.ranges[1].sample_offset = 64;
    mixed_mapping.ranges[1].frame_count = 64;
    mixed_mapping.ranges[1].timeline_tick_end = program->tempo_map().samples_to_ticks({128});
    mixed_mapping.ranges[1].host_tick_end =
        static_cast<double>(mixed_mapping.ranges[1].timeline_tick_end.value);
    mixed_mapping.ranges[1].host_beat_mapping = false;
    mixed_mapping.ranges[1].has_precise_host_ticks = false;
    mixed_mapping.ranges[1].discontinuity = true;
    RealtimeStretchProgramRuntime mixed_runtime;
    REQUIRE(mixed_runtime.prepare(*program, 4'800.0, 128, 1, program->audio_limits()));
    REQUIRE(live_renderer.process(block, mixed_mapping, live_output.view(), {}, &mixed_runtime) ==
            AudioRenderStatus::RealtimeStretchStateRequired);
    REQUIRE(std::all_of(live_output.storage[0].begin(), live_output.storage[0].end(),
                        [](float sample) { return sample == 0.0f; }));

    // A host-mapped Stretch clip belongs exclusively to the live runtime.  Compare
    // the public track renderer with a direct live-runtime oracle so the legacy
    // artifact path cannot be silently summed into the result a second time.
    {
        RealtimeStretchProgramRuntime renderer_runtime;
        RealtimeStretchProgramRuntime oracle_runtime;
        REQUIRE(renderer_runtime.prepare(*program, 4'800.0, 128, 1, program->audio_limits()));
        REQUIRE(oracle_runtime.prepare(*program, 4'800.0, 128, 1, program->audio_limits()));
        ArrangementAudioTrackRenderer ownership_renderer({10});
        bool heard_live_audio = false;
        const auto comparison_blocks = renderer_runtime.latency_samples() / 128 + 3;
        for (std::uint32_t index = 0; index < comparison_blocks; ++index) {
            auto state = host_mapped(static_cast<std::int64_t>(index) * 128);
            Output actual(1, 128);
            Output expected(1, 128);
            expected.view().clear();
            const auto actual_status =
                ownership_renderer.process(block, state, actual.view(), {}, &renderer_runtime);
            const auto expected_status = oracle_runtime.process_track(
                *program, *track, track->mixer(), state, expected.view());
            REQUIRE(actual_status == (index == 0 ? AudioRenderStatus::RealtimeStretchGap
                                                 : AudioRenderStatus::Rendered));
            REQUIRE(expected_status == (index == 0 ? RealtimeStretchRenderCode::GapIdentityChanged
                                                   : RealtimeStretchRenderCode::Rendered));
            for (std::size_t frame = 0; frame < actual.storage[0].size(); ++frame) {
                INFO("ownership block " << index << " frame " << frame);
                REQUIRE_THAT(actual.storage[0][frame],
                             WithinAbs(expected.storage[0][frame], 1.0e-6));
            }
            heard_live_audio = heard_live_audio ||
                               std::any_of(expected.storage[0].begin(), expected.storage[0].end(),
                                           [](float sample) { return std::abs(sample) > 1.0e-7f; });
        }
        REQUIRE(heard_live_audio);
    }

    // The aggregate byte charge is an exact admission boundary and a rejected
    // reprepare must leave the previous publication/runtime usable.
    {
        RealtimeStretchProgramRuntime measured_runtime;
        auto measured_limits = program->audio_limits();
        REQUIRE(measured_runtime.prepare(*program, 4'800.0, 128, 1, measured_limits));
        const auto exact_bytes = measured_runtime.reserved_runtime_bytes();
        REQUIRE(exact_bytes > 0);

        auto exact_limits = measured_limits;
        exact_limits.max_realtime_stretch_state_bytes = exact_bytes;
        RealtimeStretchProgramRuntime exact_runtime;
        REQUIRE(exact_runtime.prepare(*program, 4'800.0, 128, 1, exact_limits));
        REQUIRE(exact_runtime.reserved_runtime_bytes() == exact_bytes);
        const auto latency_before = exact_runtime.latency_samples();

        auto one_byte_short = exact_limits;
        --one_byte_short.max_realtime_stretch_state_bytes;
        const auto rejected = exact_runtime.prepare(*program, 4'800.0, 128, 1, one_byte_short);
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.code == RealtimeStretchStateBankError::StateBytesExceeded);
        REQUIRE(exact_runtime.reserved_runtime_bytes() == exact_bytes);
        REQUIRE(exact_runtime.latency_samples() == latency_before);
        Output after_rejection(1, 128);
        after_rejection.view().clear();
        REQUIRE(exact_runtime.process_track(*program, *track, track->mixer(), mapped,
                                            after_rejection.view()) ==
                RealtimeStretchRenderCode::GapIdentityChanged);

        RealtimeStretchProgramRuntime reference_runtime;
        REQUIRE(reference_runtime.prepare(*program, 4'800.0, 128, 1, exact_limits));
        Output reference_warmup(1, 128);
        REQUIRE(reference_runtime.process_track(*program, *track, track->mixer(), mapped,
                                                reference_warmup.view()) ==
                RealtimeStretchRenderCode::GapIdentityChanged);
        exact_runtime.force_prepare_allocation_failure_for_test();
        const auto allocation_rejected =
            exact_runtime.prepare(*program, 4'800.0, 128, 1, exact_limits);
        REQUIRE_FALSE(allocation_rejected);
        REQUIRE(allocation_rejected.code == RealtimeStretchStateBankError::AllocationFailed);
        REQUIRE(exact_runtime.reserved_runtime_bytes() == exact_bytes);
        REQUIRE(exact_runtime.latency_samples() == latency_before);
        auto next = host_mapped(128);
        Output after_allocation_failure(1, 128);
        Output reference_next(1, 128);
        REQUIRE(exact_runtime.process_track(*program, *track, track->mixer(), next,
                                            after_allocation_failure.view()) ==
                RealtimeStretchRenderCode::Rendered);
        REQUIRE(reference_runtime.process_track(*program, *track, track->mixer(), next,
                                                reference_next.view()) ==
                RealtimeStretchRenderCode::Rendered);
        REQUIRE(after_allocation_failure.storage == reference_next.storage);
    }

    // A failure after processor/FIFO/timing mutation resets the entire lane.
    // Compare its recovery with a fresh runtime so rejected-block residue cannot
    // leak through the FIFO or dry-delay rings on later callbacks.
    {
        RealtimeStretchProgramRuntime failed_runtime;
        RealtimeStretchProgramRuntime fresh_runtime;
        REQUIRE(failed_runtime.prepare(*program, 4'800.0, 128, 1, program->audio_limits()));
        REQUIRE(fresh_runtime.prepare(*program, 4'800.0, 128, 1, program->audio_limits()));
        for (std::int64_t position : {0, 128}) {
            auto state = host_mapped(position);
            Output warmup(1, 128);
            warmup.view().clear();
            REQUIRE(failed_runtime.process_track(*program, *track, track->mixer(), state,
                                                 warmup.view()) ==
                    (position == 0 ? RealtimeStretchRenderCode::GapIdentityChanged
                                   : RealtimeStretchRenderCode::Rendered));
        }
        failed_runtime.force_post_mutation_failure_for_test();
        auto rejected_state = host_mapped(256);
        Output rejected_output(1, 128);
        REQUIRE(failed_runtime.process_track(*program, *track, track->mixer(), rejected_state,
                                             rejected_output.view()) ==
                RealtimeStretchRenderCode::Underflow);
        REQUIRE(std::all_of(rejected_output.storage[0].begin(), rejected_output.storage[0].end(),
                            [](float sample) { return sample == 0.0f; }));

        for (std::uint32_t index = 0; index < 3; ++index) {
            auto recovery = host_mapped(static_cast<std::int64_t>(index) * 128);
            Output recovered(1, 128);
            Output fresh(1, 128);
            recovered.view().clear();
            fresh.view().clear();
            REQUIRE(failed_runtime.process_track(*program, *track, track->mixer(), recovery,
                                                 recovered.view()) ==
                    RealtimeStretchRenderCode::StateRequired);
            REQUIRE(fresh_runtime.process_track(*program, *track, track->mixer(), recovery,
                                                fresh.view()) ==
                    (index == 0 ? RealtimeStretchRenderCode::GapIdentityChanged
                                : RealtimeStretchRenderCode::Rendered));
            REQUIRE(std::all_of(recovered.storage[0].begin(), recovered.storage[0].end(),
                                [](float sample) { return sample == 0.0f; }));
        }
    }

    {
        RealtimeStretchProgramRuntime boundary_runtime;
        RealtimeStretchProgramRuntime untouched_runtime;
        REQUIRE(boundary_runtime.prepare(*program, 4'800.0, 128, 1, program->audio_limits()));
        REQUIRE(untouched_runtime.prepare(*program, 4'800.0, 128, 1, program->audio_limits()));
        const auto maximum_ratio = program->audio_limits().realtime_stretch_max_time_ratio;
        auto at_cap = host_mapped(0);
        at_cap.ranges[0].host_tick_end = static_cast<double>(
            program->tempo_map().fractional_samples_to_ticks(128.0L / maximum_ratio));
        REQUIRE(boundary_runtime.preflight_track(*program, *track, at_cap, live_output.view()) ==
                RealtimeStretchRenderCode::GapIdentityChanged);
        auto over_cap = at_cap;
        // Binary-search processor-ratio float representations for the first
        // request that remains observably over-cap after host-tick and tempo
        // conversions. The +0.5 value only brackets the search; the asserted
        // negative control is the adjacent representable boundary.
        const auto code_for_ratio = [&](float requested_ratio) {
            over_cap.ranges[0].host_tick_end =
                static_cast<double>(program->tempo_map().fractional_samples_to_ticks(
                    128.0L / static_cast<long double>(requested_ratio)));
            return boundary_runtime.preflight_track(*program, *track, over_cap, live_output.view());
        };
        auto passing_bits = std::bit_cast<std::uint32_t>(maximum_ratio);
        auto failing_bits = std::bit_cast<std::uint32_t>(maximum_ratio + 0.5f);
        REQUIRE(code_for_ratio(std::bit_cast<float>(failing_bits)) ==
                RealtimeStretchRenderCode::ImpossibleRatio);
        while (passing_bits + 1u < failing_bits) {
            const auto middle = passing_bits + (failing_bits - passing_bits) / 2u;
            const auto code = code_for_ratio(std::bit_cast<float>(middle));
            if (code == RealtimeStretchRenderCode::ImpossibleRatio)
                failing_bits = middle;
            else {
                REQUIRE(code == RealtimeStretchRenderCode::GapIdentityChanged);
                passing_bits = middle;
            }
        }
        REQUIRE(failing_bits == passing_bits + 1u);
        INFO("minimal projected over-cap ratio " << std::bit_cast<float>(failing_bits));
        REQUIRE(code_for_ratio(std::bit_cast<float>(failing_bits)) ==
                RealtimeStretchRenderCode::ImpossibleRatio);
        REQUIRE(over_cap.ranges[0].host_tick_end < at_cap.ranges[0].host_tick_end);
        REQUIRE(boundary_runtime.preflight_track(*program, *track, over_cap, live_output.view()) ==
                RealtimeStretchRenderCode::ImpossibleRatio);

        Output after_boundary(1, 128);
        Output untouched(1, 128);
        after_boundary.view().clear();
        untouched.view().clear();
        REQUIRE(boundary_runtime.process_track(*program, *track, track->mixer(), mapped,
                                               after_boundary.view()) ==
                RealtimeStretchRenderCode::GapIdentityChanged);
        REQUIRE(untouched_runtime.process_track(*program, *track, track->mixer(), mapped,
                                                untouched.view()) ==
                RealtimeStretchRenderCode::GapIdentityChanged);
        REQUIRE(after_boundary.storage == untouched.storage);
    }

    // Three live clips exercise the retained vector allocation boundary where
    // geometric growth would otherwise reserve four ClipLane objects while
    // reporting only three.
    {
        std::vector<Clip> clips;
        for (std::uint64_t id = 200; id < 203; ++id) {
            clips.push_back(musical_media_clip(
                id, static_cast<std::int64_t>(id - 200) * kTicksPerQuarter, kTicksPerQuarter, 3,
                fixture.decoded.audio->num_frames(), {}, TimeConform::Stretch));
        }
        auto multi_track = take(Track::create({10}, "three live Stretch clips", std::move(clips)));
        CompiledFixture multi(
            project_with_tracks(
                {std::move(multi_track)},
                {{3, "finite.wav", fixture.decoded.audio->num_frames(), {4'410, 1}}}),
            fixture.map, pool({fixture.decoded}));
        const auto multi_program = multi.store.read();
        RealtimeStretchProgramRuntime measured;
        REQUIRE(measured.prepare(*multi_program, 4'800.0, 128, 1, multi_program->audio_limits()));
        const auto exact_bytes = measured.reserved_runtime_bytes();
        REQUIRE(exact_bytes > mixed_runtime.reserved_runtime_bytes());
        auto exact_limits = multi_program->audio_limits();
        exact_limits.max_realtime_stretch_state_bytes = exact_bytes;
        RealtimeStretchProgramRuntime exact;
        REQUIRE(exact.prepare(*multi_program, 4'800.0, 128, 1, exact_limits));
        exact_limits.max_realtime_stretch_state_bytes = exact_bytes - 1u;
        RealtimeStretchProgramRuntime one_under;
        REQUIRE(one_under.prepare(*multi_program, 4'800.0, 128, 1, exact_limits).code ==
                RealtimeStretchStateBankError::StateBytesExceeded);
        // Track validation forbids overlapping clips, so the strongest
        // representable lane owns three sequential live clip states. A
        // post-mutation failure must poison/reset all of them and every shared
        // FIFO/delay ring without later residue.
        const auto* multi_track_program = multi_program->find_track({10});
        REQUIRE(multi_track_program);
        auto multi_state = snapshot(*multi_program, 128, 0);
        auto& multi_range = multi_state.ranges[0];
        multi_range.host_beat_mapping = true;
        multi_range.has_precise_host_ticks = true;
        multi_range.host_tick_start =
            static_cast<double>(multi_program->tempo_map().fractional_samples_to_ticks(0.0L));
        multi_range.host_tick_end =
            static_cast<double>(multi_program->tempo_map().fractional_samples_to_ticks(128.0L));
        measured.force_post_mutation_failure_for_test();
        Output rejected(1, 128);
        REQUIRE(measured.process_track(*multi_program, *multi_track_program,
                                       multi_track_program->mixer(), multi_state,
                                       rejected.view()) == RealtimeStretchRenderCode::Underflow);
        REQUIRE(std::all_of(rejected.storage[0].begin(), rejected.storage[0].end(),
                            [](float sample) { return sample == 0.0f; }));
        for (std::int64_t position : {128, 256}) {
            multi_state.ranges[0].timeline_sample_start = {position};
            multi_state.ranges[0].host_tick_start = static_cast<double>(
                multi_program->tempo_map().fractional_samples_to_ticks(position));
            multi_state.ranges[0].host_tick_end = static_cast<double>(
                multi_program->tempo_map().fractional_samples_to_ticks(position + 128));
            Output poisoned(1, 128);
            REQUIRE(measured.process_track(
                        *multi_program, *multi_track_program, multi_track_program->mixer(),
                        multi_state, poisoned.view()) == RealtimeStretchRenderCode::StateRequired);
            REQUIRE(std::all_of(poisoned.storage[0].begin(), poisoned.storage[0].end(),
                                [](float sample) { return sample == 0.0f; }));
        }
    }

    RealtimeStretchProgramRuntime live_runtime;
    REQUIRE(live_runtime.prepare(*program, 4'800.0, 128, 1, program->audio_limits()));
    REQUIRE(live_runtime.latency_samples() > 0);
    {
        test::RtAllocationProbe probe;
        REQUIRE(live_renderer.process(block, mapped, live_output.view(), {}, &live_runtime) ==
                AudioRenderStatus::RealtimeStretchGap);
        REQUIRE(probe.allocation_count() == 0);
    }
    auto continuous = host_mapped(128);
    {
        test::RtAllocationProbe probe;
        REQUIRE(live_renderer.process(block, continuous, live_output.view(), {}, &live_runtime) ==
                AudioRenderStatus::Rendered);
        REQUIRE(probe.allocation_count() == 0);
    }
    bool became_audible = false;
    std::int64_t live_position = 256;
    const auto blocks_to_latency = live_runtime.latency_samples() / 128 + 3;
    {
        test::RtAllocationProbe probe;
        for (std::uint32_t index = 0; index < blocks_to_latency; ++index) {
            auto live = host_mapped(live_position);
            REQUIRE(live_renderer.process(block, live, live_output.view(), {}, &live_runtime) ==
                    AudioRenderStatus::Rendered);
            became_audible =
                became_audible ||
                std::any_of(live_output.storage[0].begin(), live_output.storage[0].end(),
                            [](float sample) { return sample != 0.0f; });
            live_position += 128;
        }
        REQUIRE(probe.allocation_count() == 0);
    }
    REQUIRE(became_audible);

    auto impossible = host_mapped(live_position);
    impossible.ranges[0].host_tick_end = impossible.ranges[0].host_tick_start + 0.001;
    REQUIRE(live_runtime.preflight_track(*program, *track, impossible, live_output.view()) ==
            RealtimeStretchRenderCode::ImpossibleRatio);
    REQUIRE(live_renderer.process(block, impossible, live_output.view(), {}, &live_runtime) ==
            AudioRenderStatus::RealtimeStretchImpossibleRatio);
    REQUIRE(std::all_of(live_output.storage[0].begin(), live_output.storage[0].end(),
                        [](float sample) { return sample == 0.0f; }));

    auto next_loop_pass = host_mapped(live_position);
    next_loop_pass.range_count = 2;
    next_loop_pass.ranges[0].frame_count = 64;
    next_loop_pass.ranges[0].timeline_tick_end =
        program->tempo_map().samples_to_ticks({live_position + 64});
    next_loop_pass.ranges[0].host_tick_end =
        static_cast<double>(next_loop_pass.ranges[0].timeline_tick_end.value);
    next_loop_pass.ranges[1] = host_mapped(0).ranges[0];
    next_loop_pass.ranges[1].sample_offset = 64;
    next_loop_pass.ranges[1].frame_count = 64;
    next_loop_pass.ranges[1].timeline_tick_end = program->tempo_map().samples_to_ticks({64});
    next_loop_pass.ranges[1].host_tick_end =
        static_cast<double>(next_loop_pass.ranges[1].timeline_tick_end.value);
    next_loop_pass.ranges[1].discontinuity = true;
    next_loop_pass.ranges[1].loop_pass_index = 1;
    REQUIRE(live_renderer.process(block, next_loop_pass, live_output.view(), {}, &live_runtime) ==
            AudioRenderStatus::RealtimeStretchGap);
    REQUIRE(std::any_of(live_output.storage[0].begin(), live_output.storage[0].end(),
                        [](float sample) { return std::abs(sample) > 1.0e-7f; }));
    auto scrub = host_mapped(live_position + 128);
    scrub.scrubbing = true;
    REQUIRE(live_renderer.process(block, scrub, live_output.view(), {}, &live_runtime) ==
            AudioRenderStatus::RealtimeStretchUnsupportedScrubbing);

    // A callback boundary that cuts through one fractional document-sample
    // interval must not assign that interval to the earlier callback or shift
    // startup by one callback. Compare the identical host line rendered as
    // 128-frame blocks and as alternating 37/91-frame partitions.
    const auto render_partition = [&](bool split) {
        RealtimeStretchProgramRuntime runtime;
        REQUIRE(runtime.prepare(*program, 4'800.0, 128, 1, program->audio_limits()));
        ArrangementAudioTrackRenderer renderer({10});
        const auto total = ((runtime.latency_samples() + 1'024u + 127u) / 128u) * 128u;
        std::vector<float> rendered;
        rendered.reserve(total);
        std::uint32_t output_position = 0;
        std::uint32_t callback_index = 0;
        while (output_position < total) {
            const auto frames = split ? (callback_index % 2u == 0u ? 37u : 91u) : 128u;
            const auto document_start = 0.25L + output_position / 1.25L;
            const auto document_end = 0.25L + (output_position + frames) / 1.25L;
            TransportSnapshot state;
            state.tempo_map = &program->tempo_map();
            state.sample_rate = program->tempo_map().sample_rate();
            state.frame_count = frames;
            state.is_playing = true;
            state.range_count = 1;
            auto& range = state.ranges[0];
            range.frame_count = frames;
            range.timeline_sample_start = {static_cast<std::int64_t>(document_start)};
            range.timeline_tick_start =
                program->tempo_map().samples_to_ticks({static_cast<std::int64_t>(document_start)});
            range.timeline_tick_end =
                program->tempo_map().samples_to_ticks({static_cast<std::int64_t>(document_end)});
            range.host_beat_mapping = true;
            range.has_precise_host_ticks = true;
            range.host_tick_start = static_cast<double>(
                program->tempo_map().fractional_samples_to_ticks(document_start));
            range.host_tick_end =
                static_cast<double>(program->tempo_map().fractional_samples_to_ticks(document_end));
            Output callback(1, frames);
            const auto status = renderer.process(block, state, callback.view(), {}, &runtime);
            REQUIRE(status == (callback_index == 0 ? AudioRenderStatus::RealtimeStretchGap
                                                   : AudioRenderStatus::Rendered));
            rendered.insert(rendered.end(), callback.storage[0].begin(), callback.storage[0].end());
            output_position += frames;
            ++callback_index;
        }
        return rendered;
    };
    const auto unsplit = render_partition(false);
    const auto split = render_partition(true);
    REQUIRE(split.size() == unsplit.size());
    const auto first_audible = [](const auto& samples) {
        return static_cast<std::size_t>(
            std::find_if(samples.begin(), samples.end(),
                         [](float sample) { return std::abs(sample) > 1.0e-7f; }) -
            samples.begin());
    };
    REQUIRE(first_audible(unsplit) < unsplit.size());
    REQUIRE(first_audible(split) == first_audible(unsplit));
    for (std::size_t frame = 0; frame < split.size(); ++frame)
        REQUIRE_THAT(split[frame], WithinAbs(unsplit[frame], 1.0e-5));

    auto missing_provenance = compiled;
    missing_provenance.offline_stretch_provenance.reset();
    const auto missing_link = link_audio_track_program({10}, {missing_provenance}, {});
    REQUIRE_FALSE(missing_link);
    REQUIRE(missing_link.error().code == AudioRendererErrorCode::InvalidAsset);

    auto wrong_provenance = compiled;
    wrong_provenance.offline_stretch_provenance = std::make_shared<const OfflineStretchProvenance>(
        OfflineStretchProvenance{{999}, fixture.project->id(), 7, program->generation(), false});
    const auto wrong_link = link_audio_track_program({10}, {wrong_provenance}, {});
    REQUIRE_FALSE(wrong_link);
    REQUIRE(wrong_link.error().code == AudioRendererErrorCode::InvalidAsset);
    REQUIRE_FALSE(wrong_provenance.offline_stretch_provenance->matches(
        fixture.clip.id(), fixture.project->id(), 7, program->generation()));
    REQUIRE_FALSE(compiled.offline_stretch_provenance->matches(fixture.clip.id(), {999}, 7,
                                                               program->generation()));
    REQUIRE_FALSE(compiled.offline_stretch_provenance->matches(
        fixture.clip.id(), fixture.project->id(), 8, program->generation()));
    REQUIRE_FALSE(compiled.offline_stretch_provenance->matches(
        fixture.clip.id(), fixture.project->id(), 7, program->generation() + 1));
}
