#include "test_pulp_sampler_heritage_support.hpp"

TEST_CASE("PulpSampler rejects heritage replacement without disturbing runtime",
          "[audio][sampler][heritage][configuration]") {
    const auto profile = clock_profile(2.0);
    auto sample = make_sine(48000);
    HeritageFixture fixture(64, &profile);
    fixture.load(sample);
    constexpr std::array attack{std::size_t{64}};
    (void) render(fixture, attack);
    (void) fixture.processor.consume_latency_changed_flag();

    auto invalid = clock_profile(1.25);
    invalid.schema_version = audio::kSampleHeritageProfileSchemaVersion + 1;
    REQUIRE(fixture.processor.set_heritage_profile(invalid) ==
            PulpSamplerHeritageStatus::InvalidProfile);
    REQUIRE(fixture.processor.latency_samples() == 12);
    REQUIRE_FALSE(fixture.processor.consume_latency_changed_flag());
    const auto diagnostics = fixture.processor.heritage_diagnostics();
    REQUIRE(diagnostics.status == PulpSamplerHeritageStatus::Ready);
    REQUIRE(diagnostics.profile() == profile.profile_id);

    constexpr std::array continuation{std::size_t{64}};
    const auto output = render(fixture, continuation, 65);
    REQUIRE(std::any_of(output.begin(), output.end(), [](float value) {
        return std::abs(value) > 0.01f;
    }));

    const auto replacement = clock_profile(1.25);
    REQUIRE(fixture.processor.set_heritage_profile(replacement) ==
            PulpSamplerHeritageStatus::Ready);
    REQUIRE(fixture.processor.has_sample());
    REQUIRE(fixture.processor.sample_length() ==
            static_cast<int>(sample.size()));
    REQUIRE(PulpSamplerHeritageTestAccess::stream_output_sample_rate(
                fixture.processor) == 60000.0);
    const auto rebound_output = render(fixture, continuation);
    REQUIRE(std::any_of(rebound_output.begin(), rebound_output.end(),
                        [](float value) {
                            return std::abs(value) > 0.01f;
                        }));
}

TEST_CASE("PulpSampler notifies host only when heritage latency changes",
          "[audio][sampler][heritage][latency]") {
    HeritageFixture fixture(64);
    REQUIRE_FALSE(fixture.processor.consume_latency_changed_flag());
    const auto active = clock_profile(2.0);
    REQUIRE(fixture.processor.set_heritage_profile(active) ==
            PulpSamplerHeritageStatus::Ready);
    REQUIRE(fixture.processor.consume_latency_changed_flag());
    REQUIRE(fixture.processor.latency_samples() == 12);

    REQUIRE(fixture.processor.set_heritage_profile(active) ==
            PulpSamplerHeritageStatus::Ready);
    REQUIRE_FALSE(fixture.processor.consume_latency_changed_flag());
    REQUIRE(fixture.processor.disable_heritage() ==
            PulpSamplerHeritageStatus::Disabled);
    REQUIRE(fixture.processor.consume_latency_changed_flag());
    REQUIRE(fixture.processor.latency_samples() == 0);
}

TEST_CASE("PulpSampler rejects heritage output above fixed channel capacity",
          "[audio][sampler][heritage][bounds]") {
    const auto profile = clock_profile(2.0);
    auto sample = make_sine(48000);
    HeritageFixture fixture(16, &profile);
    fixture.load(sample);
    std::array<std::array<float, 16>, 9> channels{};
    std::array<float*, 9> output_ptrs{};
    for (std::size_t channel = 0; channel < channels.size(); ++channel)
        output_ptrs[channel] = channels[channel].data();
    const float* input_ptrs[]{nullptr, nullptr};
    audio::BufferView<float> output(output_ptrs.data(), output_ptrs.size(), 16);
    audio::BufferView<const float> input(input_ptrs, 0, 16);
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    format::ProcessContext context{48000.0, 16};
    fixture.processor.process(output, input, midi_in, midi_out, context);
    for (const auto& channel : channels)
        REQUIRE(std::all_of(channel.begin(), channel.end(),
                            [](float value) { return value == 0.0f; }));
    const auto diagnostics = fixture.processor.heritage_diagnostics();
    REQUIRE(diagnostics.status == PulpSamplerHeritageStatus::RenderFailed);
    REQUIRE(diagnostics.render_failures == 1);
}

