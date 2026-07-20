#include <pulp/audio/sample_heritage.hpp>
#include <pulp/audio/loop_renderer.hpp>
#include <pulp/audio/sample_sinc_kernel.hpp>
#include <pulp/signal/adsr.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <vector>

namespace {

using namespace pulp::audio;

constexpr double kPi = 3.14159265358979323846;

SampleHeritageRecordProvenance provenance() {
    return {.source_id = "fixture:shipping-gate",
            .capture_method = "synthetic-offline",
            .evidence_id = "evidence:analytic-oracle"};
}

Buffer<float> ramp(std::size_t frames) {
    Buffer<float> result(1, frames);
    for (std::size_t frame = 0; frame < frames; ++frame)
        result.channel(0)[frame] = static_cast<float>(frame);
    return result;
}

struct SpectralImage {
    double predicted_amplitude = 0.0;
    double largest_unpredicted_amplitude = 0.0;
};

double measure_hann_tone_amplitude(std::span<const float> signal,
                                   double cycles) {
    double real = 0.0;
    double imaginary = 0.0;
    double weight_sum = 0.0;
    for (std::size_t frame = 0; frame < signal.size(); ++frame) {
        const auto weight = 0.5 - 0.5 * std::cos(
            2.0 * kPi * static_cast<double>(frame) /
            static_cast<double>(signal.size() - 1));
        const auto phase = 2.0 * kPi * cycles * static_cast<double>(frame) /
                           static_cast<double>(signal.size());
        real += weight * static_cast<double>(signal[frame]) * std::cos(phase);
        imaginary -= weight * static_cast<double>(signal[frame]) * std::sin(phase);
        weight_sum += weight;
    }
    return 2.0 * std::hypot(real, imaginary) / weight_sum;
}

struct OffGridSpectralControl {
    double expected_amplitude = 0.0;
    double unprocessed_hypothesis_amplitude = 0.0;
    double unrelated_hypothesis_amplitude = 0.0;
};

OffGridSpectralControl render_off_grid_pitch_control(
    SampleHeritagePitchFamily family, double factor) {
    constexpr std::size_t frames = 2048;
    constexpr double expected_cycles = 37.375;
    constexpr double unrelated_cycles = 83.125;
    constexpr double phase_offset = 0.317;
    SampleHeritagePitchProcessor processor;
    REQUIRE(processor.prepare(family, factor, 1) == SampleHeritagePitchStatus::Ok);
    const auto plan = processor.plan(frames);
    REQUIRE(plan.valid());
    Buffer<float> input(1, plan.input_frames);
    Buffer<float> output(1, frames);
    const auto source_period = family == SampleHeritagePitchFamily::VariableClock
        ? static_cast<double>(frames)
        : static_cast<double>(frames) * factor;
    for (std::size_t frame = 0; frame < plan.input_frames; ++frame)
        input.channel(0)[frame] = static_cast<float>(
            0.5 * std::sin(2.0 * kPi * expected_cycles *
                               static_cast<double>(frame) / source_period +
                           phase_offset));
    REQUIRE(processor.process(std::as_const(input).view(), output.view()) ==
            SampleHeritagePitchStatus::Ok);
    return {
        .expected_amplitude =
            measure_hann_tone_amplitude(output.channel(0), expected_cycles),
        .unprocessed_hypothesis_amplitude = measure_hann_tone_amplitude(
            output.channel(0), expected_cycles / factor),
        .unrelated_hypothesis_amplitude =
            measure_hann_tone_amplitude(output.channel(0), unrelated_cycles),
    };
}

SpectralImage measure_single_bin_image(std::span<const float> signal,
                                       std::size_t predicted_bin) {
    SpectralImage image;
    for (std::size_t bin = 1; bin < signal.size() / 2; ++bin) {
        double real = 0.0;
        double imaginary = 0.0;
        for (std::size_t frame = 0; frame < signal.size(); ++frame) {
            const auto phase = 2.0 * kPi * static_cast<double>(bin * frame) /
                               static_cast<double>(signal.size());
            real += static_cast<double>(signal[frame]) * std::cos(phase);
            imaginary -= static_cast<double>(signal[frame]) * std::sin(phase);
        }
        const auto amplitude = 2.0 * std::hypot(real, imaginary) /
                               static_cast<double>(signal.size());
        if (bin == predicted_bin)
            image.predicted_amplitude = amplitude;
        else
            image.largest_unpredicted_amplitude =
                std::max(image.largest_unpredicted_amplitude, amplitude);
    }
    return image;
}

SpectralImage render_pitch_image(SampleHeritagePitchFamily family,
                                 double factor) {
    constexpr std::size_t frames = 1024;
    constexpr std::size_t predicted_bin = 37;
    constexpr double amplitude = 0.5;
    SampleHeritagePitchProcessor processor;
    REQUIRE(processor.prepare(family, factor, 1) == SampleHeritagePitchStatus::Ok);
    const auto plan = processor.plan(frames);
    REQUIRE(plan.valid());
    Buffer<float> input(1, plan.input_frames);
    Buffer<float> output(1, frames);
    const auto source_period_frames = family == SampleHeritagePitchFamily::VariableClock
        ? static_cast<double>(frames)
        : static_cast<double>(frames) * factor;
    for (std::size_t frame = 0; frame < plan.input_frames; ++frame)
        input.channel(0)[frame] = static_cast<float>(
            amplitude * std::sin(2.0 * kPi * predicted_bin *
                                 static_cast<double>(frame) /
                                 source_period_frames));
    REQUIRE(processor.process(std::as_const(input).view(), output.view()) ==
            SampleHeritagePitchStatus::Ok);
    return measure_single_bin_image(output.channel(0), predicted_bin);
}

SampleHeritageProfile typed_profile(
    std::string id,
    std::vector<SampleHeritageVoiceBlockSpec> voice,
    std::vector<SampleHeritageBusBlockSpec> bus = {},
    std::vector<SampleHeritageRecordCommitBlockSpec> record_commit = {}) {
    return {.schema_version = kSampleHeritageProfileSchemaVersion,
            .profile_id = std::move(id),
            .host_sample_rate = 48000.0,
            .voice = std::move(voice),
            .bus = std::move(bus),
            .record_commit = std::move(record_commit)};
}

void render_voice_once(const SampleHeritagePreparedProfile& profile,
                       std::size_t frames = 128) {
    SampleHeritageEngine engine;
    REQUIRE(engine.prepare({profile, 1, frames}) == SampleHeritagePrepareStatus::Ok);
    const auto plan = engine.plan_exact(frames);
    REQUIRE(plan.valid());
    Buffer<float> input(1, plan.input_frames);
    Buffer<float> output(1, frames);
    for (std::size_t frame = 0; frame < plan.input_frames; ++frame)
        input.channel(0)[frame] = static_cast<float>(
            0.4 * std::sin(2.0 * kPi * static_cast<double>(frame) / 31.0));
    REQUIRE(engine.process_exact(plan, std::as_const(input).view(), output.view()) ==
            SampleHeritageProcessStatus::Ok);
    REQUIRE(std::all_of(output.channel(0).begin(), output.channel(0).end(),
                        [](float value) { return std::isfinite(value); }));
}

void render_composition_once(const SampleHeritageProfile& source_profile,
                             const SampleHeritagePreparedProfile& profile) {
    render_voice_once(profile);

    SampleHeritageBusDsp bus;
    REQUIRE(bus.prepare(profile, 48000.0, 1) == SampleHeritageBusDspStatus::Ok);
    Buffer<float> mix(1, 128);
    std::fill(mix.channel(0).begin(), mix.channel(0).end(), 0.25f);
    REQUIRE(bus.process(mix.view(), true) == SampleHeritageBusDspStatus::Ok);
    REQUIRE(std::all_of(mix.channel(0).begin(), mix.channel(0).end(),
                        [](float value) { return std::isfinite(value); }));

    if (!source_profile.record_commit.empty()) {
        auto source = ramp(256);
        const auto committed = commit_sample_heritage_recording(
            source_profile, std::as_const(source).view(), 48000.0, provenance());
        REQUIRE(committed.valid());
        REQUIRE(committed.asset->audio().num_samples() > 0);
    }
}

SampleHeritageProfile performance_profile() {
    std::vector<SampleHeritageVoiceBlockSpec> voice{
        {SampleHeritageBlockDomain::Voice, false,
         SampleHeritageVoiceMachineDomainBlock{32000.0}},
    };
    voice.push_back({SampleHeritageBlockDomain::Voice, false,
                     SampleHeritageVoiceClockBlock{1.0}});
    voice.push_back({SampleHeritageBlockDomain::Voice, false,
                     SampleHeritageVoicePitchBlock{
                         SampleHeritagePitchFamily::VariableClock, 24.0}});
    voice.push_back({SampleHeritageBlockDomain::Voice, false,
                     SampleHeritageVoiceConverterBlock{
                         SampleHeritageConverterFamily::LinearPcm, 12.0f, 0.05f, 0.0f,
                         0x4711u,
                         SampleHeritageSeedPolicy::RestartFromProfileSeed}});
    voice.push_back({SampleHeritageBlockDomain::Voice, false,
                     SampleHeritageVoiceLiveCyclicStretchBlock{
                         1.25, 20.0, 2.0, true, 0, 0,
                         SampleHeritageSeedPolicy::RestartFromProfileSeed}});
    voice.push_back({SampleHeritageBlockDomain::Voice, false,
                     SampleHeritageVoiceHoldDroopBlock{
                         SampleHeritageHoldMode::ZeroOrder, 2, 0.002f}});
    voice.push_back({SampleHeritageBlockDomain::Voice, false,
                     SampleHeritageVoiceReconstructionBlock{
                         SampleHeritageReconstructionFamily::Butterworth,
                         SampleHeritageCutoffLaw::FixedHz, 11000.0, 2, 0.0f, 0.0f}});
    voice.push_back({SampleHeritageBlockDomain::Voice, false,
                     SampleHeritageVoiceAnalogColorBlock{
                         1.1f, 0.04f, 0.5f,
                         SampleHeritageAnalogFilterFamily::Ladder4Pole,
                         SampleHeritageCutoffLaw::FixedHz, 9000.0, 0.15f}});
    std::vector<SampleHeritageBusBlockSpec> bus{
        {SampleHeritageBlockDomain::Bus, false,
         SampleHeritageBusNoiseIdleBlock{
             .noise_amplitude = 0.0005f,
             .idle_amplitude = 0.0f,
             .tilt_db_per_octave = 0.0f,
             .gate = SampleHeritageNoiseGate::VoiceActive,
             .seed = 0x7813u,
             .seed_policy = SampleHeritageSeedPolicy::RestartFromProfileSeed}},
        {SampleHeritageBlockDomain::Bus, false,
         SampleHeritageBusOutputDriveBlock{1.05f, 0.98f}},
    };
    return typed_profile("neutral.shipping-chain", std::move(voice),
                         std::move(bus));
}

double measured_sampler_pipeline_seconds(
    const SampleHeritagePreparedProfile* profile) {
    constexpr std::size_t frames = 256;
    constexpr std::size_t warmups = 128;
    constexpr std::size_t iterations = 512;
    constexpr std::size_t batches = 5;
    constexpr std::size_t source_frames = 65536;
    constexpr double playback_rate = 1.25;

    SampleSincKernelBank sinc_bank;
    REQUIRE(sinc_bank.build_dense_for_maximum_consumption(4.0));
    const PreparedSampleInterpolation interpolation{
        .policy = SampleInterpolationPolicy::RatioTrackingSinc,
        .sinc = sinc_bank.view().select(playback_rate),
    };
    REQUIRE(interpolation.valid());
    Buffer<float> source(1, source_frames);
    for (std::size_t frame = 0; frame < source_frames; ++frame)
        source.channel(0)[frame] = static_cast<float>(
            0.35 * std::sin(static_cast<double>(frame) * 0.031) +
            0.1 * std::cos(static_cast<double>(frame) * 0.007));
    LoopRenderer renderer;
    REQUIRE(renderer.set_region(
        {.start_frame = 128,
         .end_frame = source_frames - 128,
         .crossfade_frames = 0,
         .source_sample_rate = 48000.0,
         .playback_mode = LoopPlaybackMode::Forward,
         .crossfade_curve = LoopCrossfadeCurve::Linear,
         .interpolation = LoopInterpolationMode::Linear,
         .snap_policy = LoopSnapPolicy::None},
        source_frames));
    REQUIRE(renderer.set_interpolation(interpolation));
    renderer.set_playback_rate(playback_rate);
    renderer.start();

    SampleHeritageEngine engine;
    SampleHeritageBusDsp bus;
    if (profile != nullptr) {
        REQUIRE(engine.prepare({*profile, 1, frames}) ==
                SampleHeritagePrepareStatus::Ok);
        REQUIRE(bus.prepare(*profile, 48000.0, 1) ==
                SampleHeritageBusDspStatus::Ok);
    }
    Buffer<float> raw(1, profile == nullptr ? frames
                                            : engine.maximum_input_frames());
    Buffer<float> output(1, frames);
    Buffer<float> mix(1, frames);
    pulp::signal::Adsr envelope;
    envelope.set_sample_rate(48000.0f);
    envelope.set_params({.attack = 0.0f,
                         .decay = 0.0f,
                         .sustain = 1.0f,
                         .release = 0.0f});
    envelope.note_on();

    const auto run = [&](std::size_t count) {
        float sink = 0.0f;
        bool valid = true;
        for (std::size_t iteration = 0; iteration < count; ++iteration) {
            SampleHeritageProcessPlan plan;
            if (profile != nullptr) plan = engine.plan_exact(frames);
            const auto raw_frames =
                profile == nullptr ? frames : plan.input_frames;
            if (profile != nullptr && !plan.valid()) {
                valid = false;
                break;
            }
            const auto source_render = renderer.render(
                std::as_const(source).view(), raw.view().slice(0, raw_frames),
                raw_frames);
            if (!source_render.active ||
                source_render.rendered_frames != raw_frames) {
                valid = false;
                break;
            }
            if (profile != nullptr) {
                if (plan.input_frames != raw_frames ||
                    engine.process_exact(
                        plan, std::as_const(raw).view().slice(0, raw_frames),
                        output.view()) != SampleHeritageProcessStatus::Ok) {
                    valid = false;
                    break;
                }
            }
            mix.clear();
            const auto voice = profile == nullptr ? raw.channel(0).first(frames)
                                                  : output.channel(0);
            for (std::size_t frame = 0; frame < frames; ++frame) {
                const auto scale = envelope.next() * 0.8f * 0.9f;
                mix.channel(0)[frame] += voice[frame] * scale;
            }
            if (profile != nullptr &&
                bus.process(mix.view(), true) !=
                    SampleHeritageBusDspStatus::Ok) {
                valid = false;
                break;
            }
            sink += mix.channel(0)[iteration % frames];
        }
        return std::pair{sink, valid};
    };
    static volatile float observed = 0.0f;
    const auto warmup = run(warmups);
    REQUIRE(warmup.second);
    observed = observed + warmup.first;
    std::array<double, batches> elapsed{};
    for (auto& seconds : elapsed) {
        const auto start = std::chrono::steady_clock::now();
        const auto measured = run(iterations);
        seconds = std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - start)
                      .count();
        REQUIRE(measured.second);
        observed = observed + measured.first;
    }
    std::sort(elapsed.begin(), elapsed.end());
    return elapsed[elapsed.size() / 2];
}

