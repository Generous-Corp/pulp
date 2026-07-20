#include <pulp/audio/sample_heritage_live_cyclic.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace {

using namespace pulp::audio;

std::vector<float> source_signal(std::size_t frames, std::size_t channel = 0) {
    std::vector<float> result(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        result[frame] = static_cast<float>(
            0.42 * std::sin(static_cast<double>(frame) * 0.031 + channel * 0.17) +
            0.19 * std::cos(static_cast<double>(frame) * 0.007));
    }
    return result;
}

std::vector<float> render(const SampleHeritageLiveCyclicConfig& config,
                          std::span<const std::size_t> partitions,
                          std::size_t selected_channel = 0) {
    SampleHeritageLiveCyclicStretch processor;
    REQUIRE(processor.prepare(config) == SampleHeritageLiveCyclicStatus::Ok);
    std::vector<std::vector<float>> sources;
    for (std::size_t channel = 0; channel < config.channel_count; ++channel)
        sources.push_back(source_signal(32768, channel));
    std::vector<float> result;
    std::size_t source_cursor = 0;
    for (const auto frames : partitions) {
        const auto plan = processor.plan(frames);
        REQUIRE(plan.valid());
        REQUIRE(source_cursor + plan.input_frames <= sources.front().size());
        Buffer<float> input(config.channel_count, plan.input_frames);
        Buffer<float> output(config.channel_count, frames);
        for (std::size_t channel = 0; channel < config.channel_count; ++channel)
            std::copy_n(sources[channel].begin() + static_cast<std::ptrdiff_t>(source_cursor),
                        plan.input_frames, input.channel(channel).begin());
        const auto& const_input = input;
        REQUIRE(processor.process(const_input.view(), output.view()) ==
                SampleHeritageLiveCyclicStatus::Ok);
        result.insert(result.end(), output.channel(selected_channel).begin(),
                      output.channel(selected_channel).end());
        source_cursor += plan.input_frames;
    }
    return result;
}

SampleHeritageLiveCyclicConfig shuffled_config() {
    SampleHeritageLiveCyclicConfig config;
    config.factor = 1.6;
    config.cycle_samples = 96;
    config.crossfade_samples = 12;
    config.shuffle_divisions = 8;
    config.linked_channels = true;
    config.seed = 0x123456789abcdef0ULL;
    config.shuffle = SampleHeritageLiveCyclicShuffle::FisherYates;
    config.max_block_samples = 256;
    config.channel_count = 2;
    return config;
}

} // namespace

TEST_CASE("live cyclic factor one is a bit-exact zero-latency bypass",
          "[sample][heritage][live-cyclic]") {
    auto config = shuffled_config();
    config.factor = 1.0;
    const std::array<std::size_t, 4> partitions{17, 64, 3, 91};
    const auto output = render(config, partitions);
    const auto source = source_signal(output.size());
    REQUIRE(output.size() == source.size());
    for (std::size_t frame = 0; frame < output.size(); ++frame)
        CHECK(std::bit_cast<std::uint32_t>(output[frame]) ==
              std::bit_cast<std::uint32_t>(source[frame]));
}

TEST_CASE("live cyclic output is invariant to callback partitioning and ring wrap",
          "[sample][heritage][live-cyclic]") {
    auto config = shuffled_config();
    config.max_block_samples = 256;
    std::vector<std::size_t> small(128, 13);
    std::vector<std::size_t> mixed;
    std::size_t remaining = 128 * 13;
    for (const auto width : {256u, 7u, 111u, 32u}) {
        while (remaining >= width) {
            mixed.push_back(width);
            remaining -= width;
        }
    }
    if (remaining != 0)
        mixed.push_back(remaining);
    const auto first = render(config, small);
    const auto second = render(config, mixed);
    REQUIRE(first.size() == second.size());
    for (std::size_t frame = 0; frame < first.size(); ++frame)
        CHECK(first[frame] == second[frame]);
}

