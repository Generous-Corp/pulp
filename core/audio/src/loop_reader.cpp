#include <pulp/audio/loop_reader.hpp>
#include <pulp/audio/sample_interpolation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace pulp::audio {

namespace {

std::uint32_t source_channel_for(BufferView<const float> source,
                                 std::uint32_t output_channel) noexcept {
    if (source.num_channels() == 0) return 0;
    if (output_channel < source.num_channels()) return output_channel;
    return source.num_channels() == 1 ? 0 : static_cast<std::uint32_t>(source.num_channels());
}

float sample_at(BufferView<const float> source,
                const LoopRegion& region,
                std::uint32_t output_channel,
                std::int64_t frame) noexcept {
    const auto source_channel = source_channel_for(source, output_channel);
    if (source_channel >= source.num_channels() || source.num_samples() == 0) return 0.0f;

    const auto index = LoopReader::source_frame_for_tap(
        region, static_cast<std::uint64_t>(source.num_samples()), frame);
    if (index >= source.num_samples()) return 0.0f;
    return source.channel_ptr(source_channel)[index];
}

}  // namespace

std::uint64_t LoopReader::source_frame_for_tap(
    const LoopRegion& region,
    std::uint64_t source_frames,
    std::int64_t signed_frame) noexcept {
    if (source_frames == 0 || region.start_frame >= source_frames ||
        region.end_frame <= region.start_frame) {
        return 0;
    }

    if (region.playback_mode == LoopPlaybackMode::OneShot ||
        region.playback_mode == LoopPlaybackMode::ReverseOnce) {
        const auto last = std::min(region.end_frame - 1, source_frames - 1);
        if (signed_frame < 0) return region.start_frame;
        return std::clamp(static_cast<std::uint64_t>(signed_frame),
                          region.start_frame,
                          last);
    }

    const auto length = region.end_frame - region.start_frame;
    if (region.playback_mode == LoopPlaybackMode::PingPong) {
        const auto span = length - 1;
        if (span == 0) return region.start_frame;

        std::uint64_t quotient_parity = 0;
        std::uint64_t remainder = 0;
        const auto fold_distance = [&](std::uint64_t distance) {
            quotient_parity = (distance / span) & 1u;
            remainder = distance % span;
        };
        if (signed_frame >= 0) {
            const auto frame = static_cast<std::uint64_t>(signed_frame);
            fold_distance(frame >= region.start_frame
                              ? frame - region.start_frame
                              : region.start_frame - frame);
        } else {
            const auto magnitude =
                static_cast<std::uint64_t>(-(signed_frame + 1)) + 1;
            quotient_parity = ((region.start_frame / span) ^
                               (magnitude / span)) & 1u;
            const auto start_remainder = region.start_frame % span;
            const auto magnitude_remainder = magnitude % span;
            if (start_remainder >= span - magnitude_remainder) {
                remainder = start_remainder - (span - magnitude_remainder);
                quotient_parity ^= 1u;
            } else {
                remainder = start_remainder + magnitude_remainder;
            }
        }
        return region.start_frame +
               (quotient_parity == 0 ? remainder : span - remainder);
    }

    std::uint64_t offset = 0;
    if (signed_frame >= 0) {
        const auto frame = static_cast<std::uint64_t>(signed_frame);
        if (frame >= region.start_frame) {
            offset = (frame - region.start_frame) % length;
        } else {
            const auto distance = (region.start_frame - frame) % length;
            offset = distance == 0 ? 0 : length - distance;
        }
    } else {
        // Splitting the negation keeps the full INT64_MIN tap representable.
        const auto magnitude =
            static_cast<std::uint64_t>(-(signed_frame + 1)) + 1;
        const auto start_remainder = region.start_frame % length;
        const auto magnitude_remainder = magnitude % length;
        const auto distance = start_remainder >= length - magnitude_remainder
            ? start_remainder - (length - magnitude_remainder)
            : start_remainder + magnitude_remainder;
        offset = distance == 0 ? 0 : length - distance;
    }
    return region.start_frame + offset;
}

double LoopReader::normalize_position(const LoopRegion& region,
                                      double position) noexcept {
    if (region.end_frame <= region.start_frame) return static_cast<double>(region.start_frame);
    // OneShot/ReverseOnce/PingPong positions are bounded by the renderer (PingPong
    // reflects at the loop edges; the others play once), so no modulo wrap here.
    if (region.playback_mode == LoopPlaybackMode::OneShot ||
        region.playback_mode == LoopPlaybackMode::ReverseOnce ||
        region.playback_mode == LoopPlaybackMode::PingPong)
        return position;

    const auto start = static_cast<double>(region.start_frame);
    const auto length = static_cast<double>(region.end_frame - region.start_frame);
    auto relative = std::fmod(position - start, length);
    if (relative < 0.0) relative += length;
    return start + relative;
}

float LoopReader::read(BufferView<const float> source,
                       const LoopRegion& region,
                       std::uint32_t output_channel,
                       double position) noexcept {
    return read(source, region, output_channel, position,
                sample_interpolation_policy(region.interpolation));
}

float LoopReader::read(BufferView<const float> source,
                       const LoopRegion& region,
                       std::uint32_t output_channel,
                       double position,
                       SampleInterpolationPolicy interpolation) noexcept {
    return read(source, region, output_channel, position,
                PreparedSampleInterpolation{.policy = interpolation});
}

float LoopReader::read(
    BufferView<const float> source,
    const LoopRegion& region,
    std::uint32_t output_channel,
    double position,
    const PreparedSampleInterpolation& interpolation) noexcept {
    if (source.num_channels() == 0 || source.num_samples() == 0 ||
        !validate_loop_region(region, static_cast<std::uint64_t>(source.num_samples())).ok ||
        !interpolation.valid() || !std::isfinite(position)) {
        return 0.0f;
    }
    return read_validated(source, region, output_channel, position, interpolation);
}

float LoopReader::read_validated(BufferView<const float> source,
                                 const LoopRegion& region,
                                 std::uint32_t output_channel,
                                 double position) noexcept {
    return read_validated(source, region, output_channel, position,
                          sample_interpolation_policy(region.interpolation));
}

float LoopReader::read_validated(BufferView<const float> source,
                                 const LoopRegion& region,
                                 std::uint32_t output_channel,
                                 double position,
                                 SampleInterpolationPolicy interpolation) noexcept {
    return read_validated(source, region, output_channel, position,
                          PreparedSampleInterpolation{.policy = interpolation});
}

float LoopReader::read_validated(
    BufferView<const float> source,
    const LoopRegion& region,
    std::uint32_t output_channel,
    double position,
    const PreparedSampleInterpolation& interpolation) noexcept {
    if ((region.playback_mode == LoopPlaybackMode::OneShot ||
         region.playback_mode == LoopPlaybackMode::ReverseOnce) &&
        (position < static_cast<double>(region.start_frame) ||
         position >= static_cast<double>(region.end_frame))) {
        return 0.0f;
    }

    const auto normalized = normalize_position(region, position);
    const auto resolved = resolve_sample_interpolation_position(
        normalized, interpolation);
    if (!resolved.valid) return 0.0f;
    std::array<float, kMaximumSampleInterpolationTaps> taps;
    for (std::uint32_t tap = 0; tap < resolved.footprint.tap_count; ++tap) {
        taps[tap] = sample_at(source, region, output_channel,
                              resolved.base_frame +
                                  resolved.footprint.first_offset + tap);
    }
    return interpolation.evaluate(
        resolved.fraction,
        std::span<const float>(taps.data(), resolved.footprint.tap_count));
}

}  // namespace pulp::audio
