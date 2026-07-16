#include "sampler_parity.hpp"

#include "../harness/rt_allocation_probe.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <utility>

namespace pulp::test::sampler_parity {
namespace {

float deterministic_sample(std::uint64_t frame, std::uint32_t channel) {
    std::uint64_t value = frame + 0x9E3779B97F4A7C15ull
                        + static_cast<std::uint64_t>(channel) * 0xD1B54A32D192ED03ull;
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ull;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBull;
    value ^= value >> 31;

    const std::uint32_t magnitude_bits = 0x3E800000u
                                       | (static_cast<std::uint32_t>(value) & 0x003FFFFFu);
    const std::uint32_t sign_bit = static_cast<std::uint32_t>((value >> 63) << 31);
    return std::bit_cast<float>(magnitude_bits | sign_bit);
}

audio::FrameReader make_buffer_reader(const audio::Buffer<float>& source) {
    return [&source](std::uint64_t start,
                     audio::BufferView<float> destination,
                     std::uint64_t frames) -> std::uint64_t {
        const std::uint64_t available = start >= source.num_samples()
            ? 0
            : static_cast<std::uint64_t>(source.num_samples()) - start;
        const std::uint64_t count = std::min(frames, available);
        const auto channels = std::min(source.num_channels(), destination.num_channels());
        for (std::size_t channel = 0; channel < channels; ++channel) {
            std::memcpy(destination.channel_ptr(channel),
                        source.channel(channel).data() + static_cast<std::size_t>(start),
                        static_cast<std::size_t>(count) * sizeof(float));
        }
        return count;
    };
}

RenderCapture render_source(audio::StreamingSampleSource& source,
                            std::uint32_t output_channels,
                            std::uint64_t total_frames,
                            std::span<const std::uint64_t> block_partition) {
    RenderCapture capture;
    capture.output.resize(output_channels, static_cast<std::size_t>(total_frames));

    std::uint64_t offset = 0;
    std::size_t partition_index = 0;
    while (offset < total_frames) {
        while (source.pump_background() > 0) {}

        const auto requested = std::min(block_partition[partition_index],
                                        total_frames - offset);
        auto destination = capture.output.view().slice(
            static_cast<std::size_t>(offset), static_cast<std::size_t>(requested));
        std::uint64_t produced = 0;
        {
            RtAllocationProbe probe;
            produced = source.pull(destination, requested);
            capture.callback_allocations += probe.allocation_count();
        }
        capture.produced_frames += produced;
        offset += requested;
        partition_index = (partition_index + 1) % block_partition.size();
    }

    capture.stats = source.stats();
    capture.final_position = source.position();
    capture.finished = source.finished();
    return capture;
}

bool valid_config(const SequentialConfig& config,
                  std::span<const std::uint64_t> block_partition) {
    if (config.source_channels == 0 || config.output_channels == 0 ||
        config.total_frames == 0 || config.preload_frames == 0 ||
        config.preload_frames >= config.total_frames ||
        config.ring_capacity_frames == 0 || config.read_chunk_frames == 0 ||
        config.total_frames > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    return !block_partition.empty() &&
        std::all_of(block_partition.begin(), block_partition.end(),
                    [](std::uint64_t frames) { return frames > 0; });
}

}  // namespace

audio::Buffer<float> make_deterministic_source(std::uint32_t channels,
                                               std::uint64_t frames) {
    audio::Buffer<float> source(channels, static_cast<std::size_t>(frames));
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
        for (std::uint64_t frame = 0; frame < frames; ++frame) {
            source.channel(channel)[static_cast<std::size_t>(frame)] =
                deterministic_sample(frame, channel);
        }
    }
    return source;
}

RawFloatComparison compare_raw_float_bits(const audio::Buffer<float>& expected,
                                          const audio::Buffer<float>& actual) {
    RawFloatComparison comparison;
    comparison.same_shape = expected.num_channels() == actual.num_channels() &&
                            expected.num_samples() == actual.num_samples();

    const auto channels = std::min(expected.num_channels(), actual.num_channels());
    const auto frames = std::min(expected.num_samples(), actual.num_samples());
    for (std::size_t channel = 0; channel < channels; ++channel) {
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto expected_bits = std::bit_cast<std::uint32_t>(
                expected.channel(channel)[frame]);
            const auto actual_bits = std::bit_cast<std::uint32_t>(
                actual.channel(channel)[frame]);
            comparison.nonzero_expected |= (expected_bits & 0x7FFFFFFFu) != 0;
            if (expected_bits == actual_bits) continue;

            ++comparison.mismatch_count;
            if (!comparison.first_mismatch) {
                comparison.first_mismatch = RawFloatMismatch{
                    channel, frame, expected_bits, actual_bits};
            }
        }
    }
    return comparison;
}

std::optional<SequentialResult> render_sequential_parity(
    const SequentialConfig& config,
    std::span<const std::uint64_t> block_partition,
    bool render_after_reset) {
    if (!valid_config(config, block_partition)) return std::nullopt;

    SequentialResult result;
    result.source = make_deterministic_source(config.source_channels,
                                              config.total_frames);

    audio::StreamingSampleSourceConfig resident_config;
    resident_config.channels = config.source_channels;
    resident_config.total_frames = config.total_frames;
    resident_config.preload_frames = config.total_frames;
    resident_config.start_background_thread = false;

    audio::StreamingSampleSourceConfig streamed_config;
    streamed_config.channels = config.source_channels;
    streamed_config.total_frames = config.total_frames;
    streamed_config.preload_frames = config.preload_frames;
    streamed_config.ring_capacity_frames = config.ring_capacity_frames;
    streamed_config.read_chunk_frames = config.read_chunk_frames;
    streamed_config.start_background_thread = false;

    audio::StreamingSampleSource resident;
    audio::StreamingSampleSource streamed;
    if (!resident.prepare(resident_config, make_buffer_reader(result.source)) ||
        !streamed.prepare(streamed_config, make_buffer_reader(result.source))) {
        return std::nullopt;
    }

    result.resident_first = render_source(resident, config.output_channels,
                                          config.total_frames, block_partition);
    result.streamed_first = render_source(streamed, config.output_channels,
                                          config.total_frames, block_partition);

    if (render_after_reset) {
        if (!resident.reset() || !streamed.reset()) return std::nullopt;
        result.resident_after_reset = render_source(
            resident, config.output_channels, config.total_frames, block_partition);
        result.streamed_after_reset = render_source(
            streamed, config.output_channels, config.total_frames, block_partition);
    }
    return result;
}

}  // namespace pulp::test::sampler_parity
