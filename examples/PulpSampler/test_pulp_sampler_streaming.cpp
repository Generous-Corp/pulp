#include "test_pulp_sampler_support.hpp"

TEST_CASE("PulpSampler fails closed when selection generations are exhausted",
          "[sampler][stream][generation][transaction]") {
    SECTION("resident publication") {
        SamplerFixture fixture;
        const std::array<float, 4> samples{0.25f, 0.5f, -0.25f, -0.5f};
        PulpSamplerTestAccess::exhaust_selection_generation(*fixture.proc);

        REQUIRE_FALSE(fixture.proc->load_sample(samples.data(), samples.size(), 48000.0f));
        REQUIRE_FALSE(fixture.proc->has_sample());
        REQUIRE(PulpSamplerTestAccess::published_selection_generation(*fixture.proc) == 0);
    }

    SECTION("streamed publication") {
        TempSamplerWav first("generation_exhaustion_first", 24000, 0.25f);
        TempSamplerWav second("generation_exhaustion_second", 24000, 0.5f);
        SamplerFixture fixture;
        REQUIRE(fixture.proc->load_sample_file(first.path));
        const auto retained_source =
            PulpSamplerTestAccess::published_stream_source(*fixture.proc);
        const auto retained_generation =
            PulpSamplerTestAccess::published_selection_generation(*fixture.proc);
        PulpSamplerTestAccess::exhaust_selection_generation(*fixture.proc);

        const auto rejected = fixture.proc->load_sample_file_result(second.path);
        REQUIRE(rejected.status == PulpSamplerLoadStatus::GenerationExhausted);
        REQUIRE(PulpSamplerTestAccess::published_stream_source(*fixture.proc).source_id ==
                retained_source.source_id);
        REQUIRE(PulpSamplerTestAccess::published_selection_generation(*fixture.proc) ==
                retained_generation);
        REQUIRE(PulpSamplerTestAccess::physical_stream_source_count(*fixture.proc) == 1);
    }
}

TEST_CASE("PulpSampler streams continuously across the preload boundary", "[sampler][stream]") {
    TempSamplerWav wav("boundary", 24000, 0.5f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 5.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));

    const auto preload = fixture.proc->stream_stats().preload_frames;
    REQUIRE(preload > 0);
    REQUIRE(preload < 24000);

    SamplerProcessBlock block;
    const auto pages_before_note = fixture.proc->stream_stats().pages_published;
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    std::vector<float> rendered = block.left;
    block.midi_in.clear();
    block.run(*fixture.proc);
    rendered.insert(rendered.end(), block.left.begin(), block.left.end());
    block.run(*fixture.proc);
    rendered.insert(rendered.end(), block.left.begin(), block.left.end());
    REQUIRE(wait_for_condition(
        [&] { return fixture.proc->stream_stats().pages_published > pages_before_note; }));

    while (rendered.size() < preload + 32) {
        block.run(*fixture.proc);
        rendered.insert(rendered.end(), block.left.begin(), block.left.end());
    }

    REQUIRE(fixture.proc->stream_stats().starved_output_frames == 0);
    for (std::uint64_t frame = preload - 8; frame < preload + 8; ++frame) {
        REQUIRE_THAT(rendered[static_cast<std::size_t>(frame)], WithinAbs(0.5f, 1e-6));
    }
}

TEST_CASE("PulpSampler reports decode-once FLAC rejection without replacing its source",
          "[sampler][stream][codec][transaction]") {
    TempSamplerWav published("codec_fallback_published", 24000, 0.375f);
    TempSamplerFlac fallback("codec_fallback_rejected");
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    REQUIRE(fixture.proc->load_sample_file(published.path));

    const auto source_before =
        PulpSamplerTestAccess::published_stream_source(*fixture.proc);
    const auto selection_before =
        PulpSamplerTestAccess::published_selection_generation(*fixture.proc);
    REQUIRE(source_before.source_id != 0);
    REQUIRE(source_before.source_generation != 0);

    const auto rejected = fixture.proc->load_sample_file_result(fallback.path);
    REQUIRE(rejected.status == PulpSamplerLoadStatus::DecodeOnceFallbackRejected);
    REQUIRE(rejected.codec_capability ==
            PulpSamplerCodecCapability::DecodeOnceFallback);
    REQUIRE(rejected.codec() == "FLAC");
    REQUIRE(rejected.channels == 1);
    REQUIRE(rejected.sample_rate == 48000);
    REQUIRE(rejected.total_frames == 32);

    const auto source_after =
        PulpSamplerTestAccess::published_stream_source(*fixture.proc);
    REQUIRE(source_after.source_id == source_before.source_id);
    REQUIRE(source_after.source_generation == source_before.source_generation);
    REQUIRE(PulpSamplerTestAccess::published_selection_generation(*fixture.proc) ==
            selection_before);
    REQUIRE(fixture.proc->has_sample());
    REQUIRE(fixture.proc->sample_length() == 24000);

    SamplerProcessBlock block(64);
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    REQUIRE(std::all_of(block.left.begin(), block.left.end(), [](float sample) {
        return std::abs(sample - 0.375f) <= 1.0e-6f;
    }));
    REQUIRE(block.left == block.right);
    REQUIRE(fixture.proc->stream_stats().starved_output_frames == 0);
}