double measured_live_scaling_ratio(const SampleHeritageLiveCyclicConfig& config) {
    constexpr std::size_t warmup_blocks = 64;
    constexpr std::size_t measured_blocks = 256;
    constexpr std::uint64_t late_history_frames = UINT64_C(1) << 20;
    constexpr std::size_t trials = 5;
    std::array<double, trials> ratios{};
    static volatile float observed = 0.0f;

    for (auto& ratio : ratios) {
        SampleHeritageLiveCyclicStretch stretch;
        REQUIRE(stretch.prepare(config) == SampleHeritageLiveCyclicStatus::Ok);
        Buffer<float> input(config.channel_count,
                            stretch.resources().maximum_input_frames);
        Buffer<float> output(config.channel_count, config.max_block_samples);
        for (std::size_t channel = 0; channel < input.num_channels(); ++channel)
            for (std::size_t frame = 0; frame < input.num_samples(); ++frame)
                input.channel(channel)[frame] = static_cast<float>(
                    0.2 * std::sin(static_cast<double>(frame + channel) * 0.037));

        const auto process_blocks = [&](std::size_t blocks) {
            bool valid = true;
            float sink = 0.0f;
            for (std::size_t block = 0; block < blocks; ++block) {
                const auto plan = stretch.plan(config.max_block_samples);
                if (!plan.valid() ||
                    stretch.process(
                        std::as_const(input).view().slice(0, plan.input_frames),
                        output.view()) != SampleHeritageLiveCyclicStatus::Ok) {
                    valid = false;
                    break;
                }
                sink += output.channel(0)[block % config.max_block_samples];
            }
            return std::pair{sink, valid};
        };

        const auto warmup = process_blocks(warmup_blocks);
        REQUIRE(warmup.second);
        observed = observed + warmup.first;
        const auto early_start = std::chrono::steady_clock::now();
        const auto early = process_blocks(measured_blocks);
        const auto early_seconds = std::chrono::duration<double>(
                                       std::chrono::steady_clock::now() -
                                       early_start)
                                       .count();
        REQUIRE(early.second);
        observed = observed + early.first;

        const auto rendered_frames =
            (warmup_blocks + measured_blocks) * config.max_block_samples;
        const auto advance_blocks = static_cast<std::size_t>(
            (late_history_frames - rendered_frames +
             config.max_block_samples - 1) /
            config.max_block_samples);
        const auto advance = process_blocks(advance_blocks);
        REQUIRE(advance.second);
        observed = observed + advance.first;
        const auto late_start = std::chrono::steady_clock::now();
        const auto late = process_blocks(measured_blocks);
        const auto late_seconds = std::chrono::duration<double>(
                                      std::chrono::steady_clock::now() - late_start)
                                      .count();
        REQUIRE(late.second);
        observed = observed + late.first;
        REQUIRE(early_seconds > 0.0);
        ratio = late_seconds / early_seconds;
    }
    std::sort(ratios.begin(), ratios.end());
    return ratios[ratios.size() / 2];
}

}  // namespace

