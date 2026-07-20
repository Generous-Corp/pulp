#include <pulp/audio/sample_heritage_pitch.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

using namespace pulp::audio;

std::vector<float> make_source(std::size_t frames) {
    std::vector<float> result(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto position = static_cast<double>(frame);
        result[frame] = static_cast<float>(
            0.55 * std::sin(position * 0.071) +
            0.2 * std::cos(position * 0.019));
    }
    return result;
}

std::vector<float> render(SampleHeritagePitchFamily family,
                          double factor,
                          std::span<const std::size_t> partitions) {
    SampleHeritagePitchProcessor processor;
    REQUIRE(processor.prepare(family, factor, 1) == SampleHeritagePitchStatus::Ok);

    const auto source = make_source(4096);
    std::vector<float> result;
    std::size_t source_cursor = 0;
    for (const auto output_frames : partitions) {
        const auto plan = processor.plan(output_frames);
        REQUIRE(plan.valid());
        REQUIRE(source_cursor + plan.input_frames <= source.size());

        Buffer<float> input(1, plan.input_frames);
        Buffer<float> output(1, output_frames);
        std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(source_cursor),
                    plan.input_frames, input.channel(0).begin());
        const auto& const_input = input;
        REQUIRE(processor.process(const_input.view(), output.view()) ==
                SampleHeritagePitchStatus::Ok);
        result.insert(result.end(), output.channel(0).begin(), output.channel(0).end());
        source_cursor += plan.input_frames;
    }
    return result;
}

}  // namespace

TEST_CASE("heritage pitch families declare clock and source-domain routing",
          "[sample][heritage][pitch]") {
    SampleHeritagePitchProcessor processor;

    REQUIRE(processor.prepare(SampleHeritagePitchFamily::VariableClock, 0.75, 1) ==
            SampleHeritagePitchStatus::Ok);
    CHECK(processor.machine_clock_multiplier() == 0.75);
    CHECK(processor.source_frames_per_machine_frame() == 1.0);
    CHECK_FALSE(processor.fixed_clock_processing());

    REQUIRE(processor.prepare(SampleHeritagePitchFamily::DropRepeat, 0.75, 1) ==
            SampleHeritagePitchStatus::Ok);
    CHECK(processor.machine_clock_multiplier() == 1.0);
    CHECK(processor.source_frames_per_machine_frame() == 0.75);
    CHECK(processor.fixed_clock_processing());

    REQUIRE(processor.prepare(SampleHeritagePitchFamily::EarlyLinear, 1.25, 1) ==
            SampleHeritagePitchStatus::Ok);
    CHECK(processor.machine_clock_multiplier() == 1.0);
    CHECK(processor.source_frames_per_machine_frame() == 1.25);
    CHECK(processor.fixed_clock_processing());
}

TEST_CASE("heritage pitch factor one is an exact bypass for every family",
          "[sample][heritage][pitch][bypass]") {
    constexpr std::array families{
        SampleHeritagePitchFamily::VariableClock,
        SampleHeritagePitchFamily::DropRepeat,
        SampleHeritagePitchFamily::EarlyLinear,
    };
    constexpr std::array<std::uint32_t, 8> bits{
        0x00000000u, 0x80000000u, 0x3f800000u, 0xbf400000u,
        0x00800000u, 0x7f7fffffu, 0x7fc12345u, 0x3eaaaaabu,
    };

    for (const auto family : families) {
        SampleHeritagePitchProcessor processor;
        REQUIRE(processor.prepare(family, 1.0, 1) == SampleHeritagePitchStatus::Ok);
        CHECK(processor.exact_bypass());
        const auto plan = processor.plan(bits.size());
        REQUIRE(plan.valid());
        REQUIRE(plan.input_frames == bits.size());

        Buffer<float> input(1, bits.size());
        Buffer<float> output(1, bits.size());
        for (std::size_t frame = 0; frame < bits.size(); ++frame)
            input.channel(0)[frame] = std::bit_cast<float>(bits[frame]);
        const auto& const_input = input;
        REQUIRE(processor.process(const_input.view(), output.view()) ==
                SampleHeritagePitchStatus::Ok);
        for (std::size_t frame = 0; frame < bits.size(); ++frame)
            CHECK(std::bit_cast<std::uint32_t>(output.channel(0)[frame]) == bits[frame]);
    }
}

