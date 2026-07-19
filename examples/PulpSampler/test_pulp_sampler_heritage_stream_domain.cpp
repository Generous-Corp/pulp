#include "test_pulp_sampler_heritage_support.hpp"

TEST_CASE("PulpSampler sizes streamed contracts in the heritage clock domain",
          "[audio][sampler][heritage][stream][capacity]") {
    HeritageTempWav source("clock_domain", 2000000);
    const auto profile = clock_profile(2.0);
    HeritageFixture clocked(64, &profile);
    REQUIRE(clocked.processor.load_sample_file(source.path));
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                clocked.processor) == 96000.0);
    REQUIRE(PulpSamplerHeritageTestAccess::maximum_stream_block_frames(
                clocked.processor) > 64);
    const auto contract = PulpSamplerHeritageTestAccess::preload_contract(
        clocked.processor);
    REQUIRE(contract.host_sample_rate == 96000.0);
    REQUIRE(contract.maximum_host_block_frames ==
            PulpSamplerHeritageTestAccess::maximum_stream_block_frames(
                clocked.processor));
    constexpr std::array block{std::size_t{64}};
    (void) render(clocked, block);
    const auto clocked_position =
        PulpSamplerHeritageTestAccess::active_streamed_position(
            clocked.processor);
    REQUIRE(clocked_position >= 127.0);
    REQUIRE(clocked_position <= 129.0);
    for (int callback = 0; callback < 16; ++callback)
        (void) render(clocked, block, 65);
    REQUIRE(PulpSamplerHeritageTestAccess::last_lookahead_demand_fps(
                clocked.processor) == 96000.0);

    const auto bypass = clock_profile(2.0, true);
    HeritageFixture neutral(64, &bypass);
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                neutral.processor) == 48000.0);
    REQUIRE(PulpSamplerHeritageTestAccess::maximum_stream_block_frames(
                neutral.processor) == 64);

    HeritageFixture reverse_clocked(64, &profile);
    reverse_clocked.store.set_value(kSamplerReverse, 1.0f);
    REQUIRE(reverse_clocked.processor.load_sample_file(source.path));
    REQUIRE(PulpSamplerHeritageTestAccess::retire_reverse_tail_page(
        reverse_clocked.processor));
    (void) render(reverse_clocked, block);
    REQUIRE(PulpSamplerHeritageTestAccess::last_stream_demand_fps(
                reverse_clocked.processor) == 96000.0);
}