TEST_CASE("Shipping image oracles cover every pitch family and stretch splice",
          "[audio][sampler][heritage][shipping-gate][g1][image]") {
    constexpr std::array families{
        SampleHeritagePitchFamily::VariableClock,
        SampleHeritagePitchFamily::DropRepeat,
        SampleHeritagePitchFamily::EarlyLinear,
    };
    constexpr std::array<std::uint32_t, 6> identity_bits{
        0x00000000u, 0x80000000u, 0x3f800000u,
        0xbf400000u, 0x00800000u, 0x7fc12345u,
    };

    for (const auto family : families) {
        SampleHeritagePitchProcessor identity;
        REQUIRE(identity.prepare(family, 1.0, 1) == SampleHeritagePitchStatus::Ok);
        Buffer<float> input(1, identity_bits.size());
        Buffer<float> output(1, identity_bits.size());
        for (std::size_t frame = 0; frame < identity_bits.size(); ++frame)
            input.channel(0)[frame] = std::bit_cast<float>(identity_bits[frame]);
        REQUIRE(identity.process(std::as_const(input).view(), output.view()) ==
                SampleHeritagePitchStatus::Ok);
        for (std::size_t frame = 0; frame < identity_bits.size(); ++frame)
            REQUIRE(std::bit_cast<std::uint32_t>(output.channel(0)[frame]) ==
                    identity_bits[frame]);

        const auto factor = family == SampleHeritagePitchFamily::VariableClock
            ? 1.5
            : 2.0;
        const auto image = render_pitch_image(family, factor);
        const auto level_error_db = 20.0 * std::log10(image.predicted_amplitude / 0.5);
        REQUIRE(std::abs(level_error_db) <= 1.5);
        REQUIRE(image.largest_unpredicted_amplitude <=
                image.predicted_amplitude * std::pow(10.0, -90.0 / 20.0));

        const auto off_grid = render_off_grid_pitch_control(family, factor);
        REQUIRE(off_grid.expected_amplitude >= 0.45);
        REQUIRE(off_grid.unprocessed_hypothesis_amplitude <=
                off_grid.expected_amplitude * 0.02);
        REQUIRE(off_grid.unrelated_hypothesis_amplitude <=
                off_grid.expected_amplitude * 0.02);
    }

    SampleHeritagePitchProcessor drop_repeat;
    REQUIRE(drop_repeat.prepare(SampleHeritagePitchFamily::DropRepeat, 0.5, 1) ==
            SampleHeritagePitchStatus::Ok);
    auto drop_plan = drop_repeat.plan(8);
    auto drop_input = ramp(drop_plan.input_frames);
    Buffer<float> drop_output(1, 8);
    REQUIRE(drop_repeat.process(std::as_const(drop_input).view(), drop_output.view()) ==
            SampleHeritagePitchStatus::Ok);
    for (std::size_t frame = 0; frame < 8; ++frame)
        REQUIRE(drop_output.channel(0)[frame] == static_cast<float>(frame / 2));

    SampleHeritagePitchProcessor linear;
    REQUIRE(linear.prepare(SampleHeritagePitchFamily::EarlyLinear, 0.5, 1) ==
            SampleHeritagePitchStatus::Ok);
    auto linear_plan = linear.plan(8);
    auto linear_input = ramp(linear_plan.input_frames);
    Buffer<float> linear_output(1, 8);
    REQUIRE(linear.process(std::as_const(linear_input).view(), linear_output.view()) ==
            SampleHeritagePitchStatus::Ok);
    for (std::size_t frame = 0; frame < 8; ++frame)
        REQUIRE(linear_output.channel(0)[frame] == Catch::Approx(frame * 0.5));

    auto source = ramp(64);
    auto cyclic_profile = typed_profile(
        "neutral.shipping-cyclic", {}, {},
        {{SampleHeritageBlockDomain::RecordCommit, false,
          SampleHeritageRecordCommitCyclicStretchBlock{
              .factor = 2.0, .cycle_samples = 8, .crossfade_samples = 2}}});
    const auto cyclic = commit_sample_heritage_recording(
        cyclic_profile, std::as_const(source).view(), 48000.0, provenance());
    REQUIRE(cyclic.valid());
    const auto first_weight = std::pow(std::sin(kPi / 8.0), 2.0);
    REQUIRE(cyclic.asset->audio().channel(0)[8] ==
            Catch::Approx(8.0 * (1.0 - first_weight) + 4.0 * first_weight));
    REQUIRE(cyclic.asset->audio().channel(0)[16] ==
            Catch::Approx(12.0 * (1.0 - first_weight) + 8.0 * first_weight));

    auto adaptive_profile = typed_profile(
        "neutral.shipping-adaptive", {}, {},
        {{SampleHeritageBlockDomain::RecordCommit, false,
          SampleHeritageRecordCommitAdaptiveStretchBlock{
              .factor = 2.0, .decision_hop_samples = 8,
              .search_radius_samples = 4, .search_stride_samples = 1,
              .crossfade_samples = 2}}});
    Buffer<float> adaptive_source(1, 64);
    adaptive_source.channel(0)[10] = 1.0f;
    const auto adaptive = commit_sample_heritage_recording(
        adaptive_profile, std::as_const(adaptive_source).view(), 48000.0,
        provenance());
    REQUIRE(adaptive.valid());
    REQUIRE(adaptive.asset->audio().channel(0)[14] == Catch::Approx(1.0f));
}

