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

} // namespace

TEST_CASE("48 kHz Stretch preserves fixed-latency waveform across mapping producer handoffs",
          "[playback][realtime-stretch]") {
    const auto fixture = stretch_fixture(48'000, 48'000, 4 * kTicksPerQuarter);
    CompiledFixture compiled(fixture.project, fixture.map, pool({fixture.decoded}));
    const auto program = compiled.store.read();
    const auto* track = program->find_track({10});
    REQUIRE(track);
    REQUIRE(track->audio_program());
    REQUIRE(track->audio_program()->clips().size() == 1);
    const auto& artifact = *track->audio_program()->clips().front().offline_stretch_artifact->audio;

    RealtimeStretchProgramRuntime runtime;
    REQUIRE(runtime.prepare(*program, 48'000.0, 128, 1, program->audio_limits()));
    const auto latency = runtime.latency_samples();
    REQUIRE(latency > 0);
    const auto round_block = [](std::uint64_t frames) { return (frames + 127u) / 128u * 128u; };
    const auto fallback_start = round_block(std::max<std::uint64_t>(latency + 512u, 6'400u));
    const auto host_restart = fallback_start + round_block(latency + 1'024u);
    const auto render_end = host_restart + round_block(latency + 1'024u);
    REQUIRE(render_end < artifact.num_frames());

    ArrangementAudioTrackRenderer renderer({10});
    PlaybackProgramBlock block(program.get());
    std::vector<float> rendered;
    rendered.reserve(render_end);
    for (std::uint64_t position = 0; position < render_end; position += 128u) {
        auto transport = snapshot(*program, 128, static_cast<std::int64_t>(position));
        const bool host_mapped = position < fallback_start || position >= host_restart;
        if (host_mapped) {
            auto& range = transport.ranges[0];
            range.host_beat_mapping = true;
            range.has_precise_host_ticks = true;
            range.host_tick_start =
                static_cast<double>(program->tempo_map().fractional_samples_to_ticks(
                    static_cast<long double>(position)));
            range.host_tick_end =
                static_cast<double>(program->tempo_map().fractional_samples_to_ticks(
                    static_cast<long double>(position + 128u)));
            if (position == 5'120u)
                range.host_tick_end =
                    std::nextafter(range.host_tick_end, std::numeric_limits<double>::infinity());
        }
        Output output(1, 128);
        const auto preflight = runtime.preflight_track(*program, *track, transport, output.view());
        const auto status = renderer.process(block, transport, output.view(), {}, &runtime);
        INFO("position " << position << " preflight " << static_cast<int>(preflight) << " status "
                         << static_cast<int>(status));
        REQUIRE((status == AudioRenderStatus::Rendered ||
                 status == AudioRenderStatus::RealtimeStretchGap));
        rendered.insert(rendered.end(), output.storage[0].begin(), output.storage[0].end());
    }

    // The fallback producer is the immutable Stage-2C artifact itself. Its
    // entire delayed interval, including the fallback->host handoff tail, is
    // bit-identical and appears at the one declared fixed latency.
    for (std::uint64_t output_frame = fallback_start + latency;
         output_frame < host_restart + latency; ++output_frame) {
        const auto document_frame = output_frame - latency;
        INFO("output frame " << output_frame << " document frame " << document_frame);
        REQUIRE(std::bit_cast<std::uint32_t>(rendered[output_frame]) ==
                std::bit_cast<std::uint32_t>(artifact.channels[0][document_frame]));
    }

    // Finalizing the prior live producer must bridge the first latency window
    // after host->fallback without a callback-sized silent hole.
    std::uint32_t consecutive_silence = 0;
    std::uint32_t maximum_silence = 0;
    for (std::uint64_t frame = fallback_start; frame < fallback_start + latency; ++frame) {
        if (std::abs(rendered[frame]) <= 1.0e-7f) {
            maximum_silence = std::max(maximum_silence, ++consecutive_silence);
        } else {
            consecutive_silence = 0;
        }
    }
    REQUIRE(maximum_silence < 128);
    REQUIRE(std::any_of(rendered.begin() + static_cast<std::ptrdiff_t>(host_restart + latency),
                        rendered.end(), [](float sample) { return std::abs(sample) > 1.0e-7f; }));
}

TEST_CASE("document-clock Stretch mixer automation follows a tempo-ramp sample oracle",
          "[playback][realtime-stretch][automation]") {
    const auto fixture = stretch_ramp_fixture(true);
    CompiledFixture compiled(fixture.project, fixture.map, pool({fixture.decoded}));
    const auto program = compiled.store.read();
    const auto* track = program->find_track({10});
    REQUIRE(track);
    REQUIRE(track->audio_program());
    const auto& artifact = *track->audio_program()->clips().front().offline_stretch_artifact->audio;

    RealtimeStretchProgramRuntime runtime;
    REQUIRE(runtime.prepare(*program, 4'800.0, 128, 1, program->audio_limits()));
    const auto latency = runtime.latency_samples();
    REQUIRE(latency > 0);
    constexpr std::uint64_t kOracleFrames = 512;
    REQUIRE(kOracleFrames < artifact.num_frames());
    const auto total = ((static_cast<std::uint64_t>(latency) + kOracleFrames + 127u) / 128u) * 128u;
    ArrangementAudioTrackRenderer renderer({10});
    PlaybackProgramBlock block(program.get());
    std::vector<float> rendered;
    rendered.reserve(total);
    for (std::uint64_t position = 0; position < total; position += 128u) {
        auto transport = snapshot(*program, 128, static_cast<std::int64_t>(position));
        Output output(1, 128);
        const auto status = renderer.process(block, transport, output.view(), {}, &runtime);
        REQUIRE(status == (position == 0 ? AudioRenderStatus::RealtimeStretchGap
                                         : AudioRenderStatus::Rendered));
        rendered.insert(rendered.end(), output.storage[0].begin(), output.storage[0].end());
    }

    bool oracle_is_audible = false;
    bool ramp_changes_gain = false;
    for (std::uint64_t document_frame = 0; document_frame < kOracleFrames; ++document_frame) {
        const auto tick = program->tempo_map().fractional_samples_to_ticks(
            static_cast<long double>(document_frame));
        const auto gain = 0.2L + 0.6L * tick / static_cast<long double>(kTicksPerQuarter);
        const auto source = artifact.channels[0][document_frame];
        const auto expected = static_cast<float>(static_cast<long double>(source) * gain);
        INFO("document frame " << document_frame << " tick " << static_cast<double>(tick));
        REQUIRE_THAT(rendered[latency + document_frame], WithinAbs(expected, 2.0e-5f));
        oracle_is_audible = oracle_is_audible || std::abs(source) > 1.0e-5f;
        ramp_changes_gain = ramp_changes_gain || gain > 0.21L;
    }
    REQUIRE(oracle_is_audible);
    REQUIRE(ramp_changes_gain);
}

TEST_CASE("incremental compilation refreshes unchanged offline Stretch provenance",
          "[playback][offline-stretch]") {
    const auto fixture = stretch_fixture();
    auto stretch_track = take(Track::create({10}, "offline stretch", {fixture.clip}));
    auto other_track = take(Track::create({11}, "independently dirty", {}));
    const auto project = project_with_tracks({std::move(stretch_track), std::move(other_track)},
                                             {{3,
                                               "finite.wav",
                                               fixture.decoded.audio->num_frames(),
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
    REQUIRE(refreshed_clip.offline_stretch_provenance->matches(fixture.clip.id(), project->id(), 2,
                                                               second->generation()));
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
