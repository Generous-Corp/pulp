#include <catch2/catch_test_macros.hpp>

#include "support/sampler_parity.hpp"

#include <array>
#include <bit>
#include <cstdint>

namespace parity = pulp::test::sampler_parity;

namespace {

void require_complete_render(const parity::RenderCapture& capture,
                             std::uint64_t total_frames,
                             std::uint64_t streamed_frames) {
    REQUIRE(capture.produced_frames == total_frames);
    REQUIRE(capture.callback_allocations == 0);
    REQUIRE(capture.stats.underrun_frames == 0);
    REQUIRE(capture.stats.read_errors == 0);
    REQUIRE(capture.stats.streamed_frames == streamed_frames);
    REQUIRE_FALSE(capture.stats.streaming_active);
    REQUIRE(capture.final_position == total_frames);
    REQUIRE(capture.finished);
}

void require_stereo_parity(const parity::SequentialResult& result,
                           const parity::SequentialConfig& config) {
    const auto tail_frames = config.total_frames - config.preload_frames;
    require_complete_render(result.resident_first, config.total_frames, 0);
    require_complete_render(result.streamed_first, config.total_frames, tail_frames);

    REQUIRE(parity::compare_raw_float_bits(
                result.source, result.resident_first.output).equal_nonvacuous());
    REQUIRE(parity::compare_raw_float_bits(
                result.resident_first.output,
                result.streamed_first.output).equal_nonvacuous());

    REQUIRE(result.resident_after_reset.has_value());
    REQUIRE(result.streamed_after_reset.has_value());
    require_complete_render(*result.resident_after_reset, config.total_frames, 0);
    require_complete_render(*result.streamed_after_reset,
                            config.total_frames, tail_frames);
    REQUIRE(parity::compare_raw_float_bits(
                result.resident_first.output,
                result.resident_after_reset->output).equal());
    REQUIRE(parity::compare_raw_float_bits(
                result.streamed_first.output,
                result.streamed_after_reset->output).equal());
    REQUIRE(parity::compare_raw_float_bits(
                result.resident_after_reset->output,
                result.streamed_after_reset->output).equal());

    for (const auto frame : {config.preload_frames - 1, config.preload_frames}) {
        for (std::size_t channel = 0; channel < 2; ++channel) {
            REQUIRE(std::bit_cast<std::uint32_t>(
                        result.streamed_first.output.channel(channel)[frame]) ==
                    std::bit_cast<std::uint32_t>(
                        result.source.channel(channel)[frame]));
        }
    }
}

}  // namespace

TEST_CASE("Sequential streamed samples null against resident playback across block schedules",
          "[audio][sampler][streaming][parity]") {
    parity::SequentialConfig config;
    config.source_channels = 2;
    config.output_channels = 2;
    config.total_frames = 10007;
    config.preload_frames = 997;
    config.ring_capacity_frames = 1543;
    config.read_chunk_frames = 263;

    SECTION("fixed blocks cross preload and ring boundaries") {
        constexpr std::array<std::uint64_t, 1> blocks = {127};
        auto result = parity::render_sequential_parity(config, blocks, true);
        REQUIRE(result.has_value());
        require_stereo_parity(*result, config);
    }

    SECTION("one-frame callbacks prove the allocation contract over ten thousand calls") {
        constexpr std::array<std::uint64_t, 1> blocks = {1};
        auto result = parity::render_sequential_parity(config, blocks, false);
        REQUIRE(result.has_value());
        require_complete_render(result->resident_first, config.total_frames, 0);
        require_complete_render(result->streamed_first, config.total_frames,
                                config.total_frames - config.preload_frames);
        REQUIRE(parity::compare_raw_float_bits(
                    result->resident_first.output,
                    result->streamed_first.output).equal_nonvacuous());
    }

    SECTION("rugged blocks cross preload and ring boundaries") {
        constexpr std::array<std::uint64_t, 10> blocks = {
            1, 509, 7, 128, 3, 257, 64, 511, 2, 191,
        };
        auto result = parity::render_sequential_parity(config, blocks, true);
        REQUIRE(result.has_value());
        require_stereo_parity(*result, config);
    }
}