TEST_CASE("PulpSampler transactionally rebinds loaded streams across clock changes",
          "[audio][sampler][heritage][stream][configuration]") {
    HeritageTempWav source("clock_rebind");
    const auto profile = clock_profile(2.0);
    HeritageFixture fixture(64, &profile);
    REQUIRE(fixture.processor.load_sample_file(source.path));
    REQUIRE(PulpSamplerHeritageTestAccess::has_retained_streamed_source(
        fixture.processor));
    const auto source_frames = fixture.processor.sample_length();
    const auto replacement = clock_profile(1.25);
    const auto before = fixture.processor.heritage_diagnostics();
    const auto latency_before = fixture.processor.latency_samples();

    PulpSamplerHeritageTestAccess::fail_next_stream_domain_prepare(
        fixture.processor);
    REQUIRE(fixture.processor.set_heritage_profile(replacement) ==
            PulpSamplerHeritageStatus::PrepareFailed);
    REQUIRE(fixture.processor.heritage_diagnostics().profile() ==
            profile.profile_id);
    REQUIRE(fixture.processor.sample_length() == source_frames);
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                fixture.processor) == 96000.0);
    constexpr std::array proof_block{std::size_t{64}};
    const auto after_prepare_failure = render(fixture, proof_block);
    REQUIRE(std::any_of(after_prepare_failure.begin(),
                        after_prepare_failure.end(), [](float value) {
        return std::abs(value) > 0.001f;
    }));

    PulpSamplerHeritageTestAccess::fail_next_stream_domain_source_restore(
        fixture.processor);
    REQUIRE(fixture.processor.set_heritage_profile(replacement) ==
            PulpSamplerHeritageStatus::PrepareFailed);
    REQUIRE(fixture.processor.heritage_diagnostics().profile() ==
            profile.profile_id);
    REQUIRE(fixture.processor.sample_length() == source_frames);
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                fixture.processor) == 96000.0);
    const auto after_restore_failure = render(fixture, proof_block, 65);
    REQUIRE(std::any_of(after_restore_failure.begin(),
                        after_restore_failure.end(), [](float value) {
        return std::abs(value) > 0.001f;
    }));

    REQUIRE(fixture.processor.set_heritage_profile(replacement) ==
            PulpSamplerHeritageStatus::Ready);
    REQUIRE(pulp_sampler_heritage_status_name(
                PulpSamplerHeritageStatus::StreamDomainRebindRequired) ==
            "stream-domain-rebind-required");
    REQUIRE(fixture.processor.has_sample());
    const auto after_replacement = fixture.processor.heritage_diagnostics();
    REQUIRE(after_replacement.profile() == replacement.profile_id);
    REQUIRE(after_replacement.clock_ratio == 1.25);
    REQUIRE(after_replacement.rate_admission_rejections ==
            before.rate_admission_rejections);
    REQUIRE(after_replacement.rate_automation_rejections ==
            before.rate_automation_rejections);
    REQUIRE(fixture.processor.latency_samples() != latency_before);
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                fixture.processor) == 60000.0);
    const auto contract = PulpSamplerHeritageTestAccess::preload_contract(
        fixture.processor);
    REQUIRE(contract.host_sample_rate == 60000.0);

    const auto same_clock = clock_output_profile(2.0, 0.5f);
    REQUIRE(fixture.processor.set_heritage_profile(same_clock) ==
            PulpSamplerHeritageStatus::Ready);
    REQUIRE(fixture.processor.has_sample());
    REQUIRE(fixture.processor.heritage_diagnostics().profile() ==
            same_clock.profile_id);
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                fixture.processor) == 96000.0);
    REQUIRE(fixture.processor.disable_heritage() ==
            PulpSamplerHeritageStatus::Disabled);
    REQUIRE(fixture.processor.has_sample());
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                fixture.processor) == 48000.0);
    constexpr std::array disabled_block{std::size_t{64}};
    REQUIRE(render(fixture, disabled_block).size() == 64);
}

TEST_CASE("PulpSampler sizes heritage pitch from the admitted clock product",
          "[audio][sampler][heritage][stream][capacity][resources]") {
    constexpr std::size_t block_frames = 64;
    const auto profile = typed_pitch_artifact_profile(
        audio::SampleHeritagePitchFamily::VariableClock, false, 2.0);
    HeritageFixture fixture(block_frames, &profile);
    const auto stream_frames =
        PulpSamplerHeritageTestAccess::maximum_stream_block_frames(
            fixture.processor);
    REQUIRE(stream_frames > block_frames);
    REQUIRE(stream_frames < 400);

    format::PrepareContext context;
    context.sample_rate = 48000.0;
    context.max_buffer_size = static_cast<int>(block_frames);
    context.input_channels = 0;
    context.output_channels = 2;
    PulpSamplerProcessor clean;
    const auto clean_usage = clean.estimate_prepare_resources(context);
    PulpSamplerProcessor pitched;
    REQUIRE(pitched.set_heritage_profile(profile) ==
            PulpSamplerHeritageStatus::PendingPrepare);
    const auto pitched_usage = pitched.estimate_prepare_resources(context);
    REQUIRE(pitched_usage.persistent_bytes > clean_usage.persistent_bytes);
    REQUIRE(pitched_usage.block_scratch_bytes >
            clean_usage.block_scratch_bytes * 10u);
}

TEST_CASE("PulpSampler preserves the configured streaming cap across clock changes",
          "[audio][sampler][heritage][stream][capacity]") {
    const auto initial =
        SamplerStreamingRuntime::estimate_prepare(48000.0f, 64);
    REQUIRE(initial.prepared());
    const auto configured = initial.required_streaming_memory_bytes + 4096;

    HeritageFixture fixture(
        64, nullptr, 48000.0, PulpSamplerConfig{configured});
    REQUIRE(fixture.processor.prepare_result().prepared());
    REQUIRE(fixture.processor.prepare_result().configured_streaming_memory_bytes ==
            configured);
    REQUIRE(fixture.processor.diagnostics().streaming_memory_capacity_bytes ==
            configured);

    REQUIRE(fixture.processor.set_heritage_profile(clock_profile(2.0)) ==
            PulpSamplerHeritageStatus::Ready);
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                fixture.processor) == 96000.0);
    REQUIRE(fixture.processor.diagnostics().streaming_memory_capacity_bytes ==
            configured);
}

