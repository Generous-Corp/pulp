#include "sample_page_transport_parity.hpp"

#include "../harness/rt_allocation_probe.hpp"

#include <algorithm>
#include <limits>

namespace pulp::test::sample_page_transport_parity {
namespace {

std::optional<std::uint64_t> next_source_frame(std::uint64_t current,
                                               std::int64_t step) {
    if (step >= 0) {
        const auto amount = static_cast<std::uint64_t>(step);
        if (current > std::numeric_limits<std::uint64_t>::max() - amount)
            return std::nullopt;
        return current + amount;
    }

    const auto amount = static_cast<std::uint64_t>(-(step + 1)) + 1;
    if (current < amount) return std::nullopt;
    return current - amount;
}

}  // namespace

std::optional<std::vector<std::uint64_t>> make_integer_tap_schedule(
    std::uint64_t source_frames,
    std::span<const IntegerTapSegment> segments) {
    if (source_frames == 0 || segments.empty()) return std::nullopt;

    std::uint64_t total_output_frames = 0;
    for (const auto& segment : segments) {
        if (segment.output_frames == 0 ||
            segment.output_frames > std::numeric_limits<std::size_t>::max() ||
            total_output_frames > std::numeric_limits<std::uint64_t>::max()
                                - segment.output_frames) {
            return std::nullopt;
        }
        total_output_frames += segment.output_frames;
    }
    if (total_output_frames > std::numeric_limits<std::size_t>::max())
        return std::nullopt;

    std::vector<std::uint64_t> positions;
    positions.reserve(static_cast<std::size_t>(total_output_frames));
    for (const auto& segment : segments) {
        auto source_frame = segment.start_frame;
        for (std::uint64_t output_frame = 0;
             output_frame < segment.output_frames;
             ++output_frame) {
            if (source_frame >= source_frames) return std::nullopt;
            positions.push_back(source_frame);
            if (output_frame + 1 == segment.output_frames) continue;
            auto next = next_source_frame(source_frame,
                                          segment.source_frames_per_output);
            if (!next) return std::nullopt;
            source_frame = *next;
        }
    }
    return positions;
}

audio::Buffer<float> render_resident_integer_taps(
    const audio::Buffer<float>& source,
    std::span<const std::uint64_t> source_positions) {
    audio::Buffer<float> output(source.num_channels(), source_positions.size());
    for (std::size_t output_frame = 0;
         output_frame < source_positions.size();
         ++output_frame) {
        const auto source_frame = static_cast<std::size_t>(source_positions[output_frame]);
        for (std::size_t channel = 0; channel < source.num_channels(); ++channel) {
            output.channel(channel)[output_frame] = source.channel(channel)[source_frame];
        }
    }
    return output;
}

bool PreparedPageTransport::prepare(
    const audio::Buffer<float>& source,
    std::uint64_t page_frames,
    std::uint64_t stream_generation,
    std::optional<std::uint32_t> omitted_page) {
    window_.release();
    source_ = {};
    page_frames_ = 0;
    stream_generation_ = 0;
    page_count_ = 0;

    if (source.num_channels() == 0 || source.num_samples() == 0 ||
        page_frames == 0 || stream_generation == 0) {
        return false;
    }

    const auto frames = static_cast<std::uint64_t>(source.num_samples());
    const auto pages = 1 + (frames - 1) / page_frames;
    if (pages > std::numeric_limits<std::uint32_t>::max() ||
        source.num_channels() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    const auto page_count = static_cast<std::uint32_t>(pages);
    if (omitted_page && *omitted_page >= page_count) return false;

    source_ = source;
    page_frames_ = page_frames;
    stream_generation_ = stream_generation;
    page_count_ = page_count;
    if (!window_.prepare({
            .channels = static_cast<std::uint32_t>(source.num_channels()),
            .page_count = page_count_,
            .page_frames = page_frames_,
        })) {
        return false;
    }

    for (std::uint32_t page = 0; page < page_count_; ++page) {
        if (!omitted_page || page != *omitted_page) {
            if (!publish_page(page)) {
                window_.release();
                return false;
            }
        }
    }
    return true;
}

bool PreparedPageTransport::publish_page(std::uint32_t page_index) {
    if (page_index >= page_count_ ||
        window_.page_state(page_index) != audio::SampleStreamPageState::Empty) {
        return false;
    }

    const auto start_frame = static_cast<std::uint64_t>(page_index) * page_frames_;
    const auto valid_frames = std::min(
        page_frames_,
        static_cast<std::uint64_t>(source_.num_samples()) - start_frame);
    if (!window_.begin_fill_page(page_index)) return false;
    const auto source_view = source_.view().slice(
        static_cast<std::size_t>(start_frame),
        static_cast<std::size_t>(valid_frames));
    if (!window_.copy_to_filling_page(page_index, source_view, valid_frames)) {
        window_.cancel_fill_page(page_index);
        return false;
    }
    if (!window_.publish_page(
            page_index,
            {
                .stream_generation = stream_generation_,
                .start_frame = start_frame,
                .valid_frames = valid_frames,
                .final_page = page_index + 1 == page_count_,
            })) {
        window_.cancel_fill_page(page_index);
        return false;
    }
    return true;
}

PageRenderCapture PreparedPageTransport::render_integer_taps(
    std::span<const std::uint64_t> source_positions,
    std::uint64_t stream_generation) {
    PageRenderCapture capture;
    capture.output.resize(source_.num_channels(), source_positions.size());

    RtAllocationProbe probe;
    for (std::size_t output_frame = 0;
         output_frame < source_positions.size();
         ++output_frame) {
        auto destination = capture.output.view().slice(output_frame, 1);
        const auto result = window_.read_frames(
            destination,
            {
                .stream_generation = stream_generation,
                .start_frame = source_positions[output_frame],
                .frames = 1,
            });
        capture.copied_taps += result.copied_frames;
        capture.missed_taps += result.missed_frames;
        capture.zero_filled_taps += result.zero_filled_frames;
    }
    capture.callback_allocations = probe.allocation_count();
    return capture;
}

}  // namespace pulp::test::sample_page_transport_parity