TEST_CASE("PulpSampler resident and recovered streamed polyphony null through voice steals",
          "[sampler][stream][parity][steal][recovery]") {
    constexpr std::uint64_t source_frames = 48000;
    const auto source = sampler_parity::make_deterministic_source(1, source_frames);
    const std::vector<float> mono(source.channel(0).begin(), source.channel(0).end());
    TempSamplerWav streamed_file("production_steal_parity", mono);

    SamplerFixture resident_fixture;
    SamplerFixture streamed_fixture;
    configure_parity_sampler(resident_fixture);
    configure_parity_sampler(streamed_fixture);
    REQUIRE(resident_fixture.proc->load_sample(
        mono.data(), static_cast<int>(mono.size()), 44100.0f));
    REQUIRE(streamed_fixture.proc->load_sample_file(streamed_file.path));
    const auto preload = streamed_fixture.proc->stream_stats().preload_frames;
    REQUIRE(preload > 0);
    REQUIRE(preload < source_frames);

    // Force a real service starvation, then prove the production voice reader
    // returns to Ready before using the recovered source for the null render.
    PulpSamplerTestAccess::pause_stream_dispatch(*streamed_fixture.proc, true);
    SamplerProcessBlock recovery_block(257);
    recovery_block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    recovery_block.run(*streamed_fixture.proc);
    recovery_block.midi_in.clear();
    for (std::uint32_t callback = 0;
         callback < 256 &&
         streamed_fixture.proc->stream_stats().starved_output_frames == 0;
         ++callback) {
        recovery_block.run(*streamed_fixture.proc);
    }
    REQUIRE(streamed_fixture.proc->stream_stats().starved_output_frames > 0);
    REQUIRE(PulpSamplerTestAccess::active_stream_starvation_stats(
                *streamed_fixture.proc).starved_frames > 0);

    PulpSamplerTestAccess::pause_stream_dispatch(*streamed_fixture.proc, false);
    REQUIRE(wait_for_condition(
        [&] {
            recovery_block.run(*streamed_fixture.proc);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const auto stats = PulpSamplerTestAccess::active_stream_starvation_stats(
                *streamed_fixture.proc);
            return stats.recovery_events > 0 &&
                   PulpSamplerTestAccess::active_stream_starvation_mode(
                       *streamed_fixture.proc) ==
                       audio::SampleStarvationMode::Ready &&
                   std::any_of(recovery_block.left.begin(), recovery_block.left.end(),
                               [](float sample) { return sample != 0.0f; });
        },
        std::chrono::seconds(5)));
    const auto recovered_envelope =
        PulpSamplerTestAccess::active_stream_starvation_stats(*streamed_fixture.proc);
    REQUIRE(recovered_envelope.starved_frames > 0);
    REQUIRE(recovered_envelope.recovery_events > 0);
    REQUIRE(PulpSamplerTestAccess::active_stream_starvation_mode(
                *streamed_fixture.proc) == audio::SampleStarvationMode::Ready);

    PulpSamplerTestAccess::reset_voices(*streamed_fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_voice_count(*streamed_fixture.proc) == 0);
    REQUIRE(wait_for_condition(
        [&] {
            SamplerProcessBlock drain(1);
            drain.run(*streamed_fixture.proc);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return PulpSamplerTestAccess::stream_command_count(
                       *streamed_fixture.proc) == 0;
        }));
    const auto streamed_generations_before =
        PulpSamplerTestAccess::requester_generations(*streamed_fixture.proc);
    const auto starvation_before_parity =
        streamed_fixture.proc->stream_stats().starved_output_frames;

    constexpr std::array<std::uint32_t, 9> partition = {
        31, 127, 64, 257, 19, 509, 73, 211, 43,
    };
    const auto capture_frames = preload + 4096;
    REQUIRE(capture_frames < source_frames);
    const auto resident = render_production_sampler_schedule(
        *resident_fixture.proc, capture_frames, partition, false);
    const auto streamed = render_production_sampler_schedule(
        *streamed_fixture.proc, capture_frames, partition, true);

    REQUIRE(PulpSamplerTestAccess::active_voice_count(*resident_fixture.proc) ==
            PulpSamplerProcessor::kMaxVoices);
    REQUIRE(PulpSamplerTestAccess::active_voice_count(*streamed_fixture.proc) ==
            PulpSamplerProcessor::kMaxVoices);
    std::uint64_t admitted_streamed_starts = 0;
    for (std::size_t index = 0; index < streamed.requester_generations.size(); ++index) {
        const auto starts = streamed.requester_generations[index] -
                            streamed_generations_before[index];
        // Every prepared slot was occupied. The four starts beyond that full
        // eight-voice set therefore required four production steal decisions.
        REQUIRE(starts >= 1);
        admitted_streamed_starts += starts;
        REQUIRE(resident.final_positions[index] >= 0.0);
        REQUIRE(streamed.final_positions[index] == resident.final_positions[index]);
    }
    REQUIRE(admitted_streamed_starts == 12);
    REQUIRE(admitted_streamed_starts - PulpSamplerProcessor::kMaxVoices == 4);
    // Slot zero was replaced after the voice set filled, so its younger cursor
    // must trail an untouched voice. This independently proves the schedule
    // did not merely submit twelve rejected note-ons.
    REQUIRE(resident.final_positions[0] < resident.final_positions[4]);
    REQUIRE(streamed.final_positions[0] < streamed.final_positions[4]);

    const auto comparison = sampler_parity::compare_raw_float_bits(
        resident.output, streamed.output);
    REQUIRE(comparison.equal_nonvacuous());
    float maximum_residual = 0.0f;
    double residual_energy = 0.0;
    for (std::size_t channel = 0; channel < resident.output.num_channels(); ++channel) {
        for (std::size_t frame = 0; frame < resident.output.num_samples(); ++frame) {
            const auto residual = resident.output.channel(channel)[frame] -
                                  streamed.output.channel(channel)[frame];
            maximum_residual = std::max(maximum_residual, std::abs(residual));
            residual_energy += static_cast<double>(residual) * residual;
        }
    }
    REQUIRE(maximum_residual == 0.0f);
    REQUIRE(residual_energy == 0.0);

    REQUIRE(streamed.stream_stats.starved_output_frames ==
            starvation_before_parity);
    REQUIRE(streamed.stream_stats.pages_published > 0);
    REQUIRE(streamed.stream_stats.decode_failure_events == 0);
    REQUIRE(streamed.stream_stats.invalid_preload_contract_events == 0);
    REQUIRE(streamed.stream_stats.stale_generation_events == 0);
    REQUIRE(streamed.stream_stats.normal_end_of_source_events == 0);
    REQUIRE(streamed.stream_stats.invalid_render_contract_events == 0);
}

