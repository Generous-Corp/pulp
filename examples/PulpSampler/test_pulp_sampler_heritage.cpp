#include "pulp_sampler.hpp"
#include "rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

using namespace pulp;
using namespace pulp::examples;

namespace pulp::examples {

struct PulpSamplerHeritageTestAccess {
    static void fail_next_plan(PulpSamplerProcessor& processor) noexcept {
        processor.heritage_.fail_next_plan_for_test();
    }
};

}  // namespace pulp::examples

namespace {

audio::SampleHeritageProfile clock_profile(double ratio,
                                           bool bypass = false) {
    return {
        .schema_version = audio::kSampleHeritageProfileSchemaVersion,
        .profile_id = bypass ? "neutral.sampler-bypass-v2"
                             : "neutral.sampler-clock-v2",
        .host_sample_rate = 48000.0,
        .stages = {{bypass, audio::SampleHeritageClockPitchStage{ratio}}},
    };
}

audio::SampleHeritageProfile two_leg_profile() {
    return {
        .schema_version = audio::kSampleHeritageProfileSchemaVersion,
        .profile_id = "neutral.sampler-two-leg-v2",
        .host_sample_rate = 48000.0,
        .stages = {
            {false, audio::SampleHeritageMachineDomainStage{32000.0}},
            {false, audio::SampleHeritageClockPitchStage{1.25}},
        },
    };
}

std::vector<float> make_sine(std::size_t frames,
                             double frequency = 440.0,
                             double sample_rate = 48000.0) {
    std::vector<float> result(frames);
    constexpr double pi = 3.1415926535897932384626433832795;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        result[frame] = static_cast<float>(
            std::sin(2.0 * pi * frequency * static_cast<double>(frame) /
                     sample_rate));
    }
    return result;
}

struct HeritageFixture {
    state::StateStore store;
    PulpSamplerProcessor processor;
    std::uint32_t maximum_block_frames = 0;

    HeritageFixture(std::uint32_t maximum_frames,
                    const audio::SampleHeritageProfile* profile = nullptr)
        : maximum_block_frames(maximum_frames) {
        processor.set_state_store(&store);
        processor.define_parameters(store);
        store.set_value(kSamplerAttack, 0.0f);
        store.set_value(kSamplerDecay, 0.0f);
        store.set_value(kSamplerSustain, 100.0f);
        if (profile != nullptr) {
            REQUIRE(processor.set_heritage_profile(*profile) ==
                    PulpSamplerHeritageStatus::PendingPrepare);
        }
        format::PrepareContext context;
        context.sample_rate = 48000.0;
        context.max_buffer_size = static_cast<int>(maximum_frames);
        context.input_channels = 0;
        context.output_channels = 2;
        processor.prepare(context);
    }

    void load(std::span<const float> sample) {
        REQUIRE(processor.load_sample(sample.data(),
                                      static_cast<int>(sample.size()),
                                      48000.0f));
    }
};

std::vector<float> render(HeritageFixture& fixture,
                          std::span<const std::size_t> partitions,
                          std::size_t note_on_frame = 0) {
    std::vector<float> result;
    std::size_t absolute_frame = 0;
    bool note_sent = false;
    for (const auto frames : partitions) {
        std::vector<float> left(frames, 0.0f);
        std::vector<float> right(frames, 0.0f);
        float* output_ptrs[]{left.data(), right.data()};
        const float* input_ptrs[]{nullptr, nullptr};
        audio::BufferView<float> output(output_ptrs, 2, frames);
        audio::BufferView<const float> input(input_ptrs, 0, frames);
        midi::MidiBuffer midi_in;
        midi::MidiBuffer midi_out;
        if (!note_sent && note_on_frame >= absolute_frame &&
            note_on_frame <= absolute_frame + frames) {
            auto event = midi::MidiEvent::note_on(0, 60, 127);
            event.sample_offset =
                static_cast<std::int32_t>(note_on_frame - absolute_frame);
            midi_in.add(event);
            note_sent = true;
        }
        format::ProcessContext context{48000.0, static_cast<int>(frames)};
        fixture.processor.process(output, input, midi_in, midi_out, context);
        result.insert(result.end(), left.begin(), left.end());
        absolute_frame += frames;
    }
    return result;
}