TEST_CASE("Shipping live splice and bypass oracles are analytic and bit transparent",
          "[audio][sampler][heritage][shipping-gate][g1][image][bypass]") {
    SampleHeritageLiveCyclicConfig live_config;
    live_config.factor = 2.0;
    live_config.cycle_samples = 8;
    live_config.crossfade_samples = 2;
    live_config.max_block_samples = 18;
    live_config.channel_count = 1;
    SampleHeritageLiveCyclicStretch live;
    REQUIRE(live.prepare(live_config) == SampleHeritageLiveCyclicStatus::Ok);
    const auto live_plan = live.plan(18);
    REQUIRE(live_plan.valid());
    auto live_input = ramp(live_plan.input_frames);
    Buffer<float> live_output(1, 18);
    REQUIRE(live.process(std::as_const(live_input).view(), live_output.view()) ==
            SampleHeritageLiveCyclicStatus::Ok);
    REQUIRE(live_output.channel(0)[7] == 7.0f);
    REQUIRE(live_output.channel(0)[8] == 8.0f);
    REQUIRE(live_output.channel(0)[9] == 5.0f);
    REQUIRE(live_output.channel(0)[15] == 11.0f);
    REQUIRE(live_output.channel(0)[16] == 12.0f);
    REQUIRE(live_output.channel(0)[17] == 9.0f);

    constexpr std::array<std::uint32_t, 6> identity_bits{
        0x00000000u, 0x80000000u, 0x3f800000u,
        0xbf400000u, 0x00800000u, 0x7fc12345u,
    };
    live_config.factor = 1.0;
    live_config.max_block_samples = identity_bits.size();
    SampleHeritageLiveCyclicStretch identity_live;
    REQUIRE(identity_live.prepare(live_config) == SampleHeritageLiveCyclicStatus::Ok);
    const auto identity_plan = identity_live.plan(identity_bits.size());
    REQUIRE(identity_plan.valid());
    REQUIRE(identity_plan.input_frames == identity_bits.size());
    Buffer<float> identity_input(1, identity_bits.size());
    Buffer<float> identity_output(1, identity_bits.size());
    for (std::size_t frame = 0; frame < identity_bits.size(); ++frame)
        identity_input.channel(0)[frame] = std::bit_cast<float>(identity_bits[frame]);
    REQUIRE(identity_live.process(std::as_const(identity_input).view(),
                                  identity_output.view()) ==
            SampleHeritageLiveCyclicStatus::Ok);
    for (std::size_t frame = 0; frame < identity_bits.size(); ++frame)
        REQUIRE(std::bit_cast<std::uint32_t>(identity_output.channel(0)[frame]) ==
                identity_bits[frame]);

    const auto all_bypassed_source = typed_profile(
        "neutral.shipping-all-bypassed",
        {{SampleHeritageBlockDomain::Voice, true,
          SampleHeritageVoiceMachineDomainBlock{48000.0}},
         {SampleHeritageBlockDomain::Voice, true,
          SampleHeritageVoiceClockBlock{2.0}},
         {SampleHeritageBlockDomain::Voice, true,
          SampleHeritageVoicePitchBlock{
              SampleHeritagePitchFamily::EarlyLinear, 24.0}},
         {SampleHeritageBlockDomain::Voice, true,
          SampleHeritageVoiceLiveCyclicStretchBlock{
              2.0, 8.0, 1.0, true, 0, 0,
              SampleHeritageSeedPolicy::RestartFromProfileSeed,
              SampleHeritageLivePitchMode::RateLinked, true}}},
        {{SampleHeritageBlockDomain::Bus, true,
          SampleHeritageBusOutputDriveBlock{2.0f, 0.8f}}},
        {{SampleHeritageBlockDomain::RecordCommit, true,
          SampleHeritageRecordInputDriveClipBlock{2.0f, 0.8f}},
         {SampleHeritageBlockDomain::RecordCommit, true,
          SampleHeritageRecordRateBlock{
              SampleHeritageRecordFilterFamily::OnePole, 48000.0,
              SampleHeritageCutoffLaw::FixedHz, 10000.0, 1, 0.0f, 0.0f}},
         {SampleHeritageBlockDomain::RecordCommit, true,
          SampleHeritageRecordConverterBlock{
              SampleHeritageConverterFamily::MuLaw, 8.0f, 0.2f, 0.0f, 0,
              SampleHeritageSeedPolicy::RestartFromProfileSeed}},
         {SampleHeritageBlockDomain::RecordCommit, true,
          SampleHeritageRecordCommitAdaptiveStretchBlock{
              .factor = 2.0,
              .decision_hop_samples = 8,
              .search_radius_samples = 4,
              .search_stride_samples = 1,
              .crossfade_samples = 2}}});
    const auto all_bypassed =
        validate_sample_heritage_profile(all_bypassed_source);
    REQUIRE(all_bypassed.valid());
    SampleHeritageEngine engine;
    REQUIRE(engine.prepare({all_bypassed.profile, 1, identity_bits.size()}) ==
            SampleHeritagePrepareStatus::Ok);
    const auto engine_plan = engine.plan_exact(identity_bits.size());
    REQUIRE(engine_plan.valid());
    REQUIRE(engine_plan.input_frames == identity_bits.size());
    Buffer<float> engine_output(1, identity_bits.size());
    REQUIRE(engine.process_exact(engine_plan, std::as_const(identity_input).view(),
                                 engine_output.view()) ==
            SampleHeritageProcessStatus::Ok);
    for (std::size_t frame = 0; frame < identity_bits.size(); ++frame)
        REQUIRE(std::bit_cast<std::uint32_t>(engine_output.channel(0)[frame]) ==
                identity_bits[frame]);

    SampleHeritageBusDsp bus;
    REQUIRE(bus.prepare(all_bypassed.profile, 48000.0, 1) ==
            SampleHeritageBusDspStatus::Ok);
    auto bus_output = identity_input;
    REQUIRE(bus.process(bus_output.view(), true) == SampleHeritageBusDspStatus::Ok);
    for (std::size_t frame = 0; frame < identity_bits.size(); ++frame)
        REQUIRE(std::bit_cast<std::uint32_t>(bus_output.channel(0)[frame]) ==
                identity_bits[frame]);

    Buffer<float> finite_input(1, identity_bits.size() - 1);
    for (std::size_t frame = 0; frame < finite_input.num_samples(); ++frame)
        finite_input.channel(0)[frame] = std::bit_cast<float>(identity_bits[frame]);
    const auto committed = commit_sample_heritage_recording(
        all_bypassed_source, std::as_const(finite_input).view(), 48000.0,
        provenance());
    REQUIRE(committed.valid());
    REQUIRE(committed.asset->audio().num_samples() == finite_input.num_samples());
    for (std::size_t frame = 0; frame < finite_input.num_samples(); ++frame)
        REQUIRE(std::bit_cast<std::uint32_t>(
                    committed.asset->audio().channel(0)[frame]) ==
                identity_bits[frame]);
}

