#include <pulp/audio/loop_reader.hpp>

#include <pulp/signal/interpolator.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::audio {

namespace {

std::uint32_t source_channel_for(BufferView<const float> source,
                                 std::uint32_t output_channel) noexcept {
    if (source.num_channels() == 0) return 0;
    if (output_channel < source.num_channels()) return output_channel;
    return source.num_channels() == 1 ? 0 : static_cast<std::uint32_t>(source.num_channels());
}

std::uint64_t wrap_index(const LoopRegion& region, long long frame) noexcept {
    const auto length = static_cast<long long>(region.end_frame - region.start_frame);
    if (length <= 0) return region.start_frame;
    auto relative = frame - static_cast<long long>(region.start_frame);
    relative %= length;
    if (relative < 0) relative += length;
    return region.start_frame + static_cast<std::uint64_t>(relative);
}

float sample_at(BufferView<const float> source,
                const LoopRegion& region,
                std::uint32_t output_channel,
                long long frame) noexcept {
    const auto source_channel = source_channel_for(source, output_channel);
    if (source_channel >= source.num_channels() || source.num_samples() == 0) return 0.0f;

    std::uint64_t index = 0;
    if (region.playback_mode == LoopPlaybackMode::OneShot) {
        const auto source_last = static_cast<long long>(source.num_samples() - 1);
        const auto lo = static_cast<long long>(region.start_frame);
        const auto hi = std::min(static_cast<long long>(region.end_frame - 1), source_last);
        index = static_cast<std::uint64_t>(std::clamp(frame, lo, hi));
    } else {
        index = wrap_index(region, frame);
    }
    if (index >= source.num_samples()) return 0.0f;
    return source.channel_ptr(source_channel)[index];
}

}  // namespace

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
    if (source.num_channels() == 0 || source.num_samples() == 0 ||
        !validate_loop_region(region, static_cast<std::uint64_t>(source.num_samples())).ok) {
        return 0.0f;
    }
    return read_validated(source, region, output_channel, position);
}

float LoopReader::read_validated(BufferView<const float> source,
                                 const LoopRegion& region,
                                 std::uint32_t output_channel,
                                 double position) noexcept {
    if (region.playback_mode == LoopPlaybackMode::OneShot &&
        (position < static_cast<double>(region.start_frame) ||
         position >= static_cast<double>(region.end_frame))) {
        return 0.0f;
    }

    const auto normalized = normalize_position(region, position);
    const auto base = static_cast<long long>(std::floor(normalized));
    const auto frac = static_cast<float>(normalized - static_cast<double>(base));

    switch (region.interpolation) {
        case LoopInterpolationMode::None:
            return sample_at(source, region, output_channel, base);
        case LoopInterpolationMode::Linear: {
            const auto y0 = sample_at(source, region, output_channel, base);
            const auto y1 = sample_at(source, region, output_channel, base + 1);
            return pulp::signal::Interpolator::linear(frac, y0, y1);
        }
        case LoopInterpolationMode::Cubic: {
            const auto ym1 = sample_at(source, region, output_channel, base - 1);
            const auto y0 = sample_at(source, region, output_channel, base);
            const auto y1 = sample_at(source, region, output_channel, base + 1);
            const auto y2 = sample_at(source, region, output_channel, base + 2);
            return pulp::signal::Interpolator::hermite(frac, ym1, y0, y1, y2);
        }
    }
    return 0.0f;
}

}  // namespace pulp::audio
