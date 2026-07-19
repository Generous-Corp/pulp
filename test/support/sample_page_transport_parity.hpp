#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/sample_stream_window.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace pulp::test::sample_page_transport_parity {

struct IntegerTapSegment {
    std::uint64_t start_frame = 0;
    std::int64_t source_frames_per_output = 1;
    std::uint64_t output_frames = 0;
};

std::optional<std::vector<std::uint64_t>> make_integer_tap_schedule(
    std::uint64_t source_frames,
    std::span<const IntegerTapSegment> segments);

struct PageRenderCapture {
    audio::Buffer<float> output;
    std::uint64_t copied_taps = 0;
    std::uint64_t missed_taps = 0;
    std::uint64_t zero_filled_taps = 0;
    std::size_t callback_allocations = 0;
};

audio::Buffer<float> render_resident_integer_taps(
    const audio::Buffer<float>& source,
    std::span<const std::uint64_t> source_positions);

class PreparedPageTransport {
public:
    bool prepare(const audio::Buffer<float>& source,
                 std::uint64_t page_frames,
                 std::uint64_t stream_generation,
                 std::optional<std::uint32_t> omitted_page = std::nullopt);

    bool publish_page(std::uint32_t page_index);

    PageRenderCapture render_integer_taps(
        std::span<const std::uint64_t> source_positions,
        std::uint64_t stream_generation);

    const audio::SampleStreamWindow& window() const noexcept { return window_; }
    std::uint32_t page_count() const noexcept { return page_count_; }
    std::uint64_t page_frames() const noexcept { return page_frames_; }

private:
    audio::Buffer<float> source_;
    audio::SampleStreamWindow window_;
    std::uint64_t page_frames_ = 0;
    std::uint64_t stream_generation_ = 0;
    std::uint32_t page_count_ = 0;
};

}  // namespace pulp::test::sample_page_transport_parity