TEST_CASE("heritage variable clock consumes sequential machine-domain samples",
          "[sample][heritage][pitch]") {
    SampleHeritagePitchProcessor processor;
    REQUIRE(processor.prepare(SampleHeritagePitchFamily::VariableClock, 0.5, 1) ==
            SampleHeritagePitchStatus::Ok);
    const auto plan = processor.plan(8);
    REQUIRE(plan.valid());
    REQUIRE(plan.input_frames == 8);

    Buffer<float> input(1, 8);
    Buffer<float> output(1, 8);
    for (std::size_t frame = 0; frame < input.num_samples(); ++frame)
        input.channel(0)[frame] = static_cast<float>(frame) * 0.125f - 0.5f;
    const auto& const_input = input;
    REQUIRE(processor.process(const_input.view(), output.view()) ==
            SampleHeritagePitchStatus::Ok);
    CHECK(std::equal(input.channel(0).begin(), input.channel(0).end(),
                     output.channel(0).begin()));
    CHECK(processor.machine_clock_multiplier() == 0.5);
}

TEST_CASE("heritage drop repeat follows a zero-order source index oracle",
          "[sample][heritage][pitch]") {
    const std::array<std::size_t, 1> one_block{8};
    const auto slowed = render(SampleHeritagePitchFamily::DropRepeat, 0.5, one_block);
    const auto source = make_source(16);
    REQUIRE(slowed.size() == 8);
    for (std::size_t frame = 0; frame < slowed.size(); ++frame)
        CHECK(slowed[frame] == source[frame / 2]);

    const auto sped = render(SampleHeritagePitchFamily::DropRepeat, 2.0, one_block);
    REQUIRE(sped.size() == 8);
    for (std::size_t frame = 0; frame < sped.size(); ++frame)
        CHECK(sped[frame] == source[frame * 2]);
}

TEST_CASE("heritage early linear follows a two-point interpolation oracle",
          "[sample][heritage][pitch]") {
    SampleHeritagePitchProcessor processor;
    REQUIRE(processor.prepare(SampleHeritagePitchFamily::EarlyLinear, 0.5, 1) ==
            SampleHeritagePitchStatus::Ok);
    const auto plan = processor.plan(8);
    REQUIRE(plan.valid());
    REQUIRE(plan.input_frames == 5);

    Buffer<float> input(1, plan.input_frames);
    Buffer<float> output(1, 8);
    for (std::size_t frame = 0; frame < input.num_samples(); ++frame)
        input.channel(0)[frame] = static_cast<float>(frame);
    const auto& const_input = input;
    REQUIRE(processor.process(const_input.view(), output.view()) ==
            SampleHeritagePitchStatus::Ok);
    for (std::size_t frame = 0; frame < output.num_samples(); ++frame)
        CHECK(output.channel(0)[frame] == static_cast<float>(frame) * 0.5f);
}

TEST_CASE("heritage pitch processing is callback partition invariant",
          "[sample][heritage][pitch][partition]") {
    constexpr std::array<std::size_t, 1> contiguous{257};
    constexpr std::array<std::size_t, 9> partitioned{17, 1, 63, 5, 32, 2, 71, 11, 55};

    for (const auto family : {SampleHeritagePitchFamily::DropRepeat,
                              SampleHeritagePitchFamily::EarlyLinear}) {
        const auto whole = render(family, 0.73125, contiguous);
        const auto split = render(family, 0.73125, partitioned);
        REQUIRE(split.size() == whole.size());
        CHECK(std::equal(whole.begin(), whole.end(), split.begin()));
    }

    const auto variable_whole =
        render(SampleHeritagePitchFamily::VariableClock, 1.375, contiguous);
    const auto variable_split =
        render(SampleHeritagePitchFamily::VariableClock, 1.375, partitioned);
    CHECK(variable_split == variable_whole);
}