TEST_CASE("PulpSampler heritage render-plan failure latches silence",
          "[audio][sampler][heritage][failure]") {
    const auto profile = clock_profile(2.0);
    auto sample = make_sine(48000);
    HeritageFixture fixture(64, &profile);
    fixture.load(sample);
    PulpSamplerHeritageTestAccess::fail_next_plan(fixture.processor);
    constexpr std::array blocks{std::size_t{64}, std::size_t{64}};
    const auto output = render(fixture, blocks);
    REQUIRE(std::all_of(output.begin(), output.end(),
                        [](float sample_value) { return sample_value == 0.0f; }));
    const auto diagnostics = fixture.processor.heritage_diagnostics();
    REQUIRE(diagnostics.status == PulpSamplerHeritageStatus::RenderPlanFailed);
    REQUIRE(diagnostics.render_plan_failures == 1);
}

TEST_CASE("PulpSampler outer state round-trip resumes heritage RNG",
          "[audio][sampler][heritage][state]") {
    const auto profile = continued_noise_profile();
    HeritageFixture source(64, &profile);
    source.store.set_value(kSamplerGain, -9.0f);
    constexpr std::array block{std::size_t{64}};
    (void) render(source, block);

    const auto before = parse_sampler_heritage_state(
        source.processor.serialize_plugin_state());
    REQUIRE(before.valid());
    REQUIRE(before.state.has_runtime_state);
    REQUIRE(before.state.runtime_state.rng_state_count == 1);
    const auto saved_rng =
        before.state.runtime_state.rng_states[0].random_state;

    const auto envelope =
        format::plugin_state_io::serialize(source.store, source.processor);
    HeritageFixture restored(64);
    REQUIRE(format::plugin_state_io::deserialize(
        envelope, restored.store, restored.processor));
    REQUIRE(restored.store.get_value(kSamplerGain) == -9.0f);
    const auto diagnostics = restored.processor.heritage_diagnostics();
    REQUIRE(diagnostics.status == PulpSamplerHeritageStatus::Ready);
    REQUIRE(diagnostics.runtime_state_status ==
            audio::SampleHeritageRuntimeStateStatus::Ok);

    // The restored snapshot remains serializable before the first callback.
    const auto immediate = parse_sampler_heritage_state(
        restored.processor.serialize_plugin_state());
    REQUIRE(immediate.valid());
    REQUIRE(immediate.state.has_runtime_state);
    REQUIRE(immediate.state.runtime_state.rng_states[0].random_state ==
            saved_rng);

    // Callback-end publication atomically replaces it with the advanced RNG.
    (void) render(restored, block);
    const auto advanced = parse_sampler_heritage_state(
        restored.processor.serialize_plugin_state());
    REQUIRE(advanced.valid());
    REQUIRE(advanced.state.has_runtime_state);
    REQUIRE(advanced.state.runtime_state.rng_states[0].random_state !=
            saved_rng);
}

TEST_CASE("PulpSampler state restored before prepare reaches first callback",
          "[audio][sampler][heritage][state]") {
    const auto profile = continued_noise_profile();
    HeritageFixture source(64, &profile);
    constexpr std::array block{std::size_t{64}};
    (void) render(source, block);
    const auto saved = parse_sampler_heritage_state(
        source.processor.serialize_plugin_state());
    REQUIRE(saved.valid());
    REQUIRE(saved.state.has_runtime_state);

    state::StateStore store;
    PulpSamplerProcessor restored;
    restored.set_state_store(&store);
    restored.define_parameters(store);
    REQUIRE(restored.deserialize_plugin_state(
        source.processor.serialize_plugin_state()));
    format::PrepareContext context;
    context.sample_rate = 48000.0;
    context.max_buffer_size = 64;
    context.input_channels = 0;
    context.output_channels = 2;
    restored.prepare(context);

    const auto diagnostics = restored.heritage_diagnostics();
    REQUIRE(diagnostics.status == PulpSamplerHeritageStatus::Ready);
    REQUIRE(diagnostics.runtime_state_status ==
            audio::SampleHeritageRuntimeStateStatus::Ok);
    const auto immediate = parse_sampler_heritage_state(
        restored.serialize_plugin_state());
    REQUIRE(immediate.valid());
    REQUIRE(immediate.state.has_runtime_state);
    REQUIRE(immediate.state.runtime_state.rng_states[0].random_state ==
            saved.state.runtime_state.rng_states[0].random_state);
}