TEST_CASE("PulpSampler serializes a staged load against stream-domain rebind",
          "[audio][sampler][heritage][stream][configuration][thread]") {
    HeritageTempWav source("clock_rebind_concurrent");
    HeritageFixture fixture(64);
    const auto profile = clock_profile(2.0);
    const auto heritage_before =
        PulpSamplerHeritageTestAccess::control_heritage_counts(
            fixture.processor);
    PulpSamplerHeritageTestAccess::pause_file_stage(fixture.processor, true);

    PulpSamplerLoadResult loaded;
    PulpSamplerHeritageStatus configured =
        PulpSamplerHeritageStatus::PrepareFailed;
    std::thread loader, rebind;
    auto cleanup = runtime::make_scope_guard([&] {
        PulpSamplerHeritageTestAccess::pause_file_stage(
            fixture.processor, false);
        if (loader.joinable()) loader.join();
        if (rebind.joinable()) rebind.join();
    });
    loader = std::thread([&] {
        loaded = fixture.processor.load_sample_file_result(source.path);
    });
    REQUIRE(wait_for_heritage_condition([&] {
        return PulpSamplerHeritageTestAccess::file_stage_paused(
            fixture.processor);
    }));
    rebind = std::thread([&] {
        configured = fixture.processor.set_heritage_profile(profile);
    });
    REQUIRE(wait_for_heritage_condition([&] {
        return PulpSamplerHeritageTestAccess::control_heritage_counts(
                   fixture.processor).first == heritage_before.first + 1;
    }));
    REQUIRE(PulpSamplerHeritageTestAccess::control_heritage_counts(
                fixture.processor).second == heritage_before.second);

    PulpSamplerHeritageTestAccess::pause_file_stage(fixture.processor, false);
    loader.join();
    rebind.join();
    REQUIRE(loaded.loaded());
    REQUIRE(configured == PulpSamplerHeritageStatus::Ready);
    REQUIRE(fixture.processor.has_sample());
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                fixture.processor) == 96000.0);
    const auto snapshot = fixture.processor.diagnostics();
    REQUIRE(snapshot.last_load.loaded());
    REQUIRE(snapshot.last_load.selection_generation != 0);
    REQUIRE(snapshot.last_load.selection_generation ==
            snapshot.preload.selection_generation);
    cleanup.dismiss();
}

TEST_CASE("PulpSampler rejects streamed pitch times heritage clock above four",
          "[audio][sampler][heritage][stream][admission]") {
    HeritageTempWav source("pitch_cap", 2000000);
    const auto profile = clock_profile(2.0);
    HeritageFixture fixture(64, &profile);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    REQUIRE(fixture.processor.load_sample_file(source.path));
    constexpr std::array block{std::size_t{64}};

    (void) render(fixture, block, 0, 72);
    REQUIRE(PulpSamplerHeritageTestAccess::active_streamed_voices(
                fixture.processor) == 1);
    REQUIRE(fixture.processor.stream_stats()
                .invalid_preload_contract_events == 0);
    (void) render(fixture, block, 0, 73);
    REQUIRE(PulpSamplerHeritageTestAccess::active_streamed_voices(
                fixture.processor) == 1);
    const auto heritage = fixture.processor.heritage_diagnostics();
    REQUIRE(heritage.rate_admission_rejections == 1);
    REQUIRE(fixture.processor.stream_stats()
                .aggregate_rate_admission_rejections == 0);

    const auto slow_profile = clock_profile(0.5);
    HeritageFixture slow(64, &slow_profile);
    slow.store.set_value(kSamplerLoop, 1.0f);
    REQUIRE(slow.processor.load_sample_file(source.path));
    (void) render(slow, block, 0, 96);
    for (int callback = 0; callback < 16; ++callback)
        (void) render(slow, block, 65, 96);
    REQUIRE(PulpSamplerHeritageTestAccess::active_streamed_voices(
                slow.processor) == 1);
    REQUIRE(slow.processor.heritage_diagnostics()
                .rate_admission_rejections == 0);
    REQUIRE(slow.processor.stream_stats()
                .invalid_preload_contract_events == 0);
    REQUIRE(PulpSamplerHeritageTestAccess::last_lookahead_demand_fps(
                slow.processor) == 192000.0);
}

