#pragma once

// Shared DecodeLimits enforcement for the readers that decode through a
// third-party codec. Each reader checks the header-claimed size before it
// allocates and the running decoded size as frames arrive.

#include <pulp/audio/format_registry.hpp>
#include <pulp/runtime/log.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
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

// Frames reserved up front from a header claim. A claim is still untrusted
// after the ceiling check, so a small file must not force a ceiling-sized
// reservation; growth past this continues by resize per chunk.
inline constexpr std::uint64_t kMaxReservedFrames = 16u * kDecodeChunkFrames;

// Reserves planar storage for a header-claimed frame count that has already
// passed within_decode_limits, capped at kMaxReservedFrames. A zero claim
// (unknown length) reserves nothing.
inline void reserve_planar(std::vector<std::vector<float>>& channels,
                           std::uint64_t claimed_frames) {
    if (claimed_frames == 0u)
        return;
    const auto frames = static_cast<std::size_t>(std::min(claimed_frames, kMaxReservedFrames));
    for (auto& planar : channels)
        planar.reserve(frames);
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

// Decodes a whole stream into planar float under the length contract every
// codec-backed reader shares. `claimed_frames` is the container's declared
// frame count, zero when it declares none. The claim is checked against
// `limits` before any output is allocated and the running total is checked
// as chunks arrive; at the end a declared count the stream did not deliver
// is a refusal (a truncated or damaged file), while an undeclared length is
// accepted as decoded. `read_chunk(float* interleaved, std::uint64_t
// max_frames)` writes up to max_frames interleaved frames and returns how many,
// zero at the end of the stream.
template <typename ReadChunk>
inline std::optional<AudioFileData>
decode_chunked(std::string_view format, std::uint32_t sample_rate, std::uint32_t channels,
               std::uint64_t claimed_frames, const DecodeLimits& limits, ReadChunk&& read_chunk) {
    if (channels == 0u || !within_decode_limits(format, claimed_frames, channels, limits))
        return std::nullopt;

    AudioFileData data;
    data.sample_rate = sample_rate;
    data.channels.resize(channels);
    reserve_planar(data.channels, claimed_frames);

    std::vector<float> interleaved(static_cast<std::size_t>(kDecodeChunkFrames) * channels);
    std::uint64_t frames_read = 0;
    for (;;) {
        const std::uint64_t got = read_chunk(interleaved.data(), kDecodeChunkFrames);
        if (got == 0u)
            break;
        if (!within_decode_limits(format, frames_read + got, channels, limits))
            return std::nullopt;
        append_deinterleaved(data.channels, interleaved.data(), got, channels);
        frames_read += got;
    }
    if (frames_read == 0u || (claimed_frames != 0u && frames_read != claimed_frames))
        return std::nullopt;
    return data;
}

// Frames a stream delivers, found by decoding and discarding, for a reader
// whose container declares no length and whose header scan can count a frame
// the decoder then rejects. Same read_chunk contract as decode_chunked.
template <typename ReadChunk>
inline std::uint64_t count_decoded_frames(std::uint32_t channels, ReadChunk&& read_chunk) {
    std::vector<float> scratch(static_cast<std::size_t>(kDecodeChunkFrames) *
                               std::max<std::uint32_t>(channels, 1u));
    std::uint64_t frames = 0;
    for (;;) {
        const std::uint64_t got = read_chunk(scratch.data(), kDecodeChunkFrames);
        if (got == 0u)
            break;
        frames += got;
    }
    return frames;
}

} // namespace pulp::audio::detail
