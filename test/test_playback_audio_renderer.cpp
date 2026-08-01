#include "playback_audio_renderer_test_support.hpp"

#include <random>

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

TEST_CASE("musical resample spans the authored duration while none stays native") {
    std::vector<float> ramp(120);
    for (std::size_t frame = 0; frame < ramp.size(); ++frame)
        ramp[frame] = static_cast<float>(frame);

    auto render = [&](TimeConform time_conform, bool split = false) {
        auto clip =
            musical_media_clip(100, 0, kTicksPerQuarter / 120, 3, 100, {}, time_conform, 10);
        auto track = take(Track::create({10}, "time conform", {clip}));
        auto project = project_with_tracks({track}, {{3, "ramp", ramp.size(), {48'000, 1}}});
        CompiledFixture compiled(project, map_120(), pool({{3, audio_data({ramp})}}));
        auto program = compiled.store.read();
        Output first(1, split ? 73 : 200);
        AudioRenderStatus status = AudioRenderStatus::InvalidProgram;
        std::size_t allocations = 1;
        {
            test::ScopedRtProcessProbe probe;
            status = ArrangementAudioRenderer::process(
                *program, snapshot(*program, static_cast<std::uint32_t>(first.storage[0].size())),
                first.view());
            allocations = probe.allocation_count();
        }
        REQUIRE(status == AudioRenderStatus::Rendered);
        REQUIRE(allocations == 0);
        if (!split)
            return first.storage[0];

        Output second(1, 127);
        {
            test::ScopedRtProcessProbe probe;
            status = ArrangementAudioRenderer::process(*program, snapshot(*program, 127, 73),
                                                       second.view());
            allocations = probe.allocation_count();
        }
        REQUIRE(status == AudioRenderStatus::Rendered);
        REQUIRE(allocations == 0);
        auto output = first.storage[0];
        output.insert(output.end(), second.storage[0].begin(), second.storage[0].end());
        return output;
    };

    const auto none = render(TimeConform::None);
    const auto resample = render(TimeConform::Resample);
    const auto split_resample = render(TimeConform::Resample, true);

    REQUIRE_THAT(resample[50], WithinAbs(35.0f, 1.0e-5f));
    REQUIRE_THAT(resample[100], WithinAbs(60.0f, 1.0e-5f));
    REQUIRE_THAT(resample[198], WithinAbs(109.0f, 1.0e-5f));
    REQUIRE(resample == split_resample);
    REQUIRE_THAT(none[50], WithinAbs(60.0f, 1.0e-7f));
    REQUIRE(none[100] == 0.0f);
}

TEST_CASE("musical resample follows tick phase through a document tempo ramp") {
    const std::array points{
        TempoPoint{{0}, 60.0, TempoCurve::LinearInTicks},
        TempoPoint{{2 * kTicksPerQuarter}, 180.0},
    };
    const auto map = shared_compiled_tempo_map(points, RationalRate{48'000, 1});
    const auto document_frames =
        static_cast<std::uint64_t>(map->ticks_to_samples({2 * kTicksPerQuarter}).value);
    std::vector<float> ramp(document_frames);
    for (std::size_t frame = 0; frame < ramp.size(); ++frame)
        ramp[frame] =
            static_cast<float>(static_cast<double>(frame) / static_cast<double>(document_frames));

    auto render = [&](TimeConform time_conform) {
        auto clip =
            musical_media_clip(100, 0, 2 * kTicksPerQuarter, 3, document_frames, {}, time_conform);
        auto track = take(Track::create({10}, "tempo ramp conform", {clip}));
        auto project =
            project_with_tracks({track}, {{3, "tempo-ramp.wav", document_frames, {48'000, 1}}});
        CompiledFixture compiled(project, map, pool({{3, audio_data({ramp})}}));
        auto program = compiled.store.read();
        Output output(1, 1);
        const auto midpoint_sample = map->ticks_to_samples({kTicksPerQuarter}).value;
        REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 1, midpoint_sample),
                                                  output.view()) == AudioRenderStatus::Rendered);
        return output.storage[0][0];
    };

    const auto native_midpoint = render(TimeConform::None);
    const auto resampled_midpoint = render(TimeConform::Resample);
    REQUIRE_THAT(resampled_midpoint, WithinAbs(0.5f, 1.0e-4f));
    REQUIRE(std::abs(native_midpoint - 0.5f) > 0.05f);
}

TEST_CASE("musical resample is block-split and seek exact through a tempo ramp") {
    const std::array points{
        TempoPoint{{0}, 60.0, TempoCurve::LinearInTicks},
        TempoPoint{{2 * kTicksPerQuarter}, 180.0},
    };
    const auto map = shared_compiled_tempo_map(points, RationalRate{48'000, 1});
    const auto frames =
        static_cast<std::uint32_t>(map->ticks_to_samples({2 * kTicksPerQuarter}).value);
    std::vector<float> source(frames);
    for (std::size_t index = 0; index < source.size(); ++index)
        source[index] = static_cast<float>(index % 251u) / 251.0f;
    auto clip = musical_media_clip(100, 0, 2 * kTicksPerQuarter, 3, source.size(), {},
                                   TimeConform::Resample);
    auto track = take(Track::create({10}, "ramp split", {clip}));
    auto project = project_with_tracks({track}, {{3, "pattern", source.size(), {48'000, 1}}});
    CompiledFixture compiled(project, map, pool({{3, audio_data({source})}}));
    auto program = compiled.store.read();

    Output whole(1, frames);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, frames), whole.view()) ==
            AudioRenderStatus::Rendered);
    const auto split_frame = frames / 3u;
    Output first(1, split_frame);
    Output second(1, frames - split_frame);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, split_frame),
                                              first.view()) == AudioRenderStatus::Rendered);
    REQUIRE(ArrangementAudioRenderer::process(
                *program, snapshot(*program, frames - split_frame, split_frame), second.view()) ==
            AudioRenderStatus::Rendered);
    std::vector<float> split = first.storage[0];
    split.insert(split.end(), second.storage[0].begin(), second.storage[0].end());
    REQUIRE(split == whole.storage[0]);

    constexpr std::uint32_t seek_frames = 31;
    const auto seek_start = frames / 2u;
    Output seeked(1, seek_frames);
    REQUIRE(ArrangementAudioRenderer::process(
                *program, snapshot(*program, seek_frames, seek_start), seeked.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(std::equal(seeked.storage[0].begin(), seeked.storage[0].end(),
                       whole.storage[0].begin() + seek_start));
}

TEST_CASE("musical resample compilation admits the converter ceiling and rejects above it") {
    constexpr auto boundary_frames = static_cast<std::uint64_t>(
        audio::PreparedVariableRateConversion::kMaximumSourceFramesPerOutputFrame);
    std::vector<float> source(boundary_frames + 1u, 0.25f);
    const std::array points{TempoPoint{{0}, 60.0}};
    const auto map = shared_compiled_tempo_map(points, RationalRate{1, 1});
    auto boundary = musical_media_clip(100, 0, kTicksPerQuarter, 3, boundary_frames, {},
                                       TimeConform::Resample);
    auto track = take(Track::create({10}, "converter boundary", {boundary}));
    auto project = project_with_tracks(
        {track}, {{3, "constant", source.size(), {1, 1}}});
    const auto assets = pool({{3, audio_data({source}, 1)}});

    REQUIRE(map->ticks_to_samples(boundary.start()).value == 0);
    REQUIRE(map->ticks_to_samples(boundary.end()).value == 1);
    REQUIRE(compile_audio_clip_program(boundary, *project, *map, *assets, {}));

    auto above = musical_media_clip(101, 0, kTicksPerQuarter, 3, boundary_frames + 1u, {},
                                    TimeConform::Resample);
    const auto rejected = compile_audio_clip_program(above, *project, *map, *assets, {});
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == AudioRendererErrorCode::UnsupportedSampleRate);
}

TEST_CASE("musical resample compilation scans interior tempo ramp peaks") {
    constexpr std::uint64_t source_frames = 1'800'000;
    constexpr std::int64_t duration_ticks = 12'000;
    std::vector<float> source(source_frames, 0.25f);
    auto clip = musical_media_clip(100, 0, duration_ticks, 3, source_frames, {},
                                   TimeConform::Resample);
    auto track = take(Track::create({10}, "ramp ceiling", {clip}));
    auto project = project_with_tracks({track}, {{3, "constant", source_frames, {1'600, 1}}});
    const auto assets = pool({{3, audio_data({source}, 1'600)}});
    const std::array low_points{TempoPoint{{0}, 32.0}, TempoPoint{{duration_ticks}, 32.0}};
    const auto low = shared_compiled_tempo_map(low_points, RationalRate{1'600, 1});
    REQUIRE(compile_audio_clip_program(clip, *project, *low, *assets, {}));

    const std::array peak_points{
        TempoPoint{{0}, 32.0, TempoCurve::LinearInTicks},
        TempoPoint{{duration_ticks / 2}, 1'000.0, TempoCurve::LinearInTicks},
        TempoPoint{{duration_ticks}, 32.0},
    };
    const auto peak = shared_compiled_tempo_map(peak_points, RationalRate{1'600, 1});
    const auto rejected = compile_audio_clip_program(clip, *project, *peak, *assets, {});
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == AudioRendererErrorCode::UnsupportedSampleRate);
}

TEST_CASE("audio renderer maps host 60 BPM frames into a 120 BPM document") {
    constexpr auto mapped_duration = kTicksPerQuarter / 480;
    constexpr auto mapped_half = mapped_duration / 2;
    std::vector<float> ramp(50);
    for (std::size_t frame = 0; frame < ramp.size(); ++frame)
        ramp[frame] = static_cast<float>(frame + 1);
    const auto data = audio_data({ramp});
    auto clip = musical_media_clip(
        100, kTicksPerQuarter, mapped_duration, 3, ramp.size(),
        {.gain_linear = 1.0f, .fade_in_duration = mapped_half, .fade_out_duration = 0});
    auto track = take(Track::create({10}, "host mapped", {clip}));
    auto project = project_with_tracks({track}, {{3, "ramp", ramp.size(), {48'000, 1}}});
    const auto map = map_120();
    const auto assets = pool({{3, data}});
    CompiledFixture compiled(project, map, assets);
    auto program = compiled.store.read();

    auto mapped = snapshot(*program, 100, 48'000);
    mapped.ranges[0].timeline_tick_start = {kTicksPerQuarter};
    mapped.ranges[0].timeline_tick_end = {kTicksPerQuarter + mapped_duration};
    mapped.ranges[0].host_beat_mapping = true;
    Output stretched(1, 100);
    AudioRenderStatus status = AudioRenderStatus::InvalidProgram;
    std::size_t allocations = 1;
    {
        test::ScopedRtProcessProbe probe;
        status = ArrangementAudioRenderer::process(*program, mapped, stretched.view());
        allocations = probe.allocation_count();
    }
    REQUIRE(status == AudioRenderStatus::Rendered);
    REQUIRE(allocations == 0);
    REQUIRE_THAT(stretched.storage[0][0], WithinAbs(0.0f, 1e-7f));
    REQUIRE_THAT(stretched.storage[0][25], WithinAbs(6.75f, 1e-6f));
    REQUIRE_THAT(stretched.storage[0][50], WithinAbs(26.0f, 1e-6f));
    REQUIRE_THAT(stretched.storage[0][99], WithinAbs(50.0f, 1e-6f));

    mapped.range_count = 2;
    mapped.ranges[0].frame_count = 50;
    mapped.ranges[0].timeline_tick_end = {kTicksPerQuarter + mapped_half};
    mapped.ranges[1] = mapped.ranges[0];
    mapped.ranges[1].sample_offset = 50;
    mapped.ranges[1].discontinuity = true;
    Output looped(1, 100);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, looped.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE_THAT(looped.storage[0][49], WithinAbs(24.99f, 1e-5f));
    REQUIRE_THAT(looped.storage[0][50], WithinAbs(0.0f, 1e-7f));
    REQUIRE_THAT(looped.storage[0][75], WithinAbs(6.75f, 1e-6f));
}

TEST_CASE("host-mapped audio keeps sub-sample musical boundaries and source origin exact") {
    constexpr std::int64_t clip_start_tick = 10;
    constexpr std::int64_t clip_end_tick = 40;
    std::vector<float> ramp(16);
    for (std::size_t frame = 0; frame < ramp.size(); ++frame)
        ramp[frame] = static_cast<float>(frame + 1);
    auto clip =
        musical_media_clip(100, clip_start_tick, clip_end_tick - clip_start_tick, 3, ramp.size());
    auto track = take(Track::create({10}, "exact musical bounds", {clip}));
    auto project = project_with_tracks({track}, {{3, "ramp", ramp.size(), {48'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, audio_data({ramp})}}));
    auto program = compiled.store.read();
    const auto& program_clip = program->find_track({10})->audio_program()->clips().front();
    REQUIRE(program_clip.musical_tick_start == TickPosition{clip_start_tick});
    REQUIRE(program_clip.musical_tick_end == TickPosition{clip_end_tick});

    auto mapped = snapshot(*program, 50);
    mapped.ranges[0].timeline_tick_start = {0};
    mapped.ranges[0].timeline_tick_end = {50};
    mapped.ranges[0].host_beat_mapping = true;
    Output output(1, 50);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, output.view()) ==
            AudioRenderStatus::Rendered);

    REQUIRE(output.storage[0][clip_start_tick - 1] == 0.0f);
    REQUIRE_THAT(output.storage[0][clip_start_tick], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE(output.storage[0][clip_end_tick - 1] != 0.0f);
    REQUIRE(output.storage[0][clip_end_tick] == 0.0f);
}

TEST_CASE("host-mapped musical resample keeps fractional clip tails audible") {
    constexpr std::int64_t clip_start_tick = 5;
    constexpr std::int64_t clip_duration_ticks = 70;
    std::vector<float> source(70, 1.0f);
    auto clip = musical_media_clip(100, clip_start_tick, clip_duration_ticks, 3, source.size(), {},
                                   TimeConform::Resample);
    auto track = take(Track::create({10}, "fractional conform tail", {clip}));
    auto project = project_with_tracks({track}, {{3, "constant", source.size(), {48'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, audio_data({source})}}));
    auto program = compiled.store.read();

    auto mapped = snapshot(*program, 80);
    mapped.ranges[0].timeline_tick_start = {0};
    mapped.ranges[0].timeline_tick_end = {80};
    mapped.ranges[0].host_beat_mapping = true;
    Output output(1, 80);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, output.view()) ==
            AudioRenderStatus::Rendered);

    REQUIRE(output.storage[0][clip_start_tick - 1] == 0.0f);
    REQUIRE(output.storage[0][clip_start_tick] != 0.0f);
    REQUIRE(output.storage[0][clip_start_tick + clip_duration_ticks - 1] != 0.0f);
    REQUIRE(output.storage[0][clip_start_tick + clip_duration_ticks] == 0.0f);
}

TEST_CASE("precise fractional host ticks select neighboring resample phases") {
    std::vector<float> source(100);
    for (std::size_t index = 0; index < source.size(); ++index)
        source[index] = static_cast<float>(index);
    auto clip = musical_media_clip(100, 0, 100, 3, source.size(), {}, TimeConform::Resample);
    auto track = take(Track::create({10}, "fractional host phase", {clip}));
    auto project = project_with_tracks({track}, {{3, "ramp", source.size(), {48'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, audio_data({source})}}));
    auto program = compiled.store.read();

    auto render_at = [&](double tick) {
        auto mapped = snapshot(*program, 1);
        mapped.ranges[0].host_beat_mapping = true;
        mapped.ranges[0].has_precise_host_ticks = true;
        mapped.ranges[0].host_tick_start = tick;
        mapped.ranges[0].host_tick_end = tick + 0.5;
        Output output(1, 1);
        REQUIRE(ArrangementAudioRenderer::process(*program, mapped, output.view()) ==
                AudioRenderStatus::Rendered);
        return output.storage[0][0];
    };

    const auto lower = render_at(10.0);
    const auto fractional = render_at(10.25);
    const auto upper = render_at(11.0);
    REQUIRE_THAT(fractional, WithinAbs(10.25f, 1.0e-4f));
    REQUIRE(fractional > lower);
    REQUIRE(fractional < upper);
}

TEST_CASE("host-mapped audio follows a document tempo ramp instead of endpoint interpolation") {
    const std::array points{
        TempoPoint{{0}, 60.0, TempoCurve::LinearInTicks},
        TempoPoint{{2 * kTicksPerQuarter}, 180.0},
    };
    const auto map = shared_compiled_tempo_map(points, RationalRate{48'000, 1});
    const auto document_frames =
        static_cast<std::uint64_t>(map->ticks_to_samples({2 * kTicksPerQuarter}).value);
    REQUIRE(document_frames > 0);
    std::vector<float> ramp(document_frames);
    for (std::size_t frame = 0; frame < ramp.size(); ++frame)
        ramp[frame] =
            static_cast<float>(static_cast<double>(frame) / static_cast<double>(document_frames));

    const auto data = audio_data({ramp});
    auto clip = musical_media_clip(100, 0, 2 * kTicksPerQuarter, 3, document_frames);
    auto track = take(Track::create({10}, "tempo ramp", {clip}));
    auto project =
        project_with_tracks({track}, {{3, "tempo-ramp.wav", document_frames, {48'000, 1}}});
    CompiledFixture compiled(project, map, pool({{3, data}}));
    auto program = compiled.store.read();

    auto mapped = snapshot(*program, 4);
    mapped.ranges[0].timeline_tick_start = {0};
    mapped.ranges[0].timeline_tick_end = {2 * kTicksPerQuarter};
    mapped.ranges[0].host_beat_mapping = true;
    Output output(1, 4);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, output.view()) ==
            AudioRenderStatus::Rendered);

    const auto midpoint_sample = map->ticks_to_samples({kTicksPerQuarter}).value;
    const auto expected = static_cast<float>(static_cast<double>(midpoint_sample) /
                                             static_cast<double>(document_frames));
    REQUIRE_THAT(output.storage[0][2], WithinAbs(expected, 0.01f));
    REQUIRE(std::abs(output.storage[0][2] - 0.5f) > 0.05f);
}

TEST_CASE("host-mapped musical resample uses host beat phase instead of document samples") {
    const std::array points{
        TempoPoint{{0}, 60.0, TempoCurve::LinearInTicks},
        TempoPoint{{2 * kTicksPerQuarter}, 180.0},
    };
    const auto map = shared_compiled_tempo_map(points, RationalRate{48'000, 1});
    const auto document_frames =
        static_cast<std::uint64_t>(map->ticks_to_samples({2 * kTicksPerQuarter}).value);
    std::vector<float> ramp(document_frames);
    for (std::size_t frame = 0; frame < ramp.size(); ++frame)
        ramp[frame] =
            static_cast<float>(static_cast<double>(frame) / static_cast<double>(document_frames));

    auto clip = musical_media_clip(100, 0, 2 * kTicksPerQuarter, 3, document_frames, {},
                                   TimeConform::Resample);
    auto track = take(Track::create({10}, "host phase conform", {clip}));
    auto project =
        project_with_tracks({track}, {{3, "tempo-ramp.wav", document_frames, {48'000, 1}}});
    CompiledFixture compiled(project, map, pool({{3, audio_data({ramp})}}));
    auto program = compiled.store.read();

    auto mapped = snapshot(*program, 4);
    mapped.ranges[0].timeline_tick_start = {0};
    mapped.ranges[0].timeline_tick_end = {2 * kTicksPerQuarter};
    mapped.ranges[0].host_beat_mapping = true;
    Output output(1, 4);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, output.view()) ==
            AudioRenderStatus::Rendered);

    REQUIRE_THAT(output.storage[0][2], WithinAbs(0.5f, 0.01f));
    const auto native_sample_phase =
        static_cast<float>(static_cast<double>(map->ticks_to_samples({kTicksPerQuarter}).value) /
                           static_cast<double>(document_frames));
    REQUIRE(std::abs(output.storage[0][2] - native_sample_phase) > 0.05f);

    mapped.range_count = 2;
    mapped.ranges[0].frame_count = 2;
    mapped.ranges[0].timeline_tick_end = {kTicksPerQuarter};
    mapped.ranges[1] = mapped.ranges[0];
    mapped.ranges[1].sample_offset = 2;
    mapped.ranges[1].timeline_tick_start = {kTicksPerQuarter};
    mapped.ranges[1].timeline_tick_end = {2 * kTicksPerQuarter};
    mapped.ranges[1].discontinuity = true;
    Output split(1, 4);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, split.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(split.storage[0] == output.storage[0]);
}

TEST_CASE("host musical resample preflights converter limits without touching output") {
    constexpr auto maximum_step = static_cast<std::uint64_t>(
        audio::PreparedVariableRateConversion::kMaximumSourceFramesPerOutputFrame);
    constexpr auto source_frames = maximum_step + 1u;
    std::vector<float> source(source_frames, 0.25f);
    auto clip = musical_media_clip(100, 0, static_cast<std::int64_t>(source_frames), 3,
                                   source_frames, {}, TimeConform::Resample);
    auto safe_clip = musical_media_clip(99, 0, static_cast<std::int64_t>(source_frames), 3, 1, {},
                                        TimeConform::Resample);
    auto safe_track = take(Track::create({9}, "safe earlier track", {safe_clip}));
    auto track = take(Track::create({10}, "bounded host phase", {clip}));
    auto project =
        project_with_tracks({safe_track, track}, {{3, "constant", source_frames, {96'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, audio_data({source}, 96'000)}}));
    auto program = compiled.store.read();

    auto mapped = snapshot(*program, 1);
    mapped.ranges[0].host_beat_mapping = true;
    mapped.ranges[0].has_precise_host_ticks = true;
    mapped.ranges[0].host_tick_start = 0.0;
    mapped.ranges[0].host_tick_end = static_cast<double>(maximum_step);
    Output exact(1, 1);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, exact.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE_THAT(exact.storage[0][0], WithinAbs(0.5f, 1.0e-6f));

    mapped.ranges[0].host_tick_end = static_cast<double>(maximum_step) + 0.5;
    Output rejected(1, 1);
    AudioRenderStatus status = AudioRenderStatus::Rendered;
    std::size_t allocations = 1;
    {
        test::ScopedRtProcessProbe probe;
        status = ArrangementAudioRenderer::process(*program, mapped, rejected.view());
        allocations = probe.allocation_count();
    }
    REQUIRE(status == AudioRenderStatus::InvalidTransport);
    REQUIRE(allocations == 0);
    REQUIRE(rejected.storage[0][0] == 99.0f);

    mapped.ranges[0].host_tick_start = 0.25;
    mapped.ranges[0].host_tick_end = 0.25;
    Output held(1, 1);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, held.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE_THAT(held.storage[0][0], WithinAbs(0.5f, 1.0e-6f));

    mapped.ranges[0].host_tick_start = static_cast<double>(source_frames - 1u);
    mapped.ranges[0].host_tick_end = static_cast<double>(source_frames) + 1.0e9;
    Output clamped_edge(1, 1);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, clamped_edge.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE_THAT(clamped_edge.storage[0][0], WithinAbs(0.5f, 1.0e-6f));

    mapped.ranges[0].has_precise_host_ticks = false;
    mapped.ranges[0].timeline_tick_start = {0};
    mapped.ranges[0].timeline_tick_end = {static_cast<std::int64_t>(maximum_step)};
    Output integer_endpoint(1, 1);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, integer_endpoint.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(integer_endpoint.storage[0] == exact.storage[0]);

    auto split = snapshot(*program, 2);
    split.range_count = 2;
    split.ranges[0] = mapped.ranges[0];
    split.ranges[0].frame_count = 1;
    split.ranges[0].sample_offset = 0;
    split.ranges[1] = split.ranges[0];
    split.ranges[1].sample_offset = 1;
    split.ranges[1].timeline_sample_start = {1};
    split.ranges[1].discontinuity = true;
    split.ranges[1].has_precise_host_ticks = true;
    split.ranges[1].host_tick_start = 0.0;
    split.ranges[1].host_tick_end = static_cast<double>(maximum_step) + 0.5;
    Output second_range_rejected(1, 2);
    REQUIRE(ArrangementAudioRenderer::process(*program, split, second_range_rejected.view()) ==
            AudioRenderStatus::InvalidTransport);
    REQUIRE(second_range_rejected.storage[0] == std::vector<float>{99.0f, 99.0f});
}

TEST_CASE("host native-rate mapping enforces the converter step ceiling atomically") {
    constexpr auto maximum_step = static_cast<std::uint64_t>(
        audio::PreparedVariableRateConversion::kMaximumSourceFramesPerOutputFrame);
    constexpr std::int64_t ticks_per_source_frame = 5'880;
    std::vector<float> source(16, 0.25f);
    auto clip = musical_media_clip(100, 0, 20'000, 3, source.size());
    auto track = take(Track::create({10}, "native host ceiling", {clip}));
    auto project = project_with_tracks({track}, {{3, "constant", source.size(), {2, 1}}});
    const std::array points{TempoPoint{{0}, 1.0}};
    const auto map = shared_compiled_tempo_map(points, RationalRate{1, 1});
    CompiledFixture compiled(project, map, pool({{3, audio_data({source}, 2)}}));
    auto program = compiled.store.read();

    auto mapped = snapshot(*program, 1);
    mapped.ranges[0].host_beat_mapping = true;
    mapped.ranges[0].has_precise_host_ticks = true;
    mapped.ranges[0].host_tick_start = 0.0;
    mapped.ranges[0].host_tick_end =
        static_cast<double>(maximum_step * ticks_per_source_frame);
    Output exact(1, 1);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, exact.view()) ==
            AudioRenderStatus::Rendered);

    mapped.ranges[0].host_tick_end =
        static_cast<double>(maximum_step * ticks_per_source_frame + ticks_per_source_frame / 2);
    Output rejected(1, 1);
    AudioRenderStatus status = AudioRenderStatus::Rendered;
    std::size_t allocations = 1;
    {
        test::ScopedRtProcessProbe probe;
        status = ArrangementAudioRenderer::process(*program, mapped, rejected.view());
        allocations = probe.allocation_count();
    }
    REQUIRE(status == AudioRenderStatus::InvalidTransport);
    REQUIRE(allocations == 0);
    REQUIRE(rejected.storage[0][0] == 99.0f);
}

TEST_CASE("host beat mapping keeps absolute audio on the host sample clock") {
    std::vector<float> absolute_ramp(64);
    for (std::size_t frame = 0; frame < absolute_ramp.size(); ++frame)
        absolute_ramp[frame] = static_cast<float>(frame);
    auto absolute = absolute_media_clip(100, 0, 50, 3, 0, 50);
    auto absolute_track = take(Track::create({10}, "absolute", {absolute}));
    auto project = project_with_tracks({absolute_track}, {{3, "absolute", 64, {48'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, audio_data({absolute_ramp})}}));
    auto program = compiled.store.read();

    auto mapped = snapshot(*program, 50);
    mapped.ranges[0].timeline_tick_start = {0};
    mapped.ranges[0].timeline_tick_end = {kTicksPerQuarter / 960};
    mapped.ranges[0].host_beat_mapping = true;
    Output output(1, 50);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, output.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE_THAT(output.storage[0][10], WithinAbs(10.0f, 1.0e-6f));
    REQUIRE_THAT(output.storage[0][20], WithinAbs(20.0f, 1.0e-6f));
}

TEST_CASE("host-tempo audio decimation rejects aliases without muting the passband path") {
    std::vector<float> source(256);
    for (std::size_t frame = 0; frame < source.size(); ++frame)
        source[frame] = frame % 2 == 0 ? 1.0f : -1.0f;
    auto clip = musical_media_clip(100, 0, kTicksPerQuarter / 120, 3, 200);
    auto track = take(Track::create({10}, "host decimation", {clip}));
    auto project = project_with_tracks({track}, {{3, "nyquist", source.size(), {48'000, 1}}});
    const auto map = map_120();
    const auto assets = pool({{3, audio_data({source})}});
    CompiledFixture compiled(project, map, assets);
    auto program = compiled.store.read();
    REQUIRE(program->find_track({10})->audio_program()->clips()[0].uses_host_rate_conversion());

    auto render = [&](std::int64_t tick_end) {
        auto mapped = snapshot(*program, 100);
        mapped.ranges[0].timeline_tick_start = {0};
        mapped.ranges[0].timeline_tick_end = {tick_end};
        mapped.ranges[0].host_beat_mapping = true;
        Output output(1, 100);
        REQUIRE(ArrangementAudioRenderer::process(*program, mapped, output.view()) ==
                AudioRenderStatus::Rendered);
        double squared = 0.0;
        for (std::size_t frame = 32; frame < 68; ++frame)
            squared += static_cast<double>(output.storage[0][frame]) *
                       static_cast<double>(output.storage[0][frame]);
        return std::sqrt(squared / 36.0);
    };

    const auto passband_reference_rms = render(kTicksPerQuarter / 240);
    const auto decimated_alias_rms = render(kTicksPerQuarter / 120);
    CAPTURE(passband_reference_rms, decimated_alias_rms);
    REQUIRE(passband_reference_rms >= 0.8);
    REQUIRE(decimated_alias_rms <= 0.05);
}

TEST_CASE("host-tempo conversion clamps reconstruction to the referenced media range") {
    std::vector<float> source(192, 1.0f);
    std::fill(source.begin() + 64, source.begin() + 128, 0.0f);
    auto clip = take(Clip::create({100}, {0}, {kTicksPerQuarter / 120}, MediaRef{{3}, {64}, 64}));
    auto track = take(Track::create({10}, "range isolation", {clip}));
    auto project = project_with_tracks({track}, {{3, "guarded", source.size(), {48'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, audio_data({source})}}));
    const auto& program_clip =
        compiled.store.read()->find_track({10})->audio_program()->clips().front();
    REQUIRE(program_clip.uses_host_rate_conversion());
    auto program = compiled.store.read();
    Output output(1, 64);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 64), output.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(output.storage[0] == std::vector<float>(64, 0.0f));
}

TEST_CASE("host-tempo reconstruction uses the effective rate when slowed audio stops decimating") {
    std::vector<float> source(256);
    for (std::size_t frame = 0; frame < source.size(); ++frame)
        source[frame] = frame % 2 == 0 ? 1.0f : -1.0f;
    auto clip = musical_media_clip(100, 0, kTicksPerQuarter / 240, 3, 200);
    auto track = take(Track::create({10}, "slowed downsample", {clip}));
    auto project = project_with_tracks({track}, {{3, "96k-nyquist", source.size(), {96'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, audio_data({source}, 96'000)}}));
    auto program = compiled.store.read();

    auto mapped = snapshot(*program, 100);
    mapped.ranges[0].timeline_tick_start = {0};
    mapped.ranges[0].timeline_tick_end = {kTicksPerQuarter / 480};
    mapped.ranges[0].host_beat_mapping = true;
    Output output(1, 100);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, output.view()) ==
            AudioRenderStatus::Rendered);
    double squared = 0.0;
    for (std::size_t frame = 16; frame < 84; ++frame)
        squared += static_cast<double>(output.storage[0][frame]) *
                   static_cast<double>(output.storage[0][frame]);
    REQUIRE(std::sqrt(squared / 68.0) >= 0.8);
}

TEST_CASE("one bounded compiler pass publishes note and audio payloads for a mixed track") {
    const auto map = map_120();
    auto notes = take(MidiContent::create({{{101}, {0}, {kTicksPerQuarter / 4}, 0x8000, 64, 1}}));
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

TEST_CASE("active take comp lowers to a derived artifact and renders exact selections") {
    std::vector<float> source(32);
    for (std::size_t index = 0; index < source.size(); ++index)
        source[index] = static_cast<float>(index);
    const auto first =
        take(Take::create({20}, MediaRef{{3}, {0}, 16}, {0}, RationalRate{48'000, 1}));
    const auto second =
        take(Take::create({21}, MediaRef{{3}, {16}, 16}, {0}, RationalRate{48'000, 1}));
    auto lane = take(TakeLane::create({30}, "active", {first, second},
                                      {{.take_id = {20}, .range = {{0}, 2, {48'000, 1}}},
                                       {.take_id = {21}, .range = {{2}, 2, {48'000, 1}}},
                                       {.take_id = {20}, .range = {{4}, 2, {48'000, 1}}}}));
    auto track = take(Track::create(TrackInput{.id = {10},
                                               .name = "comp",
                                               .clips = {absolute_media_clip(100, 0, 6, 3, 0, 6)},
                                               .take_lanes = {lane},
                                               .active_take_lane_id = {30}}));
    auto project = project_with_tracks({track}, {{3, "takes", 32, {48'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, audio_data({source})}}));
    auto program = compiled.store.read();
    const auto* compiled_track = program->find_track({10});
    REQUIRE(compiled_track->expanded_clip_count() == 3);
    const auto regions = compiled_track->audio_program()->clips();
    REQUIRE(compiled_track->ordered_clip_ids().empty());
    REQUIRE(regions.size() == 3);
    REQUIRE(regions[0].source_kind == AudioClipRendererProgram::SourceKind::TakeCompSegment);
    REQUIRE(regions[0].source_ordinal == 1);
    REQUIRE(regions[1].source_ordinal == 2);
    REQUIRE(regions[2].source_ordinal == 3);
    REQUIRE(regions[0].id == ItemId{20});
    REQUIRE(regions[1].id == ItemId{21});
    REQUIRE(regions[2].id == ItemId{20});

    Output output(1, 6);
    AudioRenderStatus status = AudioRenderStatus::InvalidProgram;
    std::size_t allocations = 1;
    {
        test::ScopedRtProcessProbe probe;
        status = ArrangementAudioRenderer::process(*program, snapshot(*program, 6), output.view());
        allocations = probe.allocation_count();
    }
    REQUIRE(status == AudioRenderStatus::Rendered);
    REQUIRE(allocations == 0);
    REQUIRE(output.storage[0] == std::vector<float>{0, 1, 18, 19, 4, 5});

    auto mapped = snapshot(*program, 6);
    mapped.ranges[0].timeline_tick_start = {0};
    mapped.ranges[0].timeline_tick_end = {kTicksPerQuarter / 8'000};
    mapped.ranges[0].host_beat_mapping = true;
    Output host_tempo_output(1, 6);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, host_tempo_output.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(host_tempo_output.storage[0] == std::vector<float>{0, 1, 18, 19, 4, 5});
}

TEST_CASE("inactive take comp is document data but not a playback source") {
    const auto recorded =
        take(Take::create({20}, MediaRef{{3}, {0}, 4}, {0}, RationalRate{48'000, 1}));
    auto lane = take(TakeLane::create({30}, "inactive", {recorded},
                                      {{.take_id = {20}, .range = {{0}, 4, {48'000, 1}}}}));
    auto track = take(Track::create(TrackInput{.id = {10},
                                               .name = "comp",
                                               .clips = {absolute_media_clip(100, 0, 4, 3, 0, 4)},
                                               .take_lanes = {lane}}));
    auto project = project_with_tracks({track}, {{3, "take", 4, {48'000, 1}}});
    CompiledFixture compiled(project, map_120(),
                             pool({{3, audio_data({{1.0f, 2.0f, 3.0f, 4.0f}})}}));
    const auto* compiled_track = compiled.store.read()->find_track({10});
    REQUIRE(compiled_track->ordered_clip_ids().size() == 1);
    REQUIRE(compiled_track->ordered_clip_ids()[0] == ItemId{100});
    REQUIRE(compiled_track->audio_program()->clips()[0].source_kind ==
            AudioClipRendererProgram::SourceKind::ArrangementClip);
}

TEST_CASE("take comp compilation validates asset rate and whole program capacity") {
    const auto recorded =
        take(Take::create({20}, MediaRef{{3}, {0}, 4}, {0}, RationalRate{44'100, 1}));
    auto lane = take(TakeLane::create({30}, "rate", {recorded},
                                      {{.take_id = {20}, .range = {{0}, 4, {44'100, 1}}}}));
    auto track = take(Track::create(
        TrackInput{.id = {10}, .name = "comp", .take_lanes = {lane}, .active_take_lane_id = {30}}));
    auto project = project_with_tracks({track}, {{3, "take", 4, {48'000, 1}}});
    auto compiled = compile_take_comp_segment_program(
        lane, 0, *project, *map_120(), *pool({{3, audio_data({{1.0f, 2.0f, 3.0f, 4.0f}})}}), {});
    REQUIRE_FALSE(compiled);
    REQUIRE(compiled.error().code == AudioRendererErrorCode::UnsupportedSampleRate);

    const auto matching =
        take(Take::create({20}, MediaRef{{3}, {0}, 4}, {0}, RationalRate{48'000, 1}));
    lane = take(TakeLane::create({30}, "capacity", {matching},
                                 {{.take_id = {20}, .range = {{0}, 2, {48'000, 1}}},
                                  {.take_id = {20}, .range = {{2}, 2, {48'000, 1}}}}));
    track = take(Track::create(
        TrackInput{.id = {10}, .name = "comp", .take_lanes = {lane}, .active_take_lane_id = {30}}));
    auto earlier = take(Track::create({9}, "earlier", {absolute_media_clip(100, 0, 1, 3, 0, 1)}));
    project = project_with_tracks({earlier, track}, {{3, "take", 4, {48'000, 1}}});
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = project;
    request.sequence_id = {2};
    request.tempo_map = map_120();
    request.document_revision = 1;
    request.dirty.all = true;
    request.audio_assets = pool({{3, audio_data({{1.0f, 2.0f, 3.0f, 4.0f}})}});
    request.audio_limits.max_clips = 2;
    REQUIRE(compiler.submit(std::move(request)));
    REQUIRE_FALSE(store.has_value());
    REQUIRE(compiler.status().last_error.code == CompileErrorCode::AudioProgramInvalid);
    REQUIRE(compiler.status().last_error.audio_detail == AudioRendererErrorCode::CapacityExceeded);
    REQUIRE(compiler.status().last_error.item == ItemId{30});
}

TEST_CASE("take comp boundary projection is exact beyond floating point integer precision") {
    constexpr std::int64_t start = (std::int64_t{1} << 53u) + 1;
    const auto recorded =
        take(Take::create({20}, MediaRef{{3}, {0}, 1}, {start}, RationalRate{48'000, 1}));
    auto lane = take(TakeLane::create({30}, "large position", {recorded},
                                      {{.take_id = {20}, .range = {{start}, 1, {48'000, 1}}}}));
    auto track = take(Track::create(
        TrackInput{.id = {10}, .name = "comp", .take_lanes = {lane}, .active_take_lane_id = {30}}));
    auto project = project_with_tracks({track}, {{3, "take", 1, {48'000, 1}}});
    const std::array points{TempoPoint{{0}, 120.0}};
    auto doubled_rate = shared_compiled_tempo_map(points, RationalRate{96'000, 1});
    auto compiled = take(compile_take_comp_segment_program(lane, 0, *project, *doubled_rate,
                                                           *pool({{3, audio_data({{1.0f}})}}), {}));
    REQUIRE(compiled.timeline_start == start * 2);
    REQUIRE(compiled.timeline_frame_count == 2);
}

TEST_CASE("playback property commands dirty and rebuild audible clip gain") {
    auto asset_pool = pool({{3, audio_data({std::vector<float>(8, 1.0f)})}});
    auto map = map_120();
    auto clip = absolute_media_clip(100, 0, 8, 3, 0, 8, {.gain_linear = 0.5f});
    auto track = take(Track::create({10}, "gain", {clip}));
    auto project = project_with_tracks({track}, {{3, "tone", 8, {48'000, 1}}});
    CompiledFixture compiled(project, map, asset_pool);

    Output before(1, 1);
    auto first_program = compiled.store.read();
    REQUIRE(ArrangementAudioRenderer::process(*first_program, snapshot(*first_program, 1),
                                              before.view()) == AudioRenderStatus::Rendered);
    REQUIRE(before.storage[0][0] == 0.5f);

    Transaction transaction;
    transaction.id = {{1}, 1};
    transaction.commands.push_back(
        {{{1}, 1},
         SetClipPlaybackProperties{
             {2}, {10}, {100}, {.gain_linear = 0.5f}, {.gain_linear = 0.25f}}});
    auto changed = reduce_transaction(*project, transaction);
    REQUIRE(changed);
    REQUIRE(changed->dirty.items().size() == 1);
    REQUIRE(changed->dirty.items()[0].owner_track == ItemId{10});
    REQUIRE(changed->dirty.items()[0].flags == DirtyFlags::Content);

    ProgramCompileRequest request;
    request.project = std::make_shared<const Project>(changed->project);
    request.sequence_id = {2};
    request.tempo_map = map;
    request.document_revision = 2;
    request.dirty = {false, {{10}}};
    request.audio_assets = asset_pool;
    REQUIRE(compiled.compiler->submit(std::move(request)));
    auto second_program = compiled.store.read();
    Output after(1, 1);
    REQUIRE(ArrangementAudioRenderer::process(*second_program, snapshot(*second_program, 1),
                                              after.view()) == AudioRenderStatus::Rendered);
    REQUIRE(after.storage[0][0] == 0.25f);
}

TEST_CASE("audio track linking remains incremental under one-work-unit slices") {
    constexpr std::size_t clip_count = 256;
    std::vector<Clip> clips;
    clips.reserve(clip_count);
    for (std::size_t index = 0; index < clip_count; ++index)
        clips.push_back(musical_media_clip(100 + index,
                                           static_cast<std::int64_t>(index * kTicksPerQuarter),
                                           kTicksPerQuarter, 3, 1));
    auto track = take(Track::create({10}, "many", std::move(clips)));
    auto project = project_with_tracks({track}, {{3, "sample", 1, {48'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, audio_data({{1.0f}})}}));
    REQUIRE(compiled.store.read()->find_track({10})->audio_program()->clips().size() == clip_count);
    REQUIRE(compiled.executor.slice_count > clip_count * 4);
}

TEST_CASE("take comp lowering remains incremental under one-work-unit slices") {
    constexpr std::size_t segment_count = 256;
    const auto recorded =
        take(Take::create({20}, MediaRef{{3}, {0}, segment_count}, {0}, RationalRate{48'000, 1}));
    std::vector<TakeCompSegment> segments;
    segments.reserve(segment_count);
    for (std::size_t index = 0; index < segment_count; ++index)
        segments.push_back(
            {.take_id = {20}, .range = {{static_cast<std::int64_t>(index)}, 1, {48'000, 1}}});
    auto lane = take(TakeLane::create({30}, "many selections", {recorded}, std::move(segments)));
    auto track = take(Track::create(
        TrackInput{.id = {10}, .name = "comp", .take_lanes = {lane}, .active_take_lane_id = {30}}));
    auto project = project_with_tracks({track}, {{3, "take", segment_count, {48'000, 1}}});
    CompiledFixture compiled(project, map_120(),
                             pool({{3, audio_data({std::vector<float>(segment_count, 1.0f)})}}));
    REQUIRE(compiled.store.read()->find_track({10})->audio_program()->clips().size() ==
            segment_count);
    REQUIRE(compiled.executor.slice_count > segment_count);
}

TEST_CASE("active take comp compile cost is independent of the hidden arrangement") {
    constexpr std::size_t hidden_clip_count = 512;
    const auto recorded =
        take(Take::create({20}, MediaRef{{3}, {0}, 1}, {0}, RationalRate{48'000, 1}));
    auto lane = take(TakeLane::create({30}, "active", {recorded},
                                      {{.take_id = {20}, .range = {{0}, 1, {48'000, 1}}}}));
    auto make_project = [&](std::size_t clip_count) {
        std::vector<Clip> clips;
        clips.reserve(clip_count);
        for (std::size_t index = 0; index < clip_count; ++index)
            clips.push_back(
                absolute_media_clip(100 + index, static_cast<std::int64_t>(index), 1, 3, 0, 1));
        auto track = take(Track::create(TrackInput{.id = {10},
                                                   .name = "comp",
                                                   .clips = std::move(clips),
                                                   .take_lanes = {lane},
                                                   .active_take_lane_id = {30}}));
        return project_with_tracks({track}, {{3, "take", 1, {48'000, 1}}});
    };
    const auto asset_pool = pool({{3, audio_data({{1.0f}})}});
    const auto tempo_map = map_120();
    auto compile_bytes = [&](std::shared_ptr<const Project> project) {
        PlaybackProgramStore store;
        InlineExecutor executor;
        PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
        ProgramCompileRequest request;
        request.project = std::move(project);
        request.sequence_id = {2};
        request.tempo_map = tempo_map;
        request.document_revision = 1;
        request.dirty.all = true;
        request.audio_assets = asset_pool;
        test::RtAllocationProbe probe;
        REQUIRE(compiler.submit(std::move(request)));
        REQUIRE(store.has_value());
        return probe.allocated_bytes();
    };
    const auto baseline_bytes = compile_bytes(make_project(0));
    const auto hidden_arrangement_bytes = compile_bytes(make_project(hidden_clip_count));
    REQUIRE(hidden_arrangement_bytes == baseline_bytes);
}

TEST_CASE("expired compile deadlines stop an in-progress audio link pass") {
    constexpr std::size_t clip_count = 64;
    std::vector<Clip> clips;
    for (std::size_t index = 0; index < clip_count; ++index)
        clips.push_back(musical_media_clip(100 + index,
                                           static_cast<std::int64_t>(index * kTicksPerQuarter),
                                           kTicksPerQuarter, 3, 1));
    auto track = take(Track::create({10}, "deadline", std::move(clips)));
    auto project = project_with_tracks({track}, {{3, "sample", 1, {48'000, 1}}});
    PlaybackProgramStore store;
    DeadlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = project;
    request.sequence_id = {2};
    request.tempo_map = map_120();
    request.document_revision = 1;
    request.dirty.all = true;
    request.audio_assets = pool({{3, audio_data({{1.0f}})}});
    REQUIRE(compiler.submit(std::move(request)));
    for (std::size_t index = 0; index <= clip_count; ++index)
        REQUIRE(executor.run_one(std::chrono::steady_clock::now() + std::chrono::seconds(1)) ==
                CompileTaskStatus::Pending);
    REQUIRE_FALSE(store.has_value());
    REQUIRE(executor.run_one(std::chrono::steady_clock::now() - std::chrono::milliseconds(1),
                             1'000'000) == CompileTaskStatus::Pending);
    REQUIRE_FALSE(store.has_value());
    executor.drain();
    REQUIRE(store.has_value());
}

namespace {

// A deterministic, fully specified generator: mt19937's output sequence is
// fixed by the standard, and the mapping to [-1, 1] avoids the distribution
// templates, whose output is implementation-defined. Two seeds give two
// uncorrelated takes, which is the case a crossfade shape has to answer for —
// correlated material sums coherently and passes under either shape, proving
// nothing.
std::vector<float> uncorrelated_noise(std::size_t frames, std::uint32_t seed) {
    std::vector<float> samples(frames);
    std::mt19937 engine(seed);
    for (auto& sample : samples)
        sample = 0.25f * (static_cast<float>(engine()) / 2'147'483'647.5f - 1.0f);
    return samples;
}

double window_power_db(const std::vector<float>& samples, std::size_t begin, std::size_t end) {
    REQUIRE(end > begin);
    REQUIRE(end <= samples.size());
    double sum = 0.0;
    for (std::size_t index = begin; index < end; ++index)
        sum += static_cast<double>(samples[index]) * static_cast<double>(samples[index]);
    return 10.0 * std::log10(sum / static_cast<double>(end - begin));
}

} // namespace

TEST_CASE("complementary equal-power fades hold power across a crossfade that linear dips") {
    constexpr std::size_t kClipFrames = 16'384;
    constexpr std::uint64_t kCrossfadeFrames = 8'192;
    constexpr std::size_t kSecondStart = kClipFrames - kCrossfadeFrames;
    constexpr std::size_t kTotalFrames = kSecondStart + kClipFrames;

    auto render = [](ClipFadeShape shape) {
        auto outgoing = absolute_media_clip(100, 0, kClipFrames, 3, 0, kClipFrames,
                                            {.gain_linear = 1.0f,
                                             .fade_in_duration = 0,
                                             .fade_out_duration = kCrossfadeFrames,
                                             .fade_shape = shape});
        auto incoming = absolute_media_clip(101, static_cast<std::int64_t>(kSecondStart),
                                            kClipFrames, 4, 0, kClipFrames,
                                            {.gain_linear = 1.0f,
                                             .fade_in_duration = kCrossfadeFrames,
                                             .fade_out_duration = 0,
                                             .fade_shape = shape});
        auto project = project_with_tracks(
            {take(Track::create({10}, "outgoing", {outgoing})),
             take(Track::create({11}, "incoming", {incoming}))},
            {{3, "outgoing", kClipFrames, {48'000, 1}}, {4, "incoming", kClipFrames, {48'000, 1}}});
        CompiledFixture compiled(project, map_120(),
                                 pool({{3, audio_data({uncorrelated_noise(kClipFrames, 20'260'731)})},
                                       {4, audio_data({uncorrelated_noise(kClipFrames, 90'210'419)})}}));
        auto program = compiled.store.read();
        Output output(1, kTotalFrames);
        REQUIRE(ArrangementAudioRenderer::process(
                    *program, snapshot(*program, static_cast<std::uint32_t>(kTotalFrames)),
                    output.view()) == AudioRenderStatus::Rendered);
        return output.storage[0];
    };

    // Steady state: inside the outgoing clip, before its fade begins, with the
    // incoming clip not yet started.
    constexpr std::size_t kSteadyBegin = 1'024;
    constexpr std::size_t kSteadyEnd = 5'120;
    // A window centred on the crossfade, narrow enough that it reports the
    // midpoint rather than an average over the whole transition. The dip lives
    // at the midpoint; averaged over the full region a linear pair loses only
    // about 1.8 dB, which understates what it sounds like.
    constexpr std::size_t kMidpoint = kSecondStart + kCrossfadeFrames / 2;
    constexpr std::size_t kMidpointBegin = kMidpoint - kCrossfadeFrames / 20;
    constexpr std::size_t kMidpointEnd = kMidpoint + kCrossfadeFrames / 20;

    const auto equal_power = render(ClipFadeShape::EqualPower);
    const auto steady_db = window_power_db(equal_power, kSteadyBegin, kSteadyEnd);
    const auto equal_power_midpoint_db =
        window_power_db(equal_power, kMidpointBegin, kMidpointEnd);
    const auto equal_power_region_db =
        window_power_db(equal_power, kSecondStart, kSecondStart + kCrossfadeFrames);
    INFO("steady " << steady_db << " dB, equal-power midpoint " << equal_power_midpoint_db
                   << " dB, equal-power region " << equal_power_region_db << " dB");
    REQUIRE_THAT(equal_power_midpoint_db, WithinAbs(steady_db, 0.5));
    REQUIRE_THAT(equal_power_region_db, WithinAbs(steady_db, 0.5));

    // The control that makes the assertion above falsifiable: the same
    // measurement over the same material with the linear shape must dip by
    // about 3 dB, so a green equal-power result cannot be coming from the
    // measurement being insensitive to the curve.
    const auto linear = render(ClipFadeShape::Linear);
    const auto linear_steady_db = window_power_db(linear, kSteadyBegin, kSteadyEnd);
    const auto linear_midpoint_db = window_power_db(linear, kMidpointBegin, kMidpointEnd);
    INFO("linear steady " << linear_steady_db << " dB, linear midpoint " << linear_midpoint_db
                          << " dB");
    REQUIRE_THAT(linear_steady_db, WithinAbs(steady_db, 1.0e-9));
    REQUIRE_THAT(linear_midpoint_db - linear_steady_db, WithinAbs(-3.01, 0.5));
}