TEST_CASE("Representative heritage compositions render and reject invalid plans",
          "[audio][sampler][heritage][shipping-gate][composition]") {
    const std::array profiles{
        typed_profile("neutral.shipping-clean", {}),
        typed_profile(
            "neutral.shipping-hardware-chain",
            {{SampleHeritageBlockDomain::Voice, false,
              SampleHeritageVoiceMachineDomainBlock{32000.0}},
             {SampleHeritageBlockDomain::Voice, false,
              SampleHeritageVoiceConverterBlock{
                  SampleHeritageConverterFamily::MuLaw, 8.0f, 0.1f, 0.0f, 23,
                  SampleHeritageSeedPolicy::RestartFromProfileSeed}},
             {SampleHeritageBlockDomain::Voice, false,
              SampleHeritageVoiceHoldDroopBlock{
                  SampleHeritageHoldMode::ZeroOrder, 2, 0.001f}},
             {SampleHeritageBlockDomain::Voice, false,
              SampleHeritageVoiceReconstructionBlock{
                  SampleHeritageReconstructionFamily::Chebyshev,
                  SampleHeritageCutoffLaw::FixedHz, 9000.0, 4, 0.5f, 0.0f}}}),
        typed_profile(
            "neutral.shipping-novel-chain",
            {{SampleHeritageBlockDomain::Voice, false,
              SampleHeritageVoiceLiveCyclicStretchBlock{
                  1.25, 8.0, 1.0, true, 4, 41,
                  SampleHeritageSeedPolicy::RestartFromProfileSeed}},
             {SampleHeritageBlockDomain::Voice, false,
              SampleHeritageVoiceAnalogColorBlock{
                  1.2f, 0.08f, 0.65f,
                  SampleHeritageAnalogFilterFamily::Ladder4Pole,
                  SampleHeritageCutoffLaw::FixedHz, 7000.0, 0.2f}}}),
        typed_profile(
            "neutral.shipping-cross-domain-chain",
            {{SampleHeritageBlockDomain::Voice, false,
              SampleHeritageVoiceConverterBlock{
                  SampleHeritageConverterFamily::ALaw, 9.0f, 0.02f, 0.0f, 71,
                  SampleHeritageSeedPolicy::RestartFromProfileSeed}}},
            {{SampleHeritageBlockDomain::Bus, false,
              SampleHeritageBusNoiseIdleBlock{
                  .noise_amplitude = 0.001f,
                  .idle_amplitude = 0.0f,
                  .tilt_db_per_octave = -1.5f,
                  .gate = SampleHeritageNoiseGate::VoiceActive,
                  .seed = 81,
                  .seed_policy = SampleHeritageSeedPolicy::RestartFromProfileSeed}},
             {SampleHeritageBlockDomain::Bus, false,
              SampleHeritageBusOutputDriveBlock{1.1f, 0.9f}}},
            {{SampleHeritageBlockDomain::RecordCommit, false,
              SampleHeritageRecordInputDriveClipBlock{1.1f, 0.95f}},
             {SampleHeritageBlockDomain::RecordCommit, false,
              SampleHeritageRecordConverterBlock{
                  SampleHeritageConverterFamily::MuLaw, 8.0f, 0.05f, 0.0f, 91,
                  SampleHeritageSeedPolicy::RestartFromProfileSeed}},
             {SampleHeritageBlockDomain::RecordCommit, false,
              SampleHeritageRecordCommitAdaptiveStretchBlock{
                  .factor = 1.1, .decision_hop_samples = 16,
                  .search_radius_samples = 4, .search_stride_samples = 1,
                  .crossfade_samples = 4}}}),
    };

    for (const auto& profile : profiles) {
        const auto validated = validate_sample_heritage_profile(profile);
        REQUIRE(validated.valid());
        render_composition_once(profile, validated.profile);
    }

    const auto validated = validate_sample_heritage_profile(profiles[1]);
    REQUIRE(validated.valid());
    SampleHeritageEngine engine;
    REQUIRE(engine.prepare({validated.profile, 1, 128}) == SampleHeritagePrepareStatus::Ok);
    REQUIRE(engine.latency_output_frames() > 0.0);
    const auto stale = engine.plan_exact(64);
    REQUIRE(stale.valid());
    engine.reset();
    Buffer<float> stale_input(1, stale.input_frames);
    Buffer<float> stale_output(1, stale.output_frames);
    REQUIRE(engine.process_exact(stale, std::as_const(stale_input).view(),
                                 stale_output.view()) ==
            SampleHeritageProcessStatus::InvalidPlan);

    auto invalid = profiles[1];
    std::swap(invalid.voice[0], invalid.voice[1]);
    REQUIRE_FALSE(validate_sample_heritage_profile(invalid).valid());
}