TEST_CASE("PulpSampler downstream prepare failure preserves pending RNG for retry",
          "[audio][sampler][heritage][state][failure]") {
    const auto profile = continued_noise_profile();
    HeritageFixture source(64, &profile);
    constexpr std::array block{std::size_t{64}};
    (void) render(source, block);
    const auto saved_bytes = source.processor.serialize_plugin_state();
    const auto saved = parse_sampler_heritage_state(saved_bytes);
    REQUIRE(saved.valid());
    REQUIRE(saved.state.has_runtime_state);

    auto initialize = [&](PulpSamplerProcessor& processor,
                          state::StateStore& store) {
        processor.set_state_store(&store);
        processor.define_parameters(store);
        REQUIRE(processor.deserialize_plugin_state(saved_bytes));
    };
    auto prepare = [](PulpSamplerProcessor& processor) {
        format::PrepareContext context;
        context.sample_rate = 48000.0;
        context.max_buffer_size = 64;
        context.input_channels = 0;
        context.output_channels = 2;
        processor.prepare(context);
    };
    auto advance = [](PulpSamplerProcessor& processor) {
        std::array<float, 64> left{}, right{};
        float* outputs[]{left.data(), right.data()};
        const float* inputs[]{nullptr, nullptr};
        audio::BufferView<float> output(outputs, 2, left.size());
        audio::BufferView<const float> input(inputs, 0, left.size());
        midi::MidiBuffer midi_in, midi_out;
        format::ProcessContext context{48000.0, 64};
        processor.process(output, input, midi_in, midi_out, context);
    };

    state::StateStore retry_store;
    PulpSamplerProcessor retry;
    initialize(retry, retry_store);
    PulpSamplerHeritageTestAccess::fail_next_stream_domain_prepare(retry);
    prepare(retry);
    REQUIRE(retry.prepare_result().status ==
            PulpSamplerPrepareStatus::AllocationFailure);
    auto after_failure = parse_sampler_heritage_state(
        retry.serialize_plugin_state());
    REQUIRE(after_failure.valid());
    REQUIRE(after_failure.state.has_runtime_state);
    REQUIRE(after_failure.state.runtime_state.rng_states[0].random_state ==
            saved.state.runtime_state.rng_states[0].random_state);

    prepare(retry);
    REQUIRE(retry.prepare_result().prepared());
    const auto immediate = parse_sampler_heritage_state(
        retry.serialize_plugin_state());
    REQUIRE(immediate.state.runtime_state.rng_states[0].random_state ==
            saved.state.runtime_state.rng_states[0].random_state);
    advance(retry);
    const auto retry_advanced = parse_sampler_heritage_state(
        retry.serialize_plugin_state());

    state::StateStore direct_store;
    PulpSamplerProcessor direct;
    initialize(direct, direct_store);
    prepare(direct);
    advance(direct);
    const auto direct_advanced = parse_sampler_heritage_state(
        direct.serialize_plugin_state());
    REQUIRE(retry_advanced.state.runtime_state.rng_states[0].random_state ==
            direct_advanced.state.runtime_state.rng_states[0].random_state);
    REQUIRE(retry_advanced.state.runtime_state.rng_states[0].random_state !=
            saved.state.runtime_state.rng_states[0].random_state);
}

