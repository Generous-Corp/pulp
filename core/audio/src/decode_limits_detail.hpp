#pragma once

// Shared DecodeLimits enforcement for the readers that decode through a
// third-party codec. Each reader checks the header-claimed size before it
// allocates and the running decoded size as frames arrive.

#include <pulp/audio/format_registry.hpp>
#include <pulp/runtime/log.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace pulp::audio::detail {

// Frames handed to the codec per call. Bounds the interleaved scratch buffer
// so allocation follows the decoded count rather than the header claim.
inline constexpr std::uint64_t kDecodeChunkFrames = 65536u;

// Computes frames * channels * sizeof(float); false when the product
// overflows 64 bits.
inline bool checked_output_bytes(std::uint64_t frames, std::uint32_t channels,
                                 std::uint64_t& bytes) noexcept {
    constexpr auto kFloatBytes = static_cast<std::uint64_t>(sizeof(float));
    if (channels == 0u || frames > std::numeric_limits<std::uint64_t>::max() / channels)
        return false;
    const auto samples = frames * channels;
    if (samples > std::numeric_limits<std::uint64_t>::max() / kFloatBytes)
        return false;
    bytes = samples * kFloatBytes;
    return true;
}

// True when `frames` x `channels` of float output fits inside `limits`.
// Logs the first violated limit by name. A zero field means that ceiling is
// not enforced; the product is still refused when it overflows size_t.
inline bool within_decode_limits(std::string_view format, std::uint64_t frames,
                                 std::uint32_t channels, const DecodeLimits& limits) {
    if (limits.max_channels != 0u && channels > limits.max_channels) {
        runtime::log_warn("{}: {} channels exceeds max_channels {}", format, channels,
                          limits.max_channels);
        return false;
    }
    if (limits.max_frames != 0u && frames > limits.max_frames) {
        runtime::log_warn("{}: {} frames exceeds max_frames {}", format, frames, limits.max_frames);
        return false;
    }
    std::uint64_t bytes = 0u;
    if (channels != 0u && (!checked_output_bytes(frames, channels, bytes) ||
                           bytes > std::numeric_limits<std::size_t>::max())) {
        runtime::log_warn("{}: {} frames x {} channels overflows addressable output", format,
                          frames, channels);
        return false;
    }
    if (limits.max_output_bytes != 0u && bytes > limits.max_output_bytes) {
        runtime::log_warn("{}: {} frames x {} channels exceeds max_output_bytes {}", format, frames,
                          channels, limits.max_output_bytes);
        return false;
    }
    return true;
}

// Reserves planar storage for a header-claimed frame count that has already
// passed within_decode_limits, so chunked appends do not regrow per chunk.
// A zero claim (unknown length) reserves nothing.
inline void reserve_planar(std::vector<std::vector<float>>& channels,
                           std::uint64_t claimed_frames) {
    if (claimed_frames == 0u)
        return;
    for (auto& planar : channels)
        planar.reserve(static_cast<std::size_t>(claimed_frames));
}

// Appends `frames` interleaved samples to the planar channel vectors.
inline void append_deinterleaved(std::vector<std::vector<float>>& channels,
                                 const float* interleaved, std::uint64_t frames,
                                 std::uint32_t num_channels) {
    const auto count = static_cast<std::size_t>(frames);
    for (std::uint32_t ch = 0; ch < num_channels; ++ch) {
        auto& planar = channels[ch];
        const auto offset = planar.size();
        planar.resize(offset + count);
        for (std::size_t i = 0; i < count; ++i)
            planar[offset + i] = interleaved[i * num_channels + ch];
    }
}

} // namespace pulp::audio::detail