TEST_CASE("live cyclic planning follows rounded cycle anchors and sustained source ratio",
          "[sample][heritage][live-cyclic]") {
    SampleHeritageLiveCyclicConfig config;
    config.factor = 1.6;
    config.cycle_samples = 64;
    config.crossfade_samples = 8;
    config.max_block_samples = 64;
    config.channel_count = 1;
    SampleHeritageLiveCyclicStretch processor;
    REQUIRE(processor.prepare(config) == SampleHeritageLiveCyclicStatus::Ok);
    CHECK(processor.source_frames_per_output_frame() == 0.625);
    CHECK(processor.cold_lookahead_frames() == 74);
    auto plan = processor.plan(64);
    REQUIRE(plan.valid());
    CHECK(plan.input_frames == 74);
    Buffer<float> input(1, plan.input_frames);
    Buffer<float> output(1, 64);
    const auto& const_input = input;
    REQUIRE(processor.process(const_input.view(), output.view()) ==
            SampleHeritageLiveCyclicStatus::Ok);
    plan = processor.plan(1);
    REQUIRE(plan.valid());
    CHECK(plan.input_frames == 32);
}

TEST_CASE("live cyclic validates bounds and reports exact prepared storage",
          "[sample][heritage][live-cyclic]") {
    SampleHeritageLiveCyclicConfig config;
    config.factor = 0.5;
    config.cycle_samples = 64;
    config.crossfade_samples = 8;
    config.shuffle_divisions = 4;
    config.max_block_samples = 32;
    config.channel_count = 2;
    const auto resources = SampleHeritageLiveCyclicStretch::resources_for(config);
    REQUIRE(resources.valid());
    CHECK(resources.maximum_input_frames == 202);
    CHECK(resources.ring_capacity_frames == 512);
    CHECK(resources.persistent_bytes ==
          512 * 2 * sizeof(float) + 2 * 4 * sizeof(std::uint32_t));
    CHECK(resources.scratch_bytes == 0);

    config.crossfade_samples = 33;
    CHECK_FALSE(SampleHeritageLiveCyclicStretch::resources_for(config).valid());
    config.crossfade_samples = 8;
    config.shuffle_divisions = 65;
    CHECK_FALSE(SampleHeritageLiveCyclicStretch::resources_for(config).valid());
    config.shuffle_divisions = 4;
    config.factor = 0.0;
    CHECK_FALSE(SampleHeritageLiveCyclicStretch::resources_for(config).valid());

    config.factor = kSampleHeritageCyclicStretchMaximumFactor;
    CHECK(SampleHeritageLiveCyclicStretch::resources_for(config).valid());
    config.factor = kSampleHeritageCyclicStretchMaximumFactor + 0.01;
    CHECK_FALSE(SampleHeritageLiveCyclicStretch::resources_for(config).valid());
    config.factor = kSampleHeritageCyclicStretchMinimumFactor;
    CHECK(SampleHeritageLiveCyclicStretch::resources_for(config).valid());
    config.factor = kSampleHeritageCyclicStretchMinimumFactor - 0.01;
    CHECK_FALSE(SampleHeritageLiveCyclicStretch::resources_for(config).valid());

    config.factor = 0.5;
    SampleHeritageLiveCyclicStretch processor;
    REQUIRE(processor.prepare(config) == SampleHeritageLiveCyclicStatus::Ok);
    CHECK(processor.plan(config.max_block_samples + 1).status ==
          SampleHeritageLiveCyclicStatus::InvalidDimensions);
    const auto plan = processor.plan(8);
    Buffer<float> short_input(config.channel_count, plan.input_frames - 1);
    Buffer<float> output(config.channel_count, 8);
    const auto& const_short_input = short_input;
    CHECK(processor.process(const_short_input.view(), output.view()) ==
          SampleHeritageLiveCyclicStatus::InputFrameMismatch);
}

TEST_CASE("live cyclic resource bounds survive repeated fast cycle boundaries",
          "[sample][heritage][live-cyclic][resources]") {
    for (const auto factor : {0.5, 0.25}) {
        SampleHeritageLiveCyclicConfig config;
        config.factor = factor;
        config.cycle_samples = 480;
        config.crossfade_samples = 48;
        config.max_block_samples = 64;
        config.channel_count = 1;
        SampleHeritageLiveCyclicStretch processor;
        REQUIRE(processor.prepare(config) == SampleHeritageLiveCyclicStatus::Ok);
        const auto resources = processor.resources();
        REQUIRE(resources.valid());

        std::size_t source_cursor = 0;
        for (std::size_t block = 0; block < 48; ++block) {
            const auto plan = processor.plan(config.max_block_samples);
            REQUIRE(plan.valid());
            REQUIRE(plan.input_frames <= resources.maximum_input_frames);
            Buffer<float> input(1, plan.input_frames);
            Buffer<float> output(1, config.max_block_samples);
            for (std::size_t frame = 0; frame < plan.input_frames; ++frame)
                input.channel(0)[frame] = static_cast<float>(source_cursor + frame);
            REQUIRE(processor.process(std::as_const(input).view(), output.view()) ==
                    SampleHeritageLiveCyclicStatus::Ok);
            source_cursor += plan.input_frames;
        }
    }
}