TEST_CASE("PulpSampler retunes streamed sinc across pitch modulation",
          "[sampler][stream][interpolation][sinc][modulation]") {
    TempSamplerWav wav("sinc_modulation", 24000, 0.5f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 5.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));

    SamplerProcessBlock block(64);
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    auto interpolation = PulpSamplerTestAccess::active_stream_interpolation(*fixture.proc);
    REQUIRE(interpolation.policy == audio::SampleInterpolationPolicy::RatioTrackingSinc);
    REQUIRE(interpolation.sinc.wider.cutoff() == 1.0);

    fixture.store.set_value(kSamplerPitch, 12.0f);
    block.midi_in.clear();
    block.run(*fixture.proc);
    interpolation = PulpSamplerTestAccess::active_stream_interpolation(*fixture.proc);
    REQUIRE(interpolation.sinc.wider.cutoff() == 0.5);
    REQUIRE(interpolation.sinc.narrower.cutoff() == 0.5);

    fixture.store.set_value(kSamplerInterpolation, 1.0f);
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_stream_interpolation(*fixture.proc).policy ==
            audio::SampleInterpolationPolicy::Nearest);
}

namespace {

constexpr double kSamplerModulationSampleRate = 44100.0;
constexpr double kSamplerModulationSourceCycles = 0.20;
constexpr double kSamplerModulationCentreRatio = 1.5;
constexpr double kSamplerModulationDepth = 0.25;
constexpr std::size_t kSamplerModulationFrames = 8820;

double sampler_modulation_ratio(double modulation_hz, std::size_t frame) {
    return kSamplerModulationCentreRatio + kSamplerModulationDepth * std::sin(
        2.0 * std::numbers::pi * modulation_hz * static_cast<double>(frame) /
        kSamplerModulationSampleRate);
}

// Deliberately duplicates the public test specification instead of calling the
// production-stimulus helper above. A mutation of that helper must not move the
// expected trajectory with the rendered candidate.
double sampler_modulation_oracle_ratio(double modulation_hz, std::size_t frame) {
    return 1.5 + 0.25 * std::sin(
        2.0 * std::numbers::pi * modulation_hz * static_cast<double>(frame) /
        44100.0);
}

std::vector<double> sampler_modulation_reference(double modulation_hz,
                                                 std::size_t block_frames,
                                                 bool freeze_modulation = false,
                                                 bool zero_modulation = false) {
    std::vector<double> result(kSamplerModulationFrames);
    double source_position = 0.0;
    for (std::size_t frame = 0; frame < result.size(); ++frame) {
        result[frame] = 0.5 * std::sin(
            2.0 * std::numbers::pi * kSamplerModulationSourceCycles *
            source_position);
        const auto control_frame = (frame / block_frames) * block_frames;
        const auto ratio = zero_modulation
            ? kSamplerModulationCentreRatio
            : freeze_modulation
            ? kSamplerModulationCentreRatio + kSamplerModulationDepth
            : sampler_modulation_oracle_ratio(modulation_hz, control_frame);
        source_position += ratio;
    }
    return result;
}

std::vector<double> render_pulp_sampler_modulation(double modulation_hz,
                                                   std::size_t block_frames) {
    SamplerFixture fixture(static_cast<std::uint32_t>(block_frames),
                           kSamplerModulationSampleRate);
    fixture.store.set_value(kSamplerGain, 0.0f);
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerRelease, 0.0f);
    fixture.store.set_value(kSamplerInterpolation, 5.0f);