TEST_CASE("PulpSampler prepared state recall rebinds without dropping its source",
          "[audio][sampler][heritage][state][stream][configuration]") {
    HeritageTempWav source("state_domain_rebind");
    const auto initial_profile = clock_profile(2.0);
    HeritageFixture fixture(64, &initial_profile);
    REQUIRE(fixture.processor.load_sample_file(source.path));
    const auto source_frames = fixture.processor.sample_length();
    std::error_code remove_error;
    REQUIRE(std::filesystem::remove(source.path, remove_error));
    REQUIRE_FALSE(remove_error);
    REQUIRE(PulpSamplerHeritageTestAccess::has_retained_streamed_source(
        fixture.processor));

    const auto restored_profile = typed_voice_profile(1.25);
    HeritageFixture saved(64, &restored_profile);
    const auto state = saved.processor.serialize_plugin_state();
    REQUIRE_FALSE(state.empty());
    REQUIRE(fixture.processor.deserialize_plugin_state(state));
    REQUIRE(fixture.processor.has_sample());
    REQUIRE(fixture.processor.sample_length() == source_frames);
    REQUIRE(fixture.processor.heritage_diagnostics().profile() ==
            restored_profile.profile_id);
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                fixture.processor) == 60000.0);
    constexpr std::array block{std::size_t{64}};
    const auto output = render(fixture, block);
    REQUIRE(std::any_of(output.begin(), output.end(), [](float value) {
        return std::abs(value) > 0.001f;
    }));

    REQUIRE(fixture.processor.deserialize_plugin_state({}));
    REQUIRE(fixture.processor.has_sample());
    REQUIRE(fixture.processor.sample_length() == source_frames);
    REQUIRE(fixture.processor.heritage_diagnostics().status ==
            PulpSamplerHeritageStatus::Disabled);
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                fixture.processor) == 48000.0);

    auto invalid = clock_profile(0.75);
    invalid.schema_version = audio::kSampleHeritageProfileSchemaVersion + 1;
    REQUIRE(fixture.processor.set_heritage_profile(invalid) ==
            PulpSamplerHeritageStatus::InvalidProfile);
    REQUIRE(fixture.processor.has_sample());
    REQUIRE(fixture.processor.sample_length() == source_frames);
    fixture.processor.release();
    REQUIRE_FALSE(PulpSamplerHeritageTestAccess::has_retained_streamed_source(
        fixture.processor));
}

TEST_CASE("PulpSampler multiplies streamed source throughput by heritage clock",
          "[audio][sampler][heritage][stream][admission]") {
    HeritageTempWav source("aggregate_clock");
    const auto profile = clock_profile(2.0);
    HeritageFixture fixture(64, &profile);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    REQUIRE(fixture.processor.load_sample_file(source.path));
    PulpSamplerHeritageTestAccess::force_stream_rate_capacity(
        fixture.processor, 60000.0);
    constexpr std::array block{std::size_t{64}};
    (void) render(fixture, block);

    REQUIRE(PulpSamplerHeritageTestAccess::active_streamed_voices(
                fixture.processor) == 0);
    const auto heritage = fixture.processor.heritage_diagnostics();
    REQUIRE(heritage.rate_admission_rejections == 1);
    REQUIRE(fixture.processor.stream_stats()
                .aggregate_rate_admission_rejections == 0);

    const auto bypass = clock_profile(2.0, true);
    HeritageFixture neutral(64, &bypass);
    neutral.store.set_value(kSamplerLoop, 1.0f);
    REQUIRE(neutral.processor.load_sample_file(source.path));
    PulpSamplerHeritageTestAccess::force_stream_rate_capacity(
        neutral.processor, 60000.0);
    (void) render(neutral, block);
    REQUIRE(PulpSamplerHeritageTestAccess::active_streamed_voices(
                neutral.processor) == 1);
    REQUIRE(neutral.processor.heritage_diagnostics()
                .rate_admission_rejections == 0);
}

TEST_CASE("PulpSampler counts clock-driven streamed automation separately",
          "[audio][sampler][heritage][stream][automation]") {
    HeritageTempWav source("automation_clock");
    const auto profile = clock_profile(2.0);
    HeritageFixture fixture(64, &profile);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    REQUIRE(fixture.processor.load_sample_file(source.path));
    constexpr std::array block{std::size_t{64}};
    (void) render(fixture, block);
    REQUIRE(PulpSamplerHeritageTestAccess::active_streamed_voices(
                fixture.processor) == 1);

    fixture.store.set_value(kSamplerPitch, 13.0f);
    (void) render(fixture, block, 65);
    REQUIRE(PulpSamplerHeritageTestAccess::active_streamed_voices(
                fixture.processor) == 0);
    const auto heritage = fixture.processor.heritage_diagnostics();
    REQUIRE(heritage.rate_automation_rejections == 1);
    REQUIRE(heritage.rate_admission_rejections == 0);
    REQUIRE(fixture.processor.stream_stats()
                .aggregate_rate_automation_rejections == 0);
}