TEST_CASE("heritage fixed-clock pitch automation preserves its source cursor",
          "[sample][heritage][pitch][partition]") {
    SampleHeritagePitchProcessor processor;
    REQUIRE(processor.prepare(SampleHeritagePitchFamily::DropRepeat, 1.0, 1) ==
            SampleHeritagePitchStatus::Ok);
    Buffer<float> first_input(1, 8);
    Buffer<float> first_output(1, 8);
    for (std::size_t frame = 0; frame < 8; ++frame)
        first_input.channel(0)[frame] = static_cast<float>(frame);
    const auto& const_first_input = first_input;
    REQUIRE(processor.process(const_first_input.view(), first_output.view()) ==
            SampleHeritagePitchStatus::Ok);

    REQUIRE(processor.set_factor(0.5) == SampleHeritagePitchStatus::Ok);
    const auto plan = processor.plan(4);
    REQUIRE(plan.valid());
    Buffer<float> second_input(1, plan.input_frames);
    Buffer<float> second_output(1, 4);
    for (std::size_t frame = 0; frame < plan.input_frames; ++frame)
        second_input.channel(0)[frame] = static_cast<float>(8 + frame);
    const auto& const_second_input = second_input;
    REQUIRE(processor.process(const_second_input.view(), second_output.view()) ==
            SampleHeritagePitchStatus::Ok);
    CHECK(second_output.channel(0)[0] == 8.0f);
    CHECK(second_output.channel(0)[1] == 8.0f);
    CHECK(second_output.channel(0)[2] == 9.0f);
    CHECK(second_output.channel(0)[3] == 9.0f);
}

TEST_CASE("heritage pitch rejects invalid calls without advancing state",
          "[sample][heritage][pitch]") {
    SampleHeritagePitchProcessor processor;
    CHECK(processor.prepare(SampleHeritagePitchFamily::DropRepeat, 0.0, 1) ==
          SampleHeritagePitchStatus::InvalidConfiguration);
    CHECK_FALSE(processor.prepared());
    CHECK(processor.prepare(SampleHeritagePitchFamily::DropRepeat, 0.75, 1) ==
          SampleHeritagePitchStatus::Ok);

    const auto plan = processor.plan(16);
    REQUIRE(plan.valid());
    Buffer<float> wrong_input(1, plan.input_frames + 1);
    Buffer<float> output(1, 16);
    const auto& const_wrong_input = wrong_input;
    CHECK(processor.process(const_wrong_input.view(), output.view()) ==
          SampleHeritagePitchStatus::InputFrameMismatch);

    const auto source = make_source(plan.input_frames);
    Buffer<float> input(1, plan.input_frames);
    std::copy(source.begin(), source.end(), input.channel(0).begin());
    const auto& const_input = input;
    CHECK(processor.process(const_input.view(), output.view()) ==
          SampleHeritagePitchStatus::Ok);
}

TEST_CASE("heritage pitch process performs no realtime allocation",
          "[sample][heritage][pitch][rt-safety]") {
    SampleHeritagePitchProcessor processor;
    REQUIRE(processor.prepare(SampleHeritagePitchFamily::EarlyLinear, 0.75, 2) ==
            SampleHeritagePitchStatus::Ok);
    const auto plan = processor.plan(64);
    REQUIRE(plan.valid());
    Buffer<float> input(2, plan.input_frames);
    Buffer<float> output(2, 64);
    const auto& const_input = input;

    SampleHeritagePitchStatus status{};
    bool saw_allocation = false;
    {
        pulp::test::RtAllocationProbe probe;
        status = processor.process(const_input.view(), output.view());
        saw_allocation = probe.saw_allocation();
    }
    CHECK(status == SampleHeritagePitchStatus::Ok);
    CHECK_FALSE(saw_allocation);
}