    std::vector<float> source(32768);
    for (std::size_t frame = 0; frame < source.size(); ++frame) {
        source[frame] = static_cast<float>(0.5 * std::sin(
            2.0 * std::numbers::pi * kSamplerModulationSourceCycles *
            static_cast<double>(frame)));
    }
    REQUIRE(fixture.proc->load_sample(
        source.data(), static_cast<int>(source.size()),
        static_cast<float>(kSamplerModulationSampleRate)));

    std::vector<double> result;
    result.reserve(kSamplerModulationFrames);
    for (std::size_t start = 0; start < kSamplerModulationFrames;
         start += block_frames) {
        const auto frames = std::min(block_frames,
                                     kSamplerModulationFrames - start);
        const auto ratio = sampler_modulation_ratio(modulation_hz, start);
        fixture.store.set_value(
            kSamplerPitch, static_cast<float>(12.0 * std::log2(ratio)));
        SamplerProcessBlock block(static_cast<std::uint32_t>(frames),
                                  kSamplerModulationSampleRate);
        if (start == 0)
            block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
        block.run(*fixture.proc);
        result.insert(result.end(), block.left.begin(), block.left.end());
    }
    return result;
}

double sampler_modulation_residual_db(std::span<const double> reference,
                                      std::span<const double> candidate) {
    REQUIRE(reference.size() == candidate.size());
    double reference_energy = 0.0;
    double residual_energy = 0.0;
    for (std::size_t index = 0; index < reference.size(); ++index) {
        reference_energy += reference[index] * reference[index];
        const auto residual = candidate[index] - reference[index];
        residual_energy += residual * residual;
    }
    return 10.0 * std::log10(std::max(
        residual_energy / reference_energy, 1.0e-20));
}

}  // namespace

TEST_CASE("PulpSampler sinc follows independent continuous pitch modulation controls",
          "[sampler][interpolation][sinc][modulation][quality]") {
    constexpr std::array<double, 5> modulation_rates = {
        5.0, 20.0, 50.0, 100.0, 200.0,
    };
    constexpr std::array<std::size_t, 3> blocks = {1, 16, 64};
    // Mutation sentinels pin the production-driving trajectory independently
    // of the rendered-audio oracle. These catch a frozen value, wrong depth,
    // wrong waveform phase, or wrong rate scaling before a shared-mode error
    // can masquerade as a good render comparison.
    CHECK_THAT(sampler_modulation_ratio(5.0, 0), WithinAbs(1.5, 1.0e-12));
    CHECK_THAT(sampler_modulation_ratio(5.0, 2205), WithinAbs(1.75, 1.0e-12));
    CHECK_THAT(sampler_modulation_ratio(5.0, 4410), WithinAbs(1.5, 1.0e-12));
    CHECK_THAT(sampler_modulation_ratio(5.0, 6615), WithinAbs(1.25, 1.0e-12));
    CHECK(sampler_modulation_ratio(20.0, 551) > 1.749);
    CHECK(sampler_modulation_ratio(200.0, 55) > 1.749);
    for (const auto modulation_hz : modulation_rates) {
        const auto continuous = sampler_modulation_reference(modulation_hz, 1);
        const auto frozen = sampler_modulation_reference(
            modulation_hz, 1, true, false);
        const auto zero_depth = sampler_modulation_reference(
            modulation_hz, 1, false, true);
        CAPTURE(modulation_hz,
                sampler_modulation_residual_db(continuous, frozen),
                sampler_modulation_residual_db(continuous, zero_depth));
        CHECK(sampler_modulation_residual_db(continuous, frozen) > -20.0);
        CHECK(sampler_modulation_residual_db(continuous, zero_depth) > -20.0);

        std::array<std::vector<double>, blocks.size()> production;
        std::array<double, blocks.size()> continuous_error{};
        for (std::size_t index = 0; index < blocks.size(); ++index) {
            const auto block_frames = blocks[index];
            production[index] = render_pulp_sampler_modulation(
                modulation_hz, block_frames);
            const auto stepped = sampler_modulation_reference(
                modulation_hz, block_frames);
            const auto stepped_error = sampler_modulation_residual_db(
                stepped, production[index]);
            continuous_error[index] = sampler_modulation_residual_db(
                continuous, production[index]);
            CAPTURE(modulation_hz, block_frames, stepped_error,
                    continuous_error[index]);
            // This includes the production StateStore's float semitone control
            // and float WAV/source path, so it owns a separate control-trajectory
            // tolerance from the raw interpolator's -55 dB alias-rejection gate.
            CHECK(stepped_error < -48.0);
        }
        CHECK(continuous_error[0] < -48.0);
        for (std::size_t index = 1; index < blocks.size(); ++index) {
            const auto stepped_error = sampler_modulation_residual_db(
                sampler_modulation_reference(modulation_hz, blocks[index]),
                production[index]);
            CHECK(continuous_error[index] > -20.0);
            CHECK(stepped_error + 25.0 < continuous_error[index]);
            CHECK(sampler_modulation_residual_db(
                      production[0], production[index]) > -20.0);
        }
        CHECK(sampler_modulation_residual_db(
                  production[1], production[2]) > -20.0);
    }
}