TEST_CASE("Representative chain stays within the shipping CPU budget",
          "[audio][sampler][heritage][shipping-gate][performance]") {
    const auto chain = validate_sample_heritage_profile(performance_profile());
    REQUIRE(chain.valid());

#if defined(NDEBUG)
    constexpr double shipping_budget_ratio = 2.0;
    constexpr double measurement_tolerance = 1.05;
    std::array<double, 5> ratios{};
    for (std::size_t trial = 0; trial < ratios.size(); ++trial) {
        double baseline_seconds = 0.0;
        double chain_seconds = 0.0;
        if ((trial & 1u) == 0u) {
            baseline_seconds = measured_sampler_pipeline_seconds(nullptr);
            chain_seconds = measured_sampler_pipeline_seconds(&chain.profile);
        } else {
            chain_seconds = measured_sampler_pipeline_seconds(&chain.profile);
            baseline_seconds = measured_sampler_pipeline_seconds(nullptr);
        }
        REQUIRE(baseline_seconds > 0.0);
        ratios[trial] = chain_seconds / baseline_seconds;
    }
    std::sort(ratios.begin(), ratios.end());
    INFO("2x shipping budget includes a 5% wall-clock measurement tolerance");
    REQUIRE(ratios[ratios.size() / 2] <=
            shipping_budget_ratio * measurement_tolerance);
#else
    SUCCEED("Relative CPU budget is enforced by the Release shipping configuration");
#endif
}

TEST_CASE("Live cyclic work and storage remain bounded per output frame",
          "[audio][sampler][heritage][shipping-gate][performance]") {
    const SampleHeritageLiveCyclicConfig config{
        .factor = 1.75,
        .cycle_samples = 192,
        .crossfade_samples = 24,
        .shuffle_divisions = 4,
        .linked_channels = true,
        .seed = 0x517cc1b727220a95ULL,
        .shuffle = SampleHeritageLiveCyclicShuffle::FisherYates,
        .max_block_samples = 128,
        .channel_count = 2,
    };
    const auto resources = SampleHeritageLiveCyclicStretch::resources_for(config);
    REQUIRE(resources.valid());
    REQUIRE(resources.ring_capacity_frames < 4 * config.cycle_samples);
    REQUIRE(resources.maximum_input_frames <= config.max_block_samples +
                                                   config.cycle_samples);

#if defined(NDEBUG)
    REQUIRE(measured_live_scaling_ratio(config) <= 1.5);
#else
    SUCCEED("Live scaling is measured by the Release shipping configuration");
#endif
}