TEST_CASE("PulpSampler pins heritage aggregate automation to the safe rate",
          "[audio][sampler][heritage][stream][automation][capacity]") {
    HeritageTempWav source("automation_aggregate_clock");
    const auto profile = clock_profile(2.0);
    HeritageFixture fixture(64, &profile);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    REQUIRE(fixture.processor.load_sample_file(source.path));
    PulpSamplerHeritageTestAccess::force_stream_rate_capacity(
        fixture.processor, 210000.0);
    constexpr std::array attack{std::size_t{64}};
    (void) render(fixture, attack);
    (void) render(fixture, attack);
    for (int callback = 0; callback < 16; ++callback)
        (void) render(fixture, attack, 65);
    REQUIRE(PulpSamplerHeritageTestAccess::active_streamed_voices(
                fixture.processor) == 2);
    const auto before = PulpSamplerHeritageTestAccess::active_streamed_position(
        fixture.processor);

    // Pitch +5 remains below the effective pitch cap, but its clock-multiplied
    // source throughput exceeds the forced aggregate certificate.
    fixture.store.set_value(kSamplerPitch, 5.0f);
    constexpr std::array fade_head{std::size_t{16}};
    (void) render(fixture, fade_head, 17);
    const auto after = PulpSamplerHeritageTestAccess::active_streamed_position(
        fixture.processor);
    REQUIRE(after - before >= 31.0);
    REQUIRE(after - before <= 33.0);
    REQUIRE(fixture.processor.heritage_diagnostics()
                .rate_automation_rejections == 2);
    REQUIRE(fixture.processor.stream_stats()
                .aggregate_rate_automation_rejections == 0);
    REQUIRE(PulpSamplerHeritageTestAccess::last_lookahead_demand_fps(
                fixture.processor) == 96000.0);
    REQUIRE(PulpSamplerHeritageTestAccess::active_streamed_voices(
                fixture.processor) == 2);

    // A later note must count both still-reading fade voices at their pinned
    // rates. Skipping them would incorrectly admit this third voice.
    constexpr std::array fade_tail{std::size_t{16}};
    (void) render(fixture, fade_tail);
    REQUIRE(PulpSamplerHeritageTestAccess::active_streamed_voices(
                fixture.processor) == 0);
    REQUIRE(fixture.processor.heritage_diagnostics()
                .rate_admission_rejections == 1);
}

TEST_CASE("PulpSampler pins legacy aggregate automation to the safe rate",
          "[audio][sampler][heritage][stream][automation][capacity]") {
    HeritageTempWav source("automation_aggregate_legacy");
    const auto profile = clock_profile(2.0);
    HeritageFixture fixture(64, &profile);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    REQUIRE(fixture.processor.load_sample_file(source.path));
    constexpr std::array attack{std::size_t{64}};
    (void) render(fixture, attack);
    for (int callback = 0; callback < 16; ++callback)
        (void) render(fixture, attack, 65);
    PulpSamplerHeritageTestAccess::force_stream_rate_capacity(
        fixture.processor, 60000.0);
    const auto before = PulpSamplerHeritageTestAccess::active_streamed_position(
        fixture.processor);

    fixture.store.set_value(kSamplerPitch, 5.0f);
    constexpr std::array fade_head{std::size_t{16}};
    (void) render(fixture, fade_head, 17);
    const auto after = PulpSamplerHeritageTestAccess::active_streamed_position(
        fixture.processor);
    REQUIRE(after - before >= 31.0);
    REQUIRE(after - before <= 33.0);
    REQUIRE(fixture.processor.heritage_diagnostics()
                .rate_automation_rejections == 0);
    REQUIRE(fixture.processor.stream_stats()
                .aggregate_rate_automation_rejections == 1);
    REQUIRE(PulpSamplerHeritageTestAccess::last_lookahead_demand_fps(
                fixture.processor) == 96000.0);
}