TEST_CASE("PulpSampler prewarms reverse entry before publishing a stream", "[sampler][stream]") {
    TempSamplerWav wav("reverse_entry", 24000, 0.5f);
    SamplerFixture fixture;

    REQUIRE(fixture.proc->load_sample_file(wav.path));
    REQUIRE(fixture.proc->stream_stats().preload_frames < 24000);
    REQUIRE(PulpSamplerTestAccess::streamed_tail_page_ready(*fixture.proc));
    const auto contract = PulpSamplerTestAccess::published_preload_contract(*fixture.proc);
    const auto evaluated = audio::evaluate_sample_preload_contract(contract);
    REQUIRE(evaluated.valid());
    REQUIRE(contract.loop_prefetch_guard_frames == evaluated.block_guard_frames);
}

TEST_CASE("PulpSampler services active streams while staging a replacement",
          "[sampler][stream][sidecar]") {
    TempSamplerWav active("stage_service_active", 24000, 0.25f);
    TempSamplerWav replacement("stage_service_replacement", 24000, 0.75f);
    TempSamplerMipSidecar sidecar(replacement);
    SamplerFixture fixture;
    REQUIRE(fixture.proc->load_sample_file(active.path));

    PulpSamplerTestAccess::pause_file_stage(*fixture.proc, true);
    std::atomic<bool> load_result{false};
    std::thread loader([&] {
        load_result.store(fixture.proc->load_sample_file(replacement.path),
                          std::memory_order_release);
    });
    FileStageLoaderGuard loader_guard{*fixture.proc, loader};
    REQUIRE(wait_for_condition(
        [&] { return PulpSamplerTestAccess::file_stage_paused(*fixture.proc); }));

    REQUIRE(PulpSamplerTestAccess::fill_stream_command_inbox(*fixture.proc) > 0);
    REQUIRE(wait_for_condition(
        [&] { return PulpSamplerTestAccess::stream_command_count(*fixture.proc) == 0; }));

    PulpSamplerTestAccess::pause_file_stage(*fixture.proc, false);
    loader.join();
    REQUIRE(load_result.load(std::memory_order_acquire));
    REQUIRE(PulpSamplerTestAccess::published_stream_mip_count(*fixture.proc) == 2);
}

TEST_CASE("PulpSampler reports file staging exceptions as load failures", "[sampler][stream]") {
    TempSamplerWav source("stage_exception", 24000, 0.5f);
    SamplerFixture fixture;
    const auto memory_baseline = fixture.proc->stream_stats().memory;
    PulpSamplerTestAccess::throw_during_next_file_stage(*fixture.proc);

    REQUIRE_FALSE(fixture.proc->load_sample_file(source.path));
    REQUIRE(PulpSamplerTestAccess::published_source_kind(*fixture.proc) ==
            SamplerPublishedSourceKind::Resident);
    const auto memory_after_failure = fixture.proc->stream_stats().memory;
    REQUIRE(memory_after_failure.current_preload_bytes ==
            memory_baseline.current_preload_bytes);
    REQUIRE(memory_after_failure.current_page_bytes == memory_baseline.current_page_bytes);
    REQUIRE(memory_after_failure.current_total_bytes == memory_baseline.current_total_bytes);
}

TEST_CASE("PulpSampler establishes the certified lookahead for small blocks", "[sampler][stream]") {
    TempSamplerWav wav("small_block_lookahead", 24000, 0.5f);
    SamplerFixture fixture(1);
    REQUIRE(fixture.proc->load_sample_file(wav.path));
    const auto preload = fixture.proc->stream_stats().preload_frames;
    REQUIRE(preload > 0);
    REQUIRE(preload < 24000);

    REQUIRE(PulpSamplerTestAccess::pause_stream_command_drain(*fixture.proc, true));
    SamplerProcessBlock block(1);
    block.midi_in.add(midi::MidiEvent::note_on(0, 84, 127));
    {
        runtime::ScopedNoAlloc no_alloc;
        block.run(*fixture.proc);
    }

    REQUIRE_FALSE(PulpSamplerTestAccess::active_stream_boundary_pending(*fixture.proc));
    REQUIRE(PulpSamplerTestAccess::stream_command_count(*fixture.proc) > 0);
    REQUIRE(PulpSamplerTestAccess::lookahead_plans_last_callback(*fixture.proc) <= 8);
    REQUIRE(fixture.proc->stream_stats().starved_output_frames == 0);
    REQUIRE(PulpSamplerTestAccess::pause_stream_command_drain(*fixture.proc, false));
}

TEST_CASE("PulpSampler bounds lookahead work for a low-rate pitched asset",
          "[sampler][stream][rt]") {
    TempSamplerWav wav("low_rate_lookahead", 64, 0.5f, 1);
    SamplerFixture fixture;
    REQUIRE(fixture.proc->load_sample_file(wav.path));
    REQUIRE(fixture.proc->stream_stats().preload_frames < 64);

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 0, 127));
    {
        runtime::ScopedNoAlloc no_alloc;
        block.run(*fixture.proc);
    }

    REQUIRE(PulpSamplerTestAccess::lookahead_plans_last_callback(*fixture.proc) > 0);
    REQUIRE(PulpSamplerTestAccess::lookahead_plans_last_callback(*fixture.proc) <= 16);
}

