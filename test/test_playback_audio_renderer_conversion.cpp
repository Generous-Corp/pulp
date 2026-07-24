#include "playback_audio_renderer_test_support.hpp"

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
    REQUIRE_THAT(converted.storage[0].back(), WithinAbs(0.75f, 5e-7f));

    const std::array points{TempoPoint{{0}, 120.0}};
    auto map_96 = shared_compiled_tempo_map(points, RationalRate{96'000, 1});
    CompiledFixture reprepared(project, map_96, pool({{3, data}}));
    auto next = reprepared.store.read();
    REQUIRE(next->find_track({10})->audio_program()->clips()[0].timeline_start == 960);
    REQUIRE(next->find_track({10})->audio_program()->clips()[0].timeline_frame_count == 960);
}

TEST_CASE("audio renderer resamples a 44k ramp identically across split blocks") {
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
    REQUIRE_THAT(whole.storage[0][240], WithinAbs(220.5f, 1e-3f));
    REQUIRE_THAT(whole.storage[0][479], WithinAbs(440.0f, 0.05f));

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

TEST_CASE("audio renderer sample-rate conversion preserves passband and rejects aliases") {
    constexpr std::uint32_t source_rate = 96'000;
    constexpr std::uint32_t target_rate = 48'000;
    constexpr std::size_t source_frames = source_rate;
    constexpr std::size_t trim_frames = 2'048;
    constexpr double amplitude = 0.5;
    constexpr double passband_hz = 20'000.0;
    constexpr double stopband_hz = 30'000.0;
    constexpr double folded_stopband_hz = 18'000.0;

    const auto passband = render_resampled_tone(source_rate, target_rate, passband_hz, amplitude,
                                                source_frames, trim_frames);
    const auto passband_gain = tone_gain_db(passband, passband_hz / target_rate, amplitude);
    CAPTURE(passband_gain);
    REQUIRE(std::abs(passband_gain) <= kSrcPassbandGainToleranceDb);

    const auto stopband = render_resampled_tone(source_rate, target_rate, stopband_hz, amplitude,
                                                source_frames, trim_frames);
    const auto alias_gain = tone_gain_db(stopband, folded_stopband_hz / target_rate, amplitude);
    CAPTURE(alias_gain);
    REQUIRE(alias_gain <= kSrcStopbandRejectionDb);

    std::vector<double> linear_negative_control;
    linear_negative_control.reserve(stopband.size());
    for (std::size_t output_frame = trim_frames; output_frame < source_frames / 2u - trim_frames;
         ++output_frame) {
        const auto source_frame = output_frame * 2u;
        linear_negative_control.push_back(
            amplitude * std::sin(2.0 * kPi * stopband_hz * source_frame / source_rate));
    }
    const auto linear_alias_gain =
        tone_gain_db(linear_negative_control, folded_stopband_hz / target_rate, amplitude);
    CAPTURE(linear_alias_gain);
    REQUIRE(linear_alias_gain >= kLinearNegativeControlAliasDb);

    constexpr std::uint32_t upsample_source_rate = 44'100;
    constexpr double upsample_passband_hz = 18'000.0;
    const auto upsampled =
        render_resampled_tone(upsample_source_rate, target_rate, upsample_passband_hz, amplitude,
                              upsample_source_rate, trim_frames);
    const auto upsample_gain =
        tone_gain_db(upsampled, upsample_passband_hz / target_rate, amplitude);
    const auto upsample_residual = tone_residual_db(upsampled, upsample_passband_hz / target_rate);
    CAPTURE(upsample_gain, upsample_residual);
    REQUIRE(std::abs(upsample_gain) <= kSrcPassbandGainToleranceDb);
    REQUIRE(upsample_residual <= kSrcPassbandPurityDb);
}

TEST_CASE("ordinary musical playback uses fixed-rate anti-alias conversion") {
    constexpr std::uint32_t source_rate = 96'000;
    constexpr std::uint32_t target_rate = 48'000;
    constexpr std::size_t source_frames = 4'800;
    constexpr std::size_t target_frames = source_frames / 2u;
    constexpr std::size_t trim_frames = 256;
    constexpr double amplitude = 0.5;
    constexpr double stopband_hz = 30'000.0;
    constexpr double folded_stopband_hz = 18'000.0;

    std::vector<float> source(source_frames);
    for (std::size_t frame = 0; frame < source.size(); ++frame)
        source[frame] = static_cast<float>(
            amplitude * std::sin(2.0 * kPi * stopband_hz * static_cast<double>(frame) /
                                 static_cast<double>(source_rate)));
    const auto data = audio_data({source}, source_rate);
    auto clip = musical_media_clip(100, 0, kTicksPerQuarter / 10, 3,
                                   static_cast<std::uint64_t>(source_frames));
    auto track = take(Track::create({10}, "musical stopband", {clip}));
    auto project = project_with_tracks({track}, {{3, "bright", source_frames, {source_rate, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, data}}));
    auto program = compiled.store.read();
    const auto& program_clip = program->find_track({10})->audio_program()->clips().front();
    REQUIRE(program_clip.uses_sample_rate_conversion());
    REQUIRE(program_clip.uses_host_rate_conversion());

    Output output(1, target_frames);
    REQUIRE(ArrangementAudioRenderer::process(
                *program, snapshot(*program, static_cast<std::uint32_t>(target_frames)),
                output.view()) == AudioRenderStatus::Rendered);
    const std::vector<double> rendered{
        output.storage[0].begin() + static_cast<std::ptrdiff_t>(trim_frames),
        output.storage[0].end() - static_cast<std::ptrdiff_t>(trim_frames)};
    const auto alias_gain = tone_gain_db(rendered, folded_stopband_hz / target_rate, amplitude);
    CAPTURE(alias_gain);
    REQUIRE(alias_gain <= kSrcStopbandRejectionDb);
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
    REQUIRE(clips[0].uses_sample_rate_conversion());
    REQUIRE(clips[0].shares_sample_rate_conversion_with(clips[1]));
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
    const auto foreign = shared_compiled_tempo_map(foreign_points, RationalRate{48'000, 1});
    CompiledFixture compiled(project, baseline, pool({{3, data}}));
    auto program = compiled.store.read();
    REQUIRE_FALSE(program->find_track({10})
                      ->audio_program()
                      ->clips()[0]
                      .uses_sample_rate_conversion());
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
    std::vector<float> ramp(256);
    for (std::size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = static_cast<float>(i);
    const auto data = audio_data({ramp}, 48'000);
    auto clip = take(Clip::create_absolute({100}, {0}, 256, {48'000, 1}, MediaRef{{3}, {0}, 256}));
    auto track = take(Track::create({10}, "loop", {clip}));
    auto project = project_with_tracks({track}, {{3, "ramp", 256, {48'000, 1}}});
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

    auto high_rate_clip = take(Clip::create_absolute({101}, {0}, 8, {768'000, 1},
                                                     MediaRef{{4}, {0}, 8}, {.gain_linear = 1.0f}));
    auto high_rate_track = take(Track::create({11}, "unsupported decimation", {high_rate_clip}));
    auto high_rate_project =
        project_with_tracks({high_rate_track}, {{4, "high-rate", 8, {768'000, 1}}});
    auto high_rate_pool = pool({{4, audio_data({std::vector<float>(8, 1.0f)}, 768'000)}});
    auto unsupported =
        compile_audio_clip_program(high_rate_clip, *high_rate_project, *map, *high_rate_pool, {});
    REQUIRE_FALSE(unsupported);
    REQUIRE(unsupported.error().code == AudioRendererErrorCode::UnsupportedSampleRate);

    auto compiled_clip =
        take(compile_audio_clip_program(clip, *project, *map, *pool({{3, good}}), {}));

    auto musical_clip =
        musical_media_clip(101, 0, kTicksPerQuarter, 3, good->num_frames());
    auto musical_track = take(Track::create({11}, "musical", {musical_clip}));
    auto musical_project =
        project_with_tracks({musical_track}, {{3, "musical", good->num_frames(), {48'000, 1}}});
    auto compiled_musical = take(compile_audio_clip_program(
        musical_clip, *musical_project, *map, *pool({{3, good}}), {}));
    const auto foreign_audio = audio_data({std::vector<float>(good->num_frames(), 0.25f)});
    auto foreign_project = project_with_tracks(
        {musical_track}, {{3, "foreign", foreign_audio->num_frames(), {48'000, 1}}});
    auto foreign_musical = take(compile_audio_clip_program(
        musical_clip, *foreign_project, *map, *pool({{3, foreign_audio}}), {}));
    compiled_musical.conversion_artifact = foreign_musical.conversion_artifact;
    auto linked = link_audio_track_program({11}, {compiled_musical}, {});
    REQUIRE_FALSE(linked);
    REQUIRE(linked.error().code == AudioRendererErrorCode::InvalidAsset);
    auto sliced_clip = take(Clip::create({102}, {0}, {kTicksPerQuarter},
                                         MediaRef{{3}, {1}, good->num_frames() - 1}));
    auto sliced_track = take(Track::create({12}, "sliced", {sliced_clip}));
    auto sliced_project =
        project_with_tracks({sliced_track}, {{3, "sliced", good->num_frames(), {48'000, 1}}});
    auto sliced_musical = take(compile_audio_clip_program(
        sliced_clip, *sliced_project, *map, *pool({{3, good}}), {}));
    compiled_musical.conversion_artifact = sliced_musical.conversion_artifact;
    linked = link_audio_track_program({11}, {compiled_musical}, {});
    REQUIRE_FALSE(linked);
    REQUIRE(linked.error().code == AudioRendererErrorCode::InvalidAsset);

    auto malformed_arrangement = compiled_clip;
    malformed_arrangement.source_ordinal = 1;
    linked = link_audio_track_program({10}, {malformed_arrangement}, {});
    REQUIRE_FALSE(linked);
    REQUIRE(linked.error().code == AudioRendererErrorCode::InvalidIdentity);
    auto first_selection = compiled_clip;
    first_selection.source_kind = AudioClipRendererProgram::SourceKind::TakeCompSegment;
    first_selection.source_ordinal = 1;
    auto second_selection = first_selection;
    second_selection.source_ordinal = 2;
    linked = link_audio_track_program({10}, {first_selection, second_selection}, {});
    REQUIRE(linked);
    second_selection.id = {999};
    second_selection.source_ordinal = 1;
    linked = link_audio_track_program({10}, {first_selection, second_selection}, {});
    REQUIRE_FALSE(linked);
    REQUIRE(linked.error().code == AudioRendererErrorCode::InvalidIdentity);
    first_selection.source_ordinal = 0;
    linked = link_audio_track_program({10}, {first_selection}, {});
    REQUIRE_FALSE(linked);
    REQUIRE(linked.error().code == AudioRendererErrorCode::InvalidIdentity);

    linked = link_audio_track_program({10}, {compiled_clip}, {.max_clips = 0});
    REQUIRE_FALSE(linked);
    REQUIRE(linked.error().code == AudioRendererErrorCode::CapacityExceeded);
    auto duplicate_late = compiled_clip;
    duplicate_late.timeline_start = 100;
    auto interleaved = compiled_clip;
    interleaved.id = {999};
    interleaved.timeline_start = 50;
    linked = link_audio_track_program({10}, {compiled_clip, interleaved, duplicate_late}, {});
    REQUIRE_FALSE(linked);
    REQUIRE(linked.error().code == AudioRendererErrorCode::InvalidIdentity);

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

TEST_CASE("audio compiler bounds distinct sample-rate conversion kernels") {
    const auto data_44 = audio_data({std::vector<float>(8, 1.0f)}, 44'100);
    const auto data_96 = audio_data({std::vector<float>(8, 1.0f)}, 96'000);
    auto first = absolute_media_clip(100, 0, 8, 3, 0, 8);
    auto second = absolute_media_clip(101, 8, 8, 4, 0, 8);
    auto track = take(Track::create({10}, "rates", {first, second}));
    auto project =
        project_with_tracks({track}, {{3, "44k", 8, {44'100, 1}}, {4, "96k", 8, {96'000, 1}}});
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = project;
    request.sequence_id = {2};
    request.tempo_map = map_120();
    request.document_revision = 1;
    request.dirty.all = true;
    request.audio_assets = pool({{3, data_44}, {4, data_96}});
    request.audio_limits.max_sample_rate_converters = 1;

    REQUIRE(compiler.submit(std::move(request)));
    REQUIRE_FALSE(store.has_value());
    REQUIRE(compiler.status().has_error);
    REQUIRE(compiler.status().last_error.code == CompileErrorCode::AudioProgramInvalid);
    REQUIRE(compiler.status().last_error.audio_detail == AudioRendererErrorCode::CapacityExceeded);
}

TEST_CASE("fixed sample-rate conversion kernels and cache storage obey the aggregate byte limit") {
    const auto data = audio_data({std::vector<float>(8, 1.0f)}, 44'100);
    auto clip = absolute_media_clip(100, 0, 8, 3, 0, 8);
    auto track = take(Track::create({10}, "fixed rate bytes", {clip}));
    auto project = project_with_tracks({track}, {{3, "44k", 8, {44'100, 1}}});
    AudioRendererLimits limits;
    limits.max_sample_rate_converter_bytes =
        audio::PreparedSampleRateConversion::estimated_prepared_bytes();
    auto compiled =
        compile_audio_clip_program(clip, *project, *map_120(), *pool({{3, data}}), limits);
    REQUIRE_FALSE(compiled);
    REQUIRE(compiled.error().code == AudioRendererErrorCode::CapacityExceeded);
}

TEST_CASE("fixed sample-rate conversion rejects a zero cache budget before preparation") {
    const auto data = audio_data({std::vector<float>(8, 1.0f)}, 44'100);
    auto clip = absolute_media_clip(100, 0, 8, 3, 0, 8);
    auto track = take(Track::create({10}, "zero fixed rate bytes", {clip}));
    auto project = project_with_tracks({track}, {{3, "44k", 8, {44'100, 1}}});
    AudioRendererLimits limits;
    limits.max_sample_rate_converter_bytes = 0;
    auto compiled =
        compile_audio_clip_program(clip, *project, *map_120(), *pool({{3, data}}), limits);
    REQUIRE_FALSE(compiled);
    REQUIRE(compiled.error().code == AudioRendererErrorCode::CapacityExceeded);
}

TEST_CASE("fixed sample-rate conversion preparation yields within compiler work slices") {
    const auto data = audio_data({std::vector<float>(8, 1.0f)}, 44'100);
    auto clip = absolute_media_clip(100, 0, 8, 3, 0, 8);
    auto track = take(Track::create({10}, "fixed rate incremental", {clip}));
    auto project = project_with_tracks({track}, {{3, "44k", 8, {44'100, 1}}});
    PlaybackProgramStore store;
    DeadlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = project;
    request.sequence_id = {2};
    request.tempo_map = map_120();
    request.document_revision = 1;
    request.dirty.all = true;
    request.audio_assets = pool({{3, data}});
    REQUIRE(compiler.submit(std::move(request)));

    for (std::size_t index = 0; index < 100; ++index)
        REQUIRE(executor.run_one(std::chrono::steady_clock::now() + std::chrono::seconds(1)) ==
                CompileTaskStatus::Pending);
    REQUIRE_FALSE(store.has_value());
    executor.drain();
    REQUIRE(store.has_value());
}

TEST_CASE("host-tempo conversion preparation is incremental and capacity bounded") {
    constexpr std::size_t source_frames = 4'096;
    const auto data = audio_data({std::vector<float>(source_frames, 1.0f)});
    auto clip = musical_media_clip(100, 0, kTicksPerQuarter, 3, source_frames);
    auto track = take(Track::create({10}, "host converter", {clip}));
    auto project = project_with_tracks({track}, {{3, "long.wav", source_frames, {48'000, 1}}});

    SECTION("one-work-unit slices cannot construct the pyramid synchronously") {
        PlaybackProgramStore store;
        DeadlineExecutor executor;
        PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
        ProgramCompileRequest request;
        request.project = project;
        request.sequence_id = {2};
        request.tempo_map = map_120();
        request.document_revision = 1;
        request.dirty.all = true;
        request.audio_assets = pool({{3, data}});
        REQUIRE(compiler.submit(std::move(request)));
        for (std::size_t index = 0; index < 100; ++index)
            REQUIRE(executor.run_one(std::chrono::steady_clock::now() + std::chrono::seconds(1)) ==
                    CompileTaskStatus::Pending);
        REQUIRE_FALSE(store.has_value());
        executor.drain();
        REQUIRE(store.has_value());
    }

    SECTION("converter count includes host-tempo pyramids") {
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
        request.audio_limits.max_sample_rate_converters = 0;
        REQUIRE(compiler.submit(std::move(request)));
        REQUIRE_FALSE(store.has_value());
        REQUIRE(compiler.status().last_error.audio_detail ==
                AudioRendererErrorCode::CapacityExceeded);
    }

    SECTION("prepared pyramid bytes obey the aggregate limit") {
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
        request.audio_limits.max_sample_rate_converter_bytes = 4;
        REQUIRE(compiler.submit(std::move(request)));
        REQUIRE_FALSE(store.has_value());
        REQUIRE(compiler.status().last_error.audio_detail ==
                AudioRendererErrorCode::CapacityExceeded);
    }
}

TEST_CASE("incremental audio compilation shares and globally bounds conversion kernels") {
    const auto data_44 = audio_data({std::vector<float>(8, 1.0f)}, 44'100);
    const auto data_48 = audio_data({std::vector<float>(8, 1.0f)}, 48'000);
    const auto data_96 = audio_data({std::vector<float>(8, 1.0f)}, 96'000);
    auto assets = pool({{3, data_44}, {4, data_44}, {5, data_48}, {6, data_96}});
    auto map = map_120();
    auto make_project = [](std::uint64_t second_asset) {
        auto clean = take(Track::create({10}, "clean", {absolute_media_clip(100, 0, 8, 3, 0, 8)}));
        auto dirty = take(
            Track::create({20}, "dirty", {absolute_media_clip(200, 8, 8, second_asset, 0, 8)}));
        return project_with_tracks({clean, dirty}, {{3, "44-a", 8, {44'100, 1}},
                                                    {4, "44-b", 8, {44'100, 1}},
                                                    {5, "48", 8, {48'000, 1}},
                                                    {6, "96", 8, {96'000, 1}}});
    };

    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest first;
    first.project = make_project(5);
    first.sequence_id = {2};
    first.tempo_map = map;
    first.document_revision = 1;
    first.dirty.all = true;
    first.audio_assets = assets;
    first.audio_limits.max_sample_rate_converters = 1;
    REQUIRE(compiler.submit(std::move(first)));
    REQUIRE(store.read()->document_revision() == 1);

    SECTION("a second distinct pair exceeds the whole-program limit") {
        ProgramCompileRequest second;
        second.project = make_project(6);
        second.sequence_id = {2};
        second.tempo_map = map;
        second.document_revision = 2;
        second.dirty.tracks = {{20}};
        second.audio_assets = assets;
        second.audio_limits.max_sample_rate_converters = 1;
        REQUIRE(compiler.submit(std::move(second)));
        REQUIRE(compiler.status().has_error);
        REQUIRE(compiler.status().last_error.audio_detail ==
                AudioRendererErrorCode::CapacityExceeded);
        REQUIRE(store.read()->document_revision() == 1);
    }

    SECTION("a reused pair shares the seeded converter") {
        ProgramCompileRequest second;
        second.project = make_project(4);
        second.sequence_id = {2};
        second.tempo_map = map;
        second.document_revision = 2;
        second.dirty.tracks = {{20}};
        second.audio_assets = assets;
        second.audio_limits.max_sample_rate_converters = 1;
        REQUIRE(compiler.submit(std::move(second)));
        auto published = store.read();
        REQUIRE(published->document_revision() == 2);
        const auto& clean = published->find_track({10})->audio_program()->clips().front();
        const auto& dirty = published->find_track({20})->audio_program()->clips().front();
        REQUIRE(clean.uses_sample_rate_conversion());
        REQUIRE(clean.shares_sample_rate_conversion_with(dirty));
    }
}

TEST_CASE("incremental audio compilation reuses seeded host-tempo pyramids") {
    const auto data = audio_data({std::vector<float>(64, 1.0f)});
    const auto assets = pool({{3, data}});
    const auto map = map_120();
    auto make_project = [] {
        auto clean = take(
            Track::create({10}, "clean", {musical_media_clip(100, 0, kTicksPerQuarter, 3, 64)}));
        auto dirty = take(
            Track::create({20}, "dirty", {musical_media_clip(200, 0, kTicksPerQuarter, 3, 64)}));
        return project_with_tracks({clean, dirty}, {{3, "shared.wav", 64, {48'000, 1}}});
    };

    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest first;
    first.project = make_project();
    first.sequence_id = {2};
    first.tempo_map = map;
    first.document_revision = 1;
    first.dirty.all = true;
    first.audio_assets = assets;
    first.audio_limits.max_sample_rate_converters = 1;
    REQUIRE(compiler.submit(std::move(first)));
    const auto original =
        store.read()->find_track({10})->audio_program()->clips().front();
    REQUIRE(original.uses_host_rate_conversion());

    ProgramCompileRequest second;
    second.project = make_project();
    second.sequence_id = {2};
    second.tempo_map = map;
    second.document_revision = 2;
    second.dirty.tracks = {{20}};
    second.audio_assets = assets;
    second.audio_limits.max_sample_rate_converters = 1;
    REQUIRE(compiler.submit(std::move(second)));
    const auto published = store.read();
    REQUIRE(published->document_revision() == 2);
    REQUIRE(published->find_track({10})
                ->audio_program()
                ->clips()
                .front()
                .shares_host_rate_conversion_with(original));
    REQUIRE(published->find_track({20})
                ->audio_program()
                ->clips()
                .front()
                .shares_host_rate_conversion_with(original));
}

TEST_CASE("compiler invalidation covers global clip counts assets and exact tempo identity") {
    auto make_project = [](bool extra_dirty_clip) {
        std::vector<Clip> dirty{absolute_media_clip(100, 0, 8, 3, 0, 8)};
        if (extra_dirty_clip)
            dirty.push_back(absolute_media_clip(101, 16, 8, 3, 0, 8));
        auto first = take(Track::create({10}, "dirty", std::move(dirty)));
        auto second =
            take(Track::create({20}, "clean", {absolute_media_clip(200, 32, 8, 3, 0, 8)}));
        return project_with_tracks({first, second}, {{3, "tone", 8, {48'000, 1}}});
    };
    auto data = audio_data({std::vector<float>(8, 1.0f)});
    auto first_pool = pool({{3, data}});
    auto first_map = map_120();
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));

    ProgramCompileRequest first;
    first.project = make_project(false);
    first.sequence_id = {2};
    first.tempo_map = first_map;
    first.document_revision = 1;
    first.dirty.all = true;
    first.audio_assets = first_pool;
    first.audio_limits.max_clips = 2;
    REQUIRE(compiler.submit(std::move(first)));
    auto baseline = store.read();
    const auto* baseline_dirty = baseline->find_track({10});
    const auto* baseline_clean = baseline->find_track({20});

    ProgramCompileRequest over_limit;
    over_limit.project = make_project(true);
    over_limit.sequence_id = {2};
    over_limit.tempo_map = first_map;
    over_limit.document_revision = 2;
    over_limit.dirty.tracks = {{10}};
    over_limit.audio_assets = first_pool;
    over_limit.audio_limits.max_clips = 2;
    REQUIRE(compiler.submit(std::move(over_limit)));
    REQUIRE(compiler.status().has_error);
    REQUIRE(compiler.status().last_error.audio_detail == AudioRendererErrorCode::CapacityExceeded);
    REQUIRE(store.read()->document_revision() == 1);

    auto second_pool = pool({{3, data}});
    ProgramCompileRequest new_pool;
    new_pool.project = make_project(false);
    new_pool.sequence_id = {2};
    new_pool.tempo_map = first_map;
    new_pool.document_revision = 3;
    new_pool.dirty.tracks = {{10}};
    new_pool.audio_assets = second_pool;
    new_pool.audio_limits.max_clips = 2;
    REQUIRE(compiler.submit(std::move(new_pool)));
    auto pool_rebuilt = store.read();
    REQUIRE(pool_rebuilt->find_track({10}) != baseline_dirty);
    REQUIRE(pool_rebuilt->find_track({20}) != baseline_clean);
    const auto* pool_dirty = pool_rebuilt->find_track({10});
    const auto* pool_clean = pool_rebuilt->find_track({20});

    auto same_rate_new_map = map_120();
    ProgramCompileRequest new_map;
    new_map.project = make_project(false);
    new_map.sequence_id = {2};
    new_map.tempo_map = same_rate_new_map;
    new_map.document_revision = 4;
    new_map.dirty.tracks = {{10}};
    new_map.audio_assets = second_pool;
    new_map.audio_limits.max_clips = 2;
    REQUIRE(compiler.submit(std::move(new_map)));
    auto map_rebuilt = store.read();
    REQUIRE(map_rebuilt->find_track({10}) != pool_dirty);
    REQUIRE(map_rebuilt->find_track({20}) != pool_clean);
    REQUIRE(map_rebuilt->tempo_map_owner().get() == same_rate_new_map.get());
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
    std::fill(output.storage[0].begin(), output.storage[0].end(), 7.0f);
    std::fill(output.storage[1].begin(), output.storage[1].end(), 7.0f);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 64), output.view(),
                                              {.max_channels = 1}) ==
            AudioRenderStatus::CapacityExceeded);
    REQUIRE(output.storage[0] == std::vector<float>(64, 7.0f));
    REQUIRE(output.storage[1] == std::vector<float>(64, 7.0f));

    CompiledFixture narrow(project, map_120(), pool({{3, data}}), {.max_channels = 1});
    auto narrow_program = narrow.store.read();
    REQUIRE(ArrangementAudioRenderer::process(*narrow_program, snapshot(*narrow_program, 64),
                                              output.view()) ==
            AudioRenderStatus::CapacityExceeded);
}

TEST_CASE("prepared sample-rate conversion rejects invalid source domains") {
    const audio::PreparedSampleRateConversion converter(1.0);
    REQUIRE(converter.prepared_bytes() ==
            audio::PreparedSampleRateConversion::estimated_prepared_bytes());
    REQUIRE(converter.read({}, 0.0) == 0.0f);

    constexpr std::array source{0.25f};
    REQUIRE_THAT(converter.read(source, -1.0e300), WithinAbs(0.25f, 1.0e-6f));
    REQUIRE_THAT(converter.read(source, 1.0e300), WithinAbs(0.25f, 1.0e-6f));
    REQUIRE(converter.read(source, std::numeric_limits<double>::quiet_NaN()) == 0.0f);
    REQUIRE(converter.read(source, std::numeric_limits<double>::infinity()) == 0.0f);

    auto variable_source = std::make_shared<audio::AudioFileData>();
    variable_source->sample_rate = 48'000;
    variable_source->channels = {{0.25f, 0.25f, 0.25f, 0.25f}};
    const auto variable = audio::PreparedVariableRateConversion::build(variable_source, 0, 4);
    const auto variable_bytes = audio::PreparedVariableRateConversion::prepared_bytes(4, 1);
    REQUIRE(variable_bytes);
    REQUIRE(variable->prepared_bytes() == *variable_bytes);
    REQUIRE(variable->prepared_bytes() > 500'000);
    REQUIRE_THAT(
        variable->read(0, 1.5,
                       audio::PreparedVariableRateConversion::kMaximumSourceFramesPerOutputFrame),
        WithinAbs(0.25f, 1.0e-6f));
    REQUIRE(variable->read(
                0, 0.0,
                audio::PreparedVariableRateConversion::kMaximumSourceFramesPerOutputFrame * 2.0) ==
            0.0f);

    auto stopband_source = std::make_shared<audio::AudioFileData>();
    stopband_source->sample_rate = 48'000;
    stopband_source->channels.resize(1);
    stopband_source->channels[0].resize(4'096);
    for (std::size_t frame = 0; frame < stopband_source->channels[0].size(); ++frame)
        stopband_source->channels[0][frame] =
            static_cast<float>(std::sin(2.0 * kPi * static_cast<double>(frame) / 32.0));
    const auto stopband_converter = audio::PreparedVariableRateConversion::build(
        stopband_source, 0, stopband_source->num_frames());
    float stopband_peak = 0.0f;
    for (std::size_t frame = 0; frame < 32; ++frame)
        stopband_peak = std::max(
            stopband_peak, std::abs(stopband_converter->read(0, 1'024.0 + frame * 64.0, 64.0)));
    REQUIRE(stopband_peak < 1.0e-3f);

    const audio::PreparedSampleRateConversion unsupported(1.0 / 16.0);
    REQUIRE_FALSE(unsupported.ready());
    REQUIRE(unsupported.read(source, 0.0) == 0.0f);

    REQUIRE_FALSE(audio::PreparedVariableRateConversion::build(nullptr, 0, 1));
    auto empty = std::make_shared<audio::AudioFileData>();
    REQUIRE_FALSE(audio::PreparedVariableRateConversion::build(empty, 0, 1));
    auto malformed = std::make_shared<audio::AudioFileData>();
    malformed->channels = {{1.0f, 2.0f}, {1.0f}};
    REQUIRE_FALSE(audio::PreparedVariableRateConversion::build(malformed, 0, 1));
    REQUIRE_FALSE(audio::PreparedVariableRateConversion::build(variable_source, 4, 1));
    audio::VariableRateConversionBuilder invalid_builder(nullptr, 0, 1);
    REQUIRE_FALSE(invalid_builder.valid());
    REQUIRE(invalid_builder.step());
    REQUIRE_FALSE(invalid_builder.take());
}