TEST_CASE("Sequential parity preserves the streaming source channel mapping",
          "[audio][sampler][streaming][parity][channels]") {
    constexpr std::array<std::uint64_t, 6> blocks = {251, 3, 127, 509, 11, 64};

    SECTION("mono source zero-fills a stereo destination") {
        parity::SequentialConfig config;
        config.source_channels = 1;
        config.output_channels = 2;
        config.total_frames = 4099;
        config.preload_frames = 257;
        config.ring_capacity_frames = 769;
        config.read_chunk_frames = 193;

        auto result = parity::render_sequential_parity(config, blocks, false);
        REQUIRE(result.has_value());
        require_complete_render(result->resident_first, config.total_frames, 0);
        require_complete_render(result->streamed_first, config.total_frames,
                                config.total_frames - config.preload_frames);
        REQUIRE(parity::compare_raw_float_bits(
                    result->resident_first.output,
                    result->streamed_first.output).equal());
        for (std::size_t frame = 0; frame < config.total_frames; ++frame) {
            REQUIRE(std::bit_cast<std::uint32_t>(
                        result->streamed_first.output.channel(0)[frame]) ==
                    std::bit_cast<std::uint32_t>(result->source.channel(0)[frame]));
            REQUIRE(std::bit_cast<std::uint32_t>(
                        result->streamed_first.output.channel(1)[frame]) == 0u);
        }
    }

    SECTION("stereo source supplies channel zero to a mono destination") {
        parity::SequentialConfig config;
        config.source_channels = 2;
        config.output_channels = 1;
        config.total_frames = 4099;
        config.preload_frames = 257;
        config.ring_capacity_frames = 769;
        config.read_chunk_frames = 193;

        auto result = parity::render_sequential_parity(config, blocks, false);
        REQUIRE(result.has_value());
        require_complete_render(result->resident_first, config.total_frames, 0);
        require_complete_render(result->streamed_first, config.total_frames,
                                config.total_frames - config.preload_frames);
        REQUIRE(parity::compare_raw_float_bits(
                    result->resident_first.output,
                    result->streamed_first.output).equal());
        REQUIRE(result->source.channel(0)[0] != 0.0f);
        REQUIRE(result->source.channel(1)[0] != 0.0f);
        REQUIRE(std::bit_cast<std::uint32_t>(result->source.channel(0)[0]) !=
                std::bit_cast<std::uint32_t>(result->source.channel(1)[0]));
        for (std::size_t frame = 0; frame < config.total_frames; ++frame) {
            REQUIRE(std::bit_cast<std::uint32_t>(
                        result->streamed_first.output.channel(0)[frame]) ==
                    std::bit_cast<std::uint32_t>(result->source.channel(0)[frame]));
        }
    }
}

TEST_CASE("Raw float parity comparator rejects bit, sign-zero, and shape changes",
          "[audio][sampler][parity][comparator]") {
    auto expected = parity::make_deterministic_source(2, 64);
    auto actual = expected;
    REQUIRE(parity::compare_raw_float_bits(expected, actual).equal());

    const auto original_bits = std::bit_cast<std::uint32_t>(actual.channel(1)[37]);
    actual.channel(1)[37] = std::bit_cast<float>(original_bits ^ 1u);
    auto changed_bit = parity::compare_raw_float_bits(expected, actual);
    REQUIRE_FALSE(changed_bit.equal());
    REQUIRE(changed_bit.same_shape);
    REQUIRE(changed_bit.mismatch_count == 1);
    REQUIRE(changed_bit.first_mismatch.has_value());
    REQUIRE(changed_bit.first_mismatch->channel == 1);
    REQUIRE(changed_bit.first_mismatch->frame == 37);
    REQUIRE(changed_bit.first_mismatch->expected_bits == original_bits);
    REQUIRE(changed_bit.first_mismatch->actual_bits == (original_bits ^ 1u));

    pulp::audio::Buffer<float> positive_zero(1, 1);
    auto negative_zero = positive_zero;
    negative_zero.channel(0)[0] = std::bit_cast<float>(0x80000000u);
    auto changed_zero_sign = parity::compare_raw_float_bits(positive_zero, negative_zero);
    REQUIRE_FALSE(changed_zero_sign.equal());
    REQUIRE(changed_zero_sign.mismatch_count == 1);

    pulp::audio::Buffer<float> wrong_shape(1, 64);
    auto changed_shape = parity::compare_raw_float_bits(expected, wrong_shape);
    REQUIRE_FALSE(changed_shape.equal());
    REQUIRE_FALSE(changed_shape.same_shape);

    pulp::audio::Buffer<float> silent(1, 64);
    auto vacuous = parity::compare_raw_float_bits(silent, silent);
    REQUIRE(vacuous.equal());
    REQUIRE_FALSE(vacuous.equal_nonvacuous());
}