TEST_CASE("PulpSampler outer state resets RNG when host rate changes",
          "[audio][sampler][heritage][state]") {
    const auto profile = continued_noise_profile();
    HeritageFixture source(64, &profile);
    constexpr std::array block{std::size_t{64}};
    (void) render(source, block);
    const auto envelope =
        format::plugin_state_io::serialize(source.store, source.processor);

    HeritageFixture restored(64, nullptr, 44100.0);
    REQUIRE(format::plugin_state_io::deserialize(
        envelope, restored.store, restored.processor));
    const auto diagnostics = restored.processor.heritage_diagnostics();
    REQUIRE(diagnostics.status ==
            PulpSamplerHeritageStatus::ReadyRuntimeResetForHostRate);
    REQUIRE(diagnostics.runtime_state_status ==
            audio::SampleHeritageRuntimeStateStatus::NotPrepared);

    const auto reset = parse_sampler_heritage_state(
        restored.processor.serialize_plugin_state());
    REQUIRE(reset.valid());
    REQUIRE(reset.state.enabled);
    REQUIRE_FALSE(reset.state.has_runtime_state);
    REQUIRE(reset.state.profile.host_sample_rate == 48000.0);

    // Once the 44.1 kHz runtime advances and is saved, another 44.1 kHz
    // restore resumes that execution state even though the profile was authored
    // at 48 kHz.
    (void) render(restored, block);
    const auto at_44100 = format::plugin_state_io::serialize(
        restored.store, restored.processor);
    const auto saved_at_44100 = parse_sampler_heritage_state(
        restored.processor.serialize_plugin_state());
    REQUIRE(saved_at_44100.valid());
    REQUIRE(saved_at_44100.state.has_runtime_state);
    REQUIRE(saved_at_44100.state.runtime_host_sample_rate == 44100.0);

    auto legacy_v1_at_44100 = restored.processor.serialize_plugin_state();
    legacy_v1_at_44100.erase(
        legacy_v1_at_44100.begin() + kSamplerHeritageStateV1HeaderBytes,
        legacy_v1_at_44100.begin() + kSamplerHeritageStateHeaderBytes);
    legacy_v1_at_44100[4] = 1;
    HeritageFixture legacy_same_rate(64, nullptr, 44100.0);
    REQUIRE(legacy_same_rate.processor.deserialize_plugin_state(
        legacy_v1_at_44100));
    REQUIRE(legacy_same_rate.processor.heritage_diagnostics().status ==
            PulpSamplerHeritageStatus::ReadyRuntimeResetForHostRate);
    REQUIRE(legacy_same_rate.processor.heritage_diagnostics()
                .runtime_state_status ==
            audio::SampleHeritageRuntimeStateStatus::NotPrepared);
    REQUIRE_FALSE(parse_sampler_heritage_state(
        legacy_same_rate.processor.serialize_plugin_state())
                      .state.has_runtime_state);

    HeritageFixture same_rate(64, nullptr, 44100.0);
    REQUIRE(format::plugin_state_io::deserialize(
        at_44100, same_rate.store, same_rate.processor));
    REQUIRE(same_rate.processor.heritage_diagnostics().status ==
            PulpSamplerHeritageStatus::Ready);
    REQUIRE(same_rate.processor.heritage_diagnostics().runtime_state_status ==
            audio::SampleHeritageRuntimeStateStatus::Ok);
    const auto resumed = parse_sampler_heritage_state(
        same_rate.processor.serialize_plugin_state());
    REQUIRE(resumed.valid());
    REQUIRE(resumed.state.has_runtime_state);
    REQUIRE(resumed.state.runtime_state.rng_states[0].random_state ==
            saved_at_44100.state.runtime_state.rng_states[0].random_state);

    HeritageFixture changed_again(64, nullptr, 48000.0);
    REQUIRE(format::plugin_state_io::deserialize(
        at_44100, changed_again.store, changed_again.processor));
    REQUIRE(changed_again.processor.heritage_diagnostics().status ==
            PulpSamplerHeritageStatus::ReadyRuntimeResetForHostRate);
    REQUIRE_FALSE(parse_sampler_heritage_state(
        changed_again.processor.serialize_plugin_state()).state.has_runtime_state);
}

TEST_CASE("Prepared PulpSampler heritage callbacks allocate nothing",
          "[audio][sampler][heritage][rt]") {
    const auto profile = clock_profile(1.25);
    auto sample = make_sine(48000);
    HeritageFixture fixture(16, &profile);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    fixture.load(sample);
    constexpr std::array attack{std::size_t{16}};
    (void) render(fixture, attack);

    std::array<float, 16> left{};
    std::array<float, 16> right{};
    float* output_ptrs[]{left.data(), right.data()};
    const float* input_ptrs[]{nullptr, nullptr};
    audio::BufferView<float> output(output_ptrs, 2, left.size());
    audio::BufferView<const float> input(input_ptrs, 0, left.size());
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    format::ProcessContext context{48000.0, static_cast<int>(left.size())};
    pulp::test::RtAllocationProbe probe;
    for (int callback = 0; callback < 10000; ++callback)
        fixture.processor.process(output, input, midi_in, midi_out, context);
    REQUIRE_FALSE(probe.saw_allocation());
}