double tone_projection(std::span<const float> samples,
                       double frequency,
                       double sample_rate) {
    constexpr double pi = 3.1415926535897932384626433832795;
    double sine = 0.0;
    double cosine = 0.0;
    for (std::size_t frame = 0; frame < samples.size(); ++frame) {
        const auto phase = 2.0 * pi * frequency *
                           static_cast<double>(frame) / sample_rate;
        sine += static_cast<double>(samples[frame]) * std::sin(phase);
        cosine += static_cast<double>(samples[frame]) * std::cos(phase);
    }
    return 2.0 * std::hypot(sine, cosine) /
           static_cast<double>(samples.size());
}

}  // namespace

TEST_CASE("PulpSampler heritage render is bitwise callback-partition invariant",
          "[audio][sampler][heritage][partition]") {
    const auto profile = two_leg_profile();
    auto sample = make_sine(48000);
    HeritageFixture whole(1024, &profile);
    HeritageFixture split(1024, &profile);
    whole.load(sample);
    split.load(sample);
    constexpr std::array one{std::size_t{1024}};
    constexpr std::array many{std::size_t{1}, std::size_t{31},
                              std::size_t{128}, std::size_t{7},
                              std::size_t{333}, std::size_t{524}};
    REQUIRE(std::accumulate(many.begin(), many.end(), std::size_t{0}) == 1024);
    REQUIRE(render(whole, one) == render(split, many));
}

TEST_CASE("PulpSampler heritage clock ratio raises sampler pitch",
          "[audio][sampler][heritage][pitch]") {
    const auto profile = clock_profile(2.0);
    auto sample = make_sine(48000);
    HeritageFixture fixture(4096, &profile);
    fixture.load(sample);
    constexpr std::array block{std::size_t{4096}};
    const auto output = render(fixture, block);
    const auto measured = std::span<const float>(output).subspan(256);
    REQUIRE(tone_projection(measured, 880.0, 48000.0) > 0.8);
    REQUIRE(tone_projection(measured, 880.0, 48000.0) >
            tone_projection(measured, 440.0, 48000.0) * 8.0);
}

TEST_CASE("PulpSampler heritage preserves host-offset MIDI causality",
          "[audio][sampler][heritage][midi]") {
    const auto profile = clock_profile(2.0);
    auto sample = make_sine(48000);
    HeritageFixture fixture(256, &profile);
    fixture.load(sample);
    constexpr std::array block{std::size_t{256}};
    const auto output = render(fixture, block, 64);
    REQUIRE(std::all_of(output.begin(), output.begin() + 64,
                        [](float sample_value) { return sample_value == 0.0f; }));
    REQUIRE(std::any_of(output.begin() + 64, output.end(),
                        [](float sample_value) {
                            return std::abs(sample_value) > 0.01f;
                        }));
}

TEST_CASE("PulpSampler heritage reports and renders causal impulse latency",
          "[audio][sampler][heritage][latency]") {
    const auto profile = clock_profile(2.0);
    std::vector<float> impulse(512, 0.0f);
    impulse[0] = 1.0f;
    HeritageFixture fixture(128, &profile);
    fixture.load(impulse);
    constexpr std::array block{std::size_t{128}};
    const auto output = render(fixture, block);
    const auto peak = static_cast<std::size_t>(std::distance(
        output.begin(), std::max_element(output.begin(), output.end(),
            [](float left, float right) {
                return std::abs(left) < std::abs(right);
            })));
    REQUIRE(fixture.processor.latency_samples() == 12);
    REQUIRE(peak == 12);
    REQUIRE(fixture.processor.descriptor().tail_samples == -1);
}

TEST_CASE("PulpSampler all-bypassed heritage uses the exact legacy render path",
          "[audio][sampler][heritage][bypass]") {
    const auto profile = clock_profile(2.0, true);
    auto sample = make_sine(48000);
    HeritageFixture disabled(512);
    HeritageFixture bypassed(512, &profile);
    disabled.load(sample);
    bypassed.load(sample);
    constexpr std::array blocks{std::size_t{17}, std::size_t{63},
                                std::size_t{128}, std::size_t{304}};
    REQUIRE(render(disabled, blocks) == render(bypassed, blocks));
    REQUIRE(bypassed.processor.latency_samples() == 0);
}

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
    fixture.processor.disable_heritage();
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