TEST_CASE("live cyclic ring retains old-cycle reads across arbitrary block partitions",
          "[sample][heritage][live-cyclic][resources]") {
    SampleHeritageLiveCyclicConfig config;
    config.factor = 0.5;
    config.cycle_samples = 960;
    config.crossfade_samples = 8;
    config.max_block_samples = 257;
    config.channel_count = 1;
    SampleHeritageLiveCyclicStretch processor;
    REQUIRE(processor.prepare(config) == SampleHeritageLiveCyclicStatus::Ok);

    constexpr std::array<std::size_t, 12> partitions{
        257, 257, 257, 257, 31, 211, 89, 257, 13, 257, 173, 257};
    std::size_t source_cursor = 0;
    for (const auto frames : partitions) {
        const auto plan = processor.plan(frames);
        REQUIRE(plan.valid());
        REQUIRE(plan.input_frames <= processor.resources().maximum_input_frames);
        Buffer<float> input(1, plan.input_frames);
        Buffer<float> output(1, frames);
        for (std::size_t frame = 0; frame < plan.input_frames; ++frame)
            input.channel(0)[frame] = static_cast<float>(source_cursor + frame);
        REQUIRE(processor.process(std::as_const(input).view(), output.view()) ==
                SampleHeritageLiveCyclicStatus::Ok);
        source_cursor += plan.input_frames;
    }
}

TEST_CASE("live cyclic marker ramp follows cycle anchors and previous-origin crossfades",
          "[sample][heritage][live-cyclic]") {
    SampleHeritageLiveCyclicConfig config;
    config.factor = 0.5;
    config.cycle_samples = 8;
    config.crossfade_samples = 4;
    config.max_block_samples = 16;
    config.channel_count = 1;
    SampleHeritageLiveCyclicStretch processor;
    REQUIRE(processor.prepare(config) == SampleHeritageLiveCyclicStatus::Ok);
    const auto plan = processor.plan(16);
    Buffer<float> input(1, plan.input_frames);
    Buffer<float> output(1, 16);
    for (std::size_t frame = 0; frame < input.num_samples(); ++frame)
        input.channel(0)[frame] = static_cast<float>(frame);
    const auto& const_input = input;
    REQUIRE(processor.process(const_input.view(), output.view()) ==
            SampleHeritageLiveCyclicStatus::Ok);

    for (std::size_t phase = 0; phase < config.cycle_samples; ++phase)
        CHECK(output.channel(0)[phase] == static_cast<float>(phase));
    CHECK(output.channel(0)[8] == 8.0f);
    CHECK(output.channel(0)[11] == 19.0f);
    CHECK(output.channel(0)[12] == 20.0f);
    CHECK(output.channel(0)[13] - output.channel(0)[12] == 1.0f);

    config.factor = 2.0;
    REQUIRE(processor.prepare(config) == SampleHeritageLiveCyclicStatus::Ok);
    const auto slow_plan = processor.plan(16);
    Buffer<float> slow_input(1, slow_plan.input_frames);
    for (std::size_t frame = 0; frame < slow_input.num_samples(); ++frame)
        slow_input.channel(0)[frame] = static_cast<float>(frame);
    const auto& const_slow_input = slow_input;
    REQUIRE(processor.process(const_slow_input.view(), output.view()) ==
            SampleHeritageLiveCyclicStatus::Ok);
    CHECK(output.channel(0)[8] == 8.0f);
    CHECK(output.channel(0)[11] == 7.0f);
    CHECK(output.channel(0)[12] == 8.0f);
}

