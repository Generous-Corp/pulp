#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/streaming_sample_source.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace pulp::test::sampler_parity {

struct SequentialConfig {
    std::uint32_t source_channels = 2;
    std::uint32_t output_channels = 2;
    std::uint64_t total_frames = 0;
    std::uint64_t preload_frames = 0;
    std::uint64_t ring_capacity_frames = 0;
    std::uint64_t read_chunk_frames = 0;
};

struct RenderCapture {
    audio::Buffer<float> output;
    std::uint64_t produced_frames = 0;
    std::size_t callback_allocations = 0;
    audio::StreamingSampleSource::Stats stats;
    std::uint64_t final_position = 0;
    bool finished = false;
};

struct SequentialResult {
    audio::Buffer<float> source;
    RenderCapture resident_first;
    RenderCapture streamed_first;
    std::optional<RenderCapture> resident_after_reset;
    std::optional<RenderCapture> streamed_after_reset;
};

struct RawFloatMismatch {
    std::size_t channel = 0;
    std::size_t frame = 0;
    std::uint32_t expected_bits = 0;
    std::uint32_t actual_bits = 0;
};

struct RawFloatComparison {
    bool same_shape = false;
    bool nonzero_expected = false;
    std::uint64_t mismatch_count = 0;
    std::optional<RawFloatMismatch> first_mismatch;

    bool equal() const noexcept { return same_shape && mismatch_count == 0; }
    bool equal_nonvacuous() const noexcept { return equal() && nonzero_expected; }
};

audio::Buffer<float> make_deterministic_source(std::uint32_t channels,
                                               std::uint64_t frames);

RawFloatComparison compare_raw_float_bits(const audio::Buffer<float>& expected,
                                          const audio::Buffer<float>& actual);

std::optional<SequentialResult> render_sequential_parity(
    const SequentialConfig& config,
    std::span<const std::uint64_t> block_partition,
    bool render_after_reset);

}  // namespace pulp::test::sampler_parity