TEST_CASE("PulpSampler recovers lookahead after command queue backpressure",
          "[sampler][stream][recovery]") {
    TempSamplerWav wav("lookahead_backpressure", 100000, 0.5f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 5.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));
    REQUIRE(PulpSamplerTestAccess::pause_stream_command_drain(*fixture.proc, true));
    REQUIRE(PulpSamplerTestAccess::fill_stream_command_inbox(*fixture.proc) > 0);

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    block.midi_in.clear();
    for (std::uint32_t callback = 0;
         callback < 8 && !PulpSamplerTestAccess::active_streamed_lookahead_pending(*fixture.proc);
         ++callback) {
        block.run(*fixture.proc);
    }
    REQUIRE(PulpSamplerTestAccess::active_streamed_lookahead_pending(*fixture.proc));
    for (std::uint32_t callback = 0;
         callback < 32 &&
         PulpSamplerTestAccess::active_streamed_lookahead_lead(*fixture.proc) >= 0.0;
         ++callback) {
        block.run(*fixture.proc);
    }
    REQUIRE(PulpSamplerTestAccess::active_streamed_lookahead_lead(*fixture.proc) < 0.0);

    REQUIRE(PulpSamplerTestAccess::pause_stream_command_drain(*fixture.proc, false));
    REQUIRE(wait_for_condition(
        [&] {
            block.run(*fixture.proc);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return !PulpSamplerTestAccess::active_streamed_lookahead_pending(*fixture.proc) &&
                   PulpSamplerTestAccess::active_streamed_lookahead_lead(*fixture.proc) > 0.0 &&
                   block.left.front() > 0.45f;
        },
        std::chrono::seconds(5)));

    const auto recovered_starvation = fixture.proc->stream_stats().starved_output_frames;
    for (std::uint32_t callback = 0; callback < 10; ++callback) {
        block.run(*fixture.proc);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(fixture.proc->stream_stats().starved_output_frames == recovered_starvation);
    REQUIRE(block.left.front() > 0.45f);
}

TEST_CASE("PulpSampler refreshes a partially queued lookahead prefix",
          "[sampler][stream][recovery]") {
    TempSamplerWav wav("lookahead_partial_prefix", 100000, 0.5f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    fixture.store.set_value(kSamplerReverse, 1.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));
    REQUIRE(PulpSamplerTestAccess::pause_stream_command_drain(*fixture.proc, true));
    REQUIRE(PulpSamplerTestAccess::fill_stream_command_inbox(*fixture.proc, 1) > 0);

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 84, 127));
    block.run(*fixture.proc);
    block.midi_in.clear();
    REQUIRE(PulpSamplerTestAccess::active_streamed_lookahead_pending(*fixture.proc));
    REQUIRE(PulpSamplerTestAccess::active_pending_demand_index(*fixture.proc) > 0);

    REQUIRE(PulpSamplerTestAccess::pause_stream_command_drain(*fixture.proc, false));
    REQUIRE(wait_for_condition(
        [&] {
            block.run(*fixture.proc);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return !PulpSamplerTestAccess::active_streamed_lookahead_pending(*fixture.proc) &&
                   block.left.front() > 0.45f;
        },
        std::chrono::seconds(5)));
}

TEST_CASE("PulpSampler emits streamed reverse one-shots from the tail",
          "[sampler][stream][reverse]") {
    std::vector<float> ramp(24000);
    for (std::size_t frame = 0; frame < ramp.size(); ++frame) {
        ramp[frame] = 0.1f + 0.8f * static_cast<float>(frame) / static_cast<float>(ramp.size() - 1);
    }
    TempSamplerWav wav("reverse_render", ramp);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerReverse, 1.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);

    REQUIRE(block.left.front() > 0.85f);
    REQUIRE(block.left[400] < block.left[63]);
    REQUIRE(fixture.proc->stream_stats().starved_output_frames == 0);
}

TEST_CASE("PulpSampler holds a reverse attack while its tail is refilled",
          "[sampler][stream][reverse]") {
    constexpr std::uint64_t kFrames = 24000;
    std::vector<float> ramp(kFrames);
    for (std::size_t frame = 0; frame < ramp.size(); ++frame) {
        ramp[frame] = 0.1f + 0.8f * static_cast<float>(frame) / static_cast<float>(ramp.size() - 1);
    }
    TempSamplerWav wav("reverse_evicted_attack", ramp);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerReverse, 1.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));
    REQUIRE(PulpSamplerTestAccess::streamed_reverse_horizon_ready(*fixture.proc));

    SamplerProcessBlock block;
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, true);
    PulpSamplerTestAccess::retire_reverse_attack_after_horizon(*fixture.proc);
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    REQUIRE_FALSE(PulpSamplerTestAccess::streamed_reverse_horizon_ready(*fixture.proc));
    REQUIRE(std::all_of(block.left.begin(), block.left.end(),
                        [](float sample) { return sample == 0.0f; }));
    REQUIRE(fixture.proc->stream_stats().starved_output_frames == block.left.size());
    REQUIRE(PulpSamplerTestAccess::active_streamed_position(*fixture.proc) ==
            static_cast<double>(kFrames - 1));

    block.midi_in.clear();
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, false);
    REQUIRE(wait_for_condition(
        [&] {
            if (PulpSamplerTestAccess::streamed_reverse_horizon_ready(*fixture.proc)) {
                return true;
            }
            block.run(*fixture.proc);
            return false;
        },
        std::chrono::milliseconds(5000)));
    block.run(*fixture.proc);
    REQUIRE(block.left.front() == 0.0f);
    REQUIRE(block.left[1] > 0.0f);
    REQUIRE(block.left[63] > 0.85f);
    REQUIRE(block.left[400] < block.left[63]);
}