TEST_CASE("live cyclic nonunit stretch retains within-grain sine frequency",
          "[sample][heritage][live-cyclic]") {
    SampleHeritageLiveCyclicConfig config;
    config.factor = 0.5;
    config.cycle_samples = 128;
    config.crossfade_samples = 8;
    config.max_block_samples = 64;
    config.channel_count = 1;
    SampleHeritageLiveCyclicStretch processor;
    REQUIRE(processor.prepare(config) == SampleHeritageLiveCyclicStatus::Ok);
    const auto plan = processor.plan(64);
    Buffer<float> input(1, plan.input_frames);
    Buffer<float> output(1, 64);
    for (std::size_t frame = 0; frame < input.num_samples(); ++frame)
        input.channel(0)[frame] = static_cast<float>(std::sin(frame * 0.17));
    const auto& const_input = input;
    REQUIRE(processor.process(const_input.view(), output.view()) ==
            SampleHeritageLiveCyclicStatus::Ok);
    for (std::size_t frame = 0; frame < output.num_samples(); ++frame)
        CHECK(output.channel(0)[frame] == input.channel(0)[frame]);
}

TEST_CASE("live cyclic shuffled divisions retain unit-speed marker slopes",
          "[sample][heritage][live-cyclic]") {
    SampleHeritageLiveCyclicConfig config;
    config.factor = 2.0;
    config.cycle_samples = 32;
    config.crossfade_samples = 2;
    config.shuffle_divisions = 4;
    config.seed = 91;
    config.shuffle = SampleHeritageLiveCyclicShuffle::FisherYates;
    config.max_block_samples = 32;
    config.channel_count = 1;
    SampleHeritageLiveCyclicStretch processor;
    REQUIRE(processor.prepare(config) == SampleHeritageLiveCyclicStatus::Ok);
    const auto plan = processor.plan(32);
    Buffer<float> input(1, plan.input_frames);
    Buffer<float> output(1, 32);
    for (std::size_t frame = 0; frame < input.num_samples(); ++frame)
        input.channel(0)[frame] = static_cast<float>(frame);
    const auto& const_input = input;
    REQUIRE(processor.process(const_input.view(), output.view()) ==
            SampleHeritageLiveCyclicStatus::Ok);
    for (std::size_t division = 0; division < config.shuffle_divisions; ++division) {
        const auto probe = division * 8 + 3;
        CHECK(output.channel(0)[probe + 1] - output.channel(0)[probe] == 1.0f);
    }
}

TEST_CASE("live cyclic Fisher-Yates streams are deterministic and linkable",
          "[sample][heritage][live-cyclic]") {
    auto config = shuffled_config();
    SampleHeritageLiveCyclicStretch first;
    SampleHeritageLiveCyclicStretch second;
    REQUIRE(first.prepare(config) == SampleHeritageLiveCyclicStatus::Ok);
    REQUIRE(second.prepare(config) == SampleHeritageLiveCyclicStatus::Ok);
    std::array<std::uint32_t, 8> a{};
    std::array<std::uint32_t, 8> b{};
    std::array<std::uint32_t, 8> linked{};
    REQUIRE(first.division_permutation(7, 0, a));
    REQUIRE(second.division_permutation(7, 0, b));
    REQUIRE(first.division_permutation(7, 1, linked));
    CHECK(a == b);
    CHECK(a == linked);
    std::sort(b.begin(), b.end());
    CHECK(b == std::array<std::uint32_t, 8>{0, 1, 2, 3, 4, 5, 6, 7});

    config.linked_channels = false;
    SampleHeritageLiveCyclicStretch unlinked;
    REQUIRE(unlinked.prepare(config) == SampleHeritageLiveCyclicStatus::Ok);
    REQUIRE(unlinked.division_permutation(7, 0, a));
    REQUIRE(unlinked.division_permutation(7, 1, b));
    CHECK(a != b);
    const auto continuation = unlinked.capture_next_cycle_rng_continuation();
    CHECK(continuation.seed == config.seed);
    CHECK(continuation.next_cycle_index == 0);

    unlinked.reset_with_rng_continuation({config.seed, 7});
    REQUIRE(unlinked.division_permutation(0, 0, b));
    REQUIRE(first.division_permutation(7, 0, a));
    CHECK(a == b);
}

