#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/sample_interpolation.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <array>
#include <cmath>
#include <type_traits>

using namespace pulp::audio;

static_assert(!std::is_copy_constructible_v<SampleSincKernel>);
static_assert(!std::is_move_constructible_v<SampleSincKernel>);
static_assert(!std::is_copy_constructible_v<SampleSincKernelBank>);
static_assert(!std::is_move_constructible_v<SampleSincKernelBank>);

TEST_CASE("Sample sinc kernels reject invalid table contracts",
          "[audio][sampler][interpolation][sinc]") {
    SampleSincKernel kernel;
    REQUIRE_FALSE(kernel.build({.half_width = 1}));
    REQUIRE_FALSE(kernel.build({.phases = 1}));
    REQUIRE_FALSE(kernel.build({.cutoff = 0.0}));
    REQUIRE_FALSE(kernel.build({.cutoff = 1.01}));

    SampleSincKernelBank bank;
    REQUIRE_FALSE(bank.build(0));
    REQUIRE_FALSE(bank.build(
        static_cast<std::uint32_t>(kMaximumSampleSincCutoffTables + 1)));

    static_assert(!std::is_aggregate_v<SampleSincKernelView>);
    const SampleSincKernelView empty;
    REQUIRE_FALSE(empty.valid());
    REQUIRE_FALSE((PreparedSampleInterpolation{
        .policy = SampleInterpolationPolicy::RatioTrackingSinc,
        .sinc = {empty, empty, 0.0f},
    }.valid()));
}

TEST_CASE("Sample sinc cutoff tables preserve DC across fractional phases",
          "[audio][sampler][interpolation][sinc]") {
    SampleSincKernelBank bank;
    REQUIRE(bank.build(4, 16, 128));
    const auto view = bank.view();
    REQUIRE(view.valid());
    std::array<float, 32> constant{};
    constant.fill(1.0f);
    for (std::uint32_t kernel = 0; kernel < view.kernel_count; ++kernel) {
        for (int phase = 0; phase < 128; ++phase) {
            const auto output = view.kernels[kernel].apply(
                constant, static_cast<double>(phase) / 128.0);
            REQUIRE(std::abs(output - 1.0f) < 2.0e-6f);
        }
        REQUIRE(std::abs(view.kernels[kernel].apply(constant, 1.0) - 1.0f) <
                2.0e-6f);
        REQUIRE(std::abs(view.kernels[kernel].apply(constant, 2.0) - 1.0f) <
                2.0e-6f);
    }

    SampleSincKernel endpoint_kernel;
    REQUIRE(endpoint_kernel.build({.half_width = 16, .phases = 512}));
    std::array<float, 32> endpoint_impulse{};
    endpoint_impulse[16] = 1.0f;
    REQUIRE(std::abs(endpoint_kernel.view().apply(endpoint_impulse, 1.0) - 1.0f) <
            2.0e-6f);
}

TEST_CASE("Sample sinc selection tracks consumption ratio continuously",
          "[audio][sampler][interpolation][sinc]") {
    SampleSincKernelBank bank;
    REQUIRE(bank.build(4, 16, 128));
    const auto view = bank.view();

    const auto unity = view.select(0.5);
    REQUIRE(unity.valid());
    REQUIRE(unity.wider.cutoff() == 1.0);
    REQUIRE(unity.narrower_gain == 0.0f);

    const auto between = view.select(std::sqrt(2.0));
    REQUIRE(between.valid());
    const auto blended_cutoff = between.wider.cutoff() +
        static_cast<double>(between.narrower_gain) *
            (between.narrower.cutoff() - between.wider.cutoff());
    REQUIRE(std::abs(blended_cutoff - 1.0 / std::sqrt(2.0)) < 1.0e-7);

    const auto narrow = view.select(8.0);
    REQUIRE(narrow.valid());
    REQUIRE(narrow.wider.cutoff() == 0.125);
    REQUIRE(narrow.narrower.cutoff() == 0.125);
    REQUIRE_FALSE(view.select(8.01).valid());

    const PreparedSampleInterpolation prepared{
        .policy = SampleInterpolationPolicy::RatioTrackingSinc,
        .sinc = view.select(4.0),
    };
    REQUIRE(prepared.guard_frames() == 16);
}

TEST_CASE("Sample sinc bank covers a declared maximum consumption ratio",
          "[audio][sampler][interpolation][sinc]") {
    SampleSincKernelBank bank;
    REQUIRE(bank.build_for_maximum_consumption(16.0, 16, 128));
    REQUIRE(bank.view().kernel_count == 5);
    REQUIRE(bank.view().select(16.0).valid());
    REQUIRE_FALSE(bank.view().select(16.01).valid());
    REQUIRE_FALSE(bank.build_for_maximum_consumption(129.0, 16, 128));
    REQUIRE_FALSE(bank.view().valid());
}

TEST_CASE("Ratio-tracking sinc rejects a known aliased downsample tone",
          "[audio][sampler][interpolation][sinc][spectral]") {
    SampleSincKernelBank bank;
    REQUIRE(bank.build(4, 16, 512));
    const auto selection = bank.view().select(4.0);
    REQUIRE(selection.valid());

    constexpr double pi = 3.1415926535897932384626433832795;
    constexpr double source_frequency = 0.4;
    double filtered_energy = 0.0;
    double unfiltered_energy = 0.0;
    std::array<float, 32> taps{};
    for (int output = 0; output < 256; ++output) {
        const auto source_position = output * 4;
        for (std::size_t tap = 0; tap < taps.size(); ++tap) {
            const auto source_frame = source_position +
                selection.wider.first_offset() + static_cast<int>(tap);
            taps[tap] = static_cast<float>(
                std::sin(2.0 * pi * source_frequency * source_frame));
        }
        const auto filtered = selection.apply(taps, 0.0);
        const auto unfiltered = static_cast<double>(
            std::sin(2.0 * pi * source_frequency * source_position));
        filtered_energy += static_cast<double>(filtered) * filtered;
        unfiltered_energy += unfiltered * unfiltered;
    }
    const auto filtered_rms = std::sqrt(filtered_energy / 256.0);
    const auto unfiltered_rms = std::sqrt(unfiltered_energy / 256.0);
    REQUIRE(unfiltered_rms > 0.6);
    REQUIRE(filtered_rms < 0.01);
}

TEST_CASE("Sample sinc selection and apply stay allocation free",
          "[audio][sampler][interpolation][sinc][rt]") {
    SampleSincKernelBank bank;
    REQUIRE(bank.build(4, 16, 128));
    const auto view = bank.view();
    std::array<float, 32> taps{};
    taps.fill(0.5f);
    float sink = 0.0f;

    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int iteration = 0; iteration < 10000; ++iteration) {
            const auto ratio = 1.0 + static_cast<double>(iteration % 300) / 100.0;
            const auto selection = view.select(ratio);
            sink += selection.apply(taps,
                                    static_cast<double>(iteration % 127) / 128.0);
        }
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
    REQUIRE(std::isfinite(sink));
}