TEST_CASE("PulpSampler keeps streamed reverse loops supplied across the seam",
          "[sampler][stream][reverse][loop]") {
    TempSamplerWav wav("reverse_loop", 24000, 0.5f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    fixture.store.set_value(kSamplerReverse, 1.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    block.midi_in.clear();
    const auto blocks = static_cast<std::uint32_t>(24000 / block.left.size()) + 4;
    std::uint32_t first_starved_block = 0;
    for (std::uint32_t index = 1; index < blocks; ++index) {
        block.run(*fixture.proc);
        if (first_starved_block == 0 && fixture.proc->stream_stats().starved_output_frames != 0) {
            first_starved_block = index;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    CAPTURE(first_starved_block);
    REQUIRE(fixture.proc->stream_stats().starved_output_frames == 0);
    REQUIRE(block.left.back() > 0.45f);
    REQUIRE(block.right.back() > 0.45f);
}

TEST_CASE("PulpSampler page geometry bounds dual-region crossfade demands", "[sampler][stream]") {
    state::StateStore store;
    PulpSamplerProcessor processor;
    processor.set_state_store(&store);
    processor.define_parameters(store);
    format::PrepareContext context;
    context.sample_rate = 44100;
    context.max_buffer_size = 8192;
    context.input_channels = 0;
    context.output_channels = 2;
    processor.prepare(context);

    const auto page_demands = PulpSamplerTestAccess::worst_case_dual_region_page_demands(processor);
    REQUIRE(page_demands <= PulpSamplerTestAccess::fixed_voice_demand_capacity());
    REQUIRE(page_demands <= PulpSamplerTestAccess::cache_pages_per_voice());
}

TEST_CASE("PulpSampler reverse prewarm deadline scales with its page horizon",
          "[sampler][stream][reverse]") {
    REQUIRE(PulpSamplerTestAccess::reverse_prewarm_timeout_for_pages(1) ==
            std::chrono::milliseconds(250));
    REQUIRE(PulpSamplerTestAccess::reverse_prewarm_timeout_for_pages(14) >=
            std::chrono::milliseconds(420));
}

TEST_CASE("PulpSampler does not publish a stream when reverse prewarm fails", "[sampler][stream]") {
    TempSamplerWav wav("reverse_entry_failure", 24000, 0.5f);
    SamplerFixture fixture;
    const auto memory_baseline = fixture.proc->stream_stats().memory;
    PulpSamplerTestAccess::set_reverse_prewarm_timeout(*fixture.proc, std::chrono::milliseconds(5));
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, true);

    REQUIRE_FALSE(fixture.proc->load_sample_file(wav.path));
    REQUIRE(PulpSamplerTestAccess::published_source_kind(*fixture.proc) ==
            SamplerPublishedSourceKind::Resident);
    REQUIRE(fixture.proc->stream_stats().active_sources == 0);
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, false);
    REQUIRE(wait_for_condition([&] {
        const auto memory = fixture.proc->stream_stats().memory;
        return memory.current_preload_bytes == memory_baseline.current_preload_bytes &&
               memory.current_page_bytes == memory_baseline.current_page_bytes &&
               memory.current_total_bytes == memory_baseline.current_total_bytes;
    }));
}

TEST_CASE("PulpSampler keeps the resident source published while prewarm waits",
          "[sampler][stream]") {
    TempSamplerWav wav("reverse_entry_ordering", 24000, 0.5f);
    SamplerFixture fixture;
    PulpSamplerTestAccess::set_reverse_prewarm_timeout(*fixture.proc, std::chrono::seconds(2));
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, true);

    std::atomic<bool> loaded{false};
    std::thread loader(
        [&] { loaded.store(fixture.proc->load_sample_file(wav.path), std::memory_order_release); });
    const bool reached_prewarm = wait_for_condition(
        [&] { return PulpSamplerTestAccess::reverse_prewarm_pending(*fixture.proc); });
    const auto kind_while_pending = PulpSamplerTestAccess::published_source_kind(*fixture.proc);
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, false);
    loader.join();

    REQUIRE(reached_prewarm);
    REQUIRE(kind_while_pending == SamplerPublishedSourceKind::Resident);
    REQUIRE(loaded.load(std::memory_order_acquire));
    REQUIRE(PulpSamplerTestAccess::streamed_tail_page_ready(*fixture.proc));
}