TEST_CASE("live cyclic simultaneous instances have independent stream state",
          "[sample][heritage][live-cyclic]") {
    auto config = shuffled_config();
    const std::array<std::size_t, 4> blocks{31, 17, 64, 9};
    const auto reference = render(config, blocks);
    auto changed = config;
    changed.seed += 1;
    const auto other = render(changed, blocks);
    const auto repeated = render(config, blocks);
    CHECK(reference == repeated);
    CHECK(reference != other);
}

TEST_CASE("live cyclic silence DC and impulse renders remain finite",
          "[sample][heritage][live-cyclic]") {
    auto config = shuffled_config();
    config.channel_count = 1;
    SampleHeritageLiveCyclicStretch processor;
    REQUIRE(processor.prepare(config) == SampleHeritageLiveCyclicStatus::Ok);
    const auto plan = processor.plan(192);
    REQUIRE(plan.valid());
    Buffer<float> input(1, plan.input_frames);
    Buffer<float> output(1, 192);
    const auto& const_input = input;
    REQUIRE(processor.process(const_input.view(), output.view()) ==
            SampleHeritageLiveCyclicStatus::Ok);
    for (const auto sample : output.channel(0))
        CHECK(sample == 0.0f);

    processor.reset();
    const auto dc_plan = processor.plan(192);
    Buffer<float> dc(1, dc_plan.input_frames);
    std::fill(dc.channel(0).begin(), dc.channel(0).end(), 0.25f);
    const auto& const_dc = dc;
    REQUIRE(processor.process(const_dc.view(), output.view()) ==
            SampleHeritageLiveCyclicStatus::Ok);
    for (const auto sample : output.channel(0))
        CHECK(sample == 0.25f);

    processor.reset();
    const auto impulse_plan = processor.plan(192);
    Buffer<float> impulse(1, impulse_plan.input_frames);
    impulse.channel(0)[0] = 1.0f;
    const auto& const_impulse = impulse;
    REQUIRE(processor.process(const_impulse.view(), output.view()) ==
            SampleHeritageLiveCyclicStatus::Ok);
    for (const auto sample : output.channel(0))
        CHECK(std::isfinite(sample));
}

TEST_CASE("live cyclic prepared processing performs no allocation",
          "[sample][heritage][live-cyclic][rt-safety]") {
    auto config = shuffled_config();
    SampleHeritageLiveCyclicStretch processor;
    REQUIRE(processor.prepare(config) == SampleHeritageLiveCyclicStatus::Ok);
    const auto plan = processor.plan(64);
    REQUIRE(plan.valid());
    Buffer<float> input(config.channel_count, plan.input_frames);
    Buffer<float> output(config.channel_count, 64);
    const auto& const_input = input;
    SampleHeritageLiveCyclicStatus status{};
    bool allocated = false;
    {
        pulp::test::RtAllocationProbe probe;
        status = processor.process(const_input.view(), output.view());
        allocated = probe.saw_allocation();
    }
    CHECK(status == SampleHeritageLiveCyclicStatus::Ok);
    CHECK_FALSE(allocated);
}

TEST_CASE("live cyclic finite sources end at rounded stretched duration",
          "[sample][heritage][live-cyclic][eof]") {
    SampleHeritageLiveCyclicConfig config;
    config.factor = 2.0;
    config.cycle_samples = 32;
    config.crossfade_samples = 4;
    config.max_block_samples = 128;
    config.channel_count = 1;
    SampleHeritageLiveCyclicStretch processor;
    REQUIRE(processor.prepare(config) == SampleHeritageLiveCyclicStatus::Ok);
    const auto plan = processor.plan(128);
    REQUIRE(plan.valid());
    Buffer<float> input(1, plan.input_frames);
    Buffer<float> output(1, 128);
    std::fill(input.channel(0).begin(), input.channel(0).end(), 0.25f);
    const auto& const_input = input;
    REQUIRE(processor.process(const_input.view(), output.view(), 37, true) ==
            SampleHeritageLiveCyclicStatus::Ok);
    CHECK(processor.last_valid_output_frames() == 74);
    CHECK(processor.remaining_output_frames() == 0);
    CHECK(std::all_of(output.channel(0).begin() + 74,
                      output.channel(0).end(),
                      [](float sample) { return sample == 0.0f; }));
}