TEST_CASE("PulpSampler reclaims an in-flight failed prewarm registration", "[sampler][stream]") {
    TempSamplerWav failed("reverse_entry_in_flight", 24000, 0.5f);
    TempSamplerWav first("reverse_entry_reuse_a", 24000, 0.25f);
    TempSamplerWav second("reverse_entry_reuse_b", 24000, 0.75f);
    SamplerFixture fixture;
    const auto memory_baseline = fixture.proc->stream_stats().memory;
    PulpSamplerTestAccess::set_reverse_prewarm_timeout(*fixture.proc,
                                                       std::chrono::milliseconds(20));
    PulpSamplerTestAccess::block_next_reverse_decode(*fixture.proc);

    const bool failed_load = fixture.proc->load_sample_file(failed.path);
    const bool decode_entered = PulpSamplerTestAccess::reverse_decode_entered(*fixture.proc);
    const auto rollback_count = PulpSamplerTestAccess::unpublished_rollback_count(*fixture.proc);
    PulpSamplerTestAccess::release_reverse_decode(*fixture.proc);
    const bool rollback_completed = wait_for_condition(
        [&] { return PulpSamplerTestAccess::unpublished_rollback_count(*fixture.proc) == 0; });
    PulpSamplerTestAccess::set_reverse_prewarm_timeout(*fixture.proc,
                                                       std::chrono::milliseconds(250));

    REQUIRE_FALSE(failed_load);
    REQUIRE(decode_entered);
    REQUIRE(rollback_count == 1);
    REQUIRE(rollback_completed);
    const auto memory_after_rollback = fixture.proc->stream_stats().memory;
    REQUIRE(memory_after_rollback.current_preload_bytes ==
            memory_baseline.current_preload_bytes);
    REQUIRE(memory_after_rollback.current_page_bytes == memory_baseline.current_page_bytes);
    REQUIRE(memory_after_rollback.current_total_bytes == memory_baseline.current_total_bytes);
    REQUIRE(fixture.proc->load_sample_file(first.path));
    REQUIRE(fixture.proc->load_sample_file(second.path));
}

TEST_CASE("PulpSampler shutdown rejects an in-flight prewarm admission", "[sampler][stream]") {
    TempSamplerWav wav("reverse_entry_shutdown", 24000, 0.5f);
    SamplerFixture fixture;
    PulpSamplerTestAccess::set_reverse_prewarm_timeout(*fixture.proc, std::chrono::seconds(2));
    PulpSamplerTestAccess::block_next_reverse_decode(*fixture.proc);

    std::atomic<bool> load_result{true};
    std::thread loader([&] {
        load_result.store(fixture.proc->load_sample_file(wav.path), std::memory_order_release);
    });
    const bool decode_entered = wait_for_condition(
        [&] { return PulpSamplerTestAccess::reverse_decode_entered(*fixture.proc); });
    fixture.proc->release();
    loader.join();

    REQUIRE(decode_entered);
    REQUIRE_FALSE(load_result.load(std::memory_order_acquire));
    REQUIRE(PulpSamplerTestAccess::published_source_kind(*fixture.proc) ==
            SamplerPublishedSourceKind::None);
    const auto released_memory = fixture.proc->stream_stats().memory;
    REQUIRE(released_memory.current_preload_bytes == 0);
    REQUIRE(released_memory.current_page_bytes == 0);
    REQUIRE(released_memory.current_total_bytes == 0);
}

TEST_CASE("PulpSampler reuses bounded stream identities without ABA",
          "[sampler][stream][generation][capacity]") {
    TempSamplerWav wav("stream_identity_churn", 24000, 0.5f);
    SamplerFixture fixture;
    SamplerProcessBlock block;
    std::set<std::pair<std::uint64_t, std::uint64_t>> observed_tokens;
    std::set<std::uint64_t> observed_ids;

    constexpr std::size_t churn_count = 64;
    for (std::size_t iteration = 0; iteration < churn_count; ++iteration) {
        REQUIRE(fixture.proc->load_sample_file(wav.path));
        const auto token =
            PulpSamplerTestAccess::published_stream_source(*fixture.proc);
        REQUIRE(token.source_id != 0);
        REQUIRE(token.source_id <=
                PulpSamplerTestAccess::stream_source_identity_capacity());
        REQUIRE(token.source_generation != 0);
        REQUIRE(observed_tokens.emplace(token.source_id,
                                        token.source_generation).second);
        observed_ids.insert(token.source_id);

        block.run(*fixture.proc);
        REQUIRE(wait_for_condition([&] {
            return PulpSamplerTestAccess::physical_stream_source_count(
                       *fixture.proc) == 1;
        }));
    }

    REQUIRE(observed_ids.size() == 2);
    const auto cache = PulpSamplerTestAccess::stream_cache_stats(*fixture.proc);
    REQUIRE(cache.source_identity_count == observed_ids.size());
    REQUIRE(cache.source_identity_capacity ==
            PulpSamplerTestAccess::stream_source_identity_capacity());
    REQUIRE(cache.source_identity_capacity_rejections == 0);
}

TEST_CASE("PulpSampler stream identity generation wrap fails closed",
          "[sampler][stream][generation][capacity][overflow]") {
    SamplerFixture fixture;
    constexpr std::size_t identity_index = 0;
    constexpr auto maximum_generation =
        std::numeric_limits<std::uint64_t>::max();
    PulpSamplerTestAccess::set_next_stream_source_generation(
        *fixture.proc, identity_index, maximum_generation);

    const auto final = PulpSamplerTestAccess::take_stream_source_token(
        *fixture.proc, identity_index);
    REQUIRE(final.has_value());
    REQUIRE(final->source_id == identity_index + 1);
    REQUIRE(final->source_generation == maximum_generation);
    REQUIRE_FALSE(PulpSamplerTestAccess::take_stream_source_token(
        *fixture.proc, identity_index).has_value());
}
