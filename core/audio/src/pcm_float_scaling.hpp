#pragma once

// Shared float conversion for WAV frames read through CHOC.
//
// Integer PCM decodes to float by dividing by 2^(bits-1): int16 -32768 maps
// to -1.0f and +32767 to 32767/32768, the convention of int16_to_float,
// sample_converter.hpp, the AIFF reader, dr_wav and libsndfile. CHOC's
// readFrames divides by (2^(bits-1) - 1) instead, so the integer depths are
// read at double precision, the original code is recovered exactly (a double
// carries more than twice the bits of any PCM depth, so rounding the product
// lands on the code) and rescaled here. Float files are read as they are.
// Every reader that pulls WAV frames out of CHOC goes through read_pulp_float
// so the registry's whole-file decode and the streaming AudioReader agree bit
// for bit, including 8-bit unsigned and a 24-valid-bits-in-32 container.

#include <choc/audio/choc_AudioFileFormat.h>
#include <choc/audio/choc_SampleBuffers.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace pulp::audio::detail {

struct IntegerDepth {
    double choc_full_scale; // what CHOC divided the code by
    float scale;            // 1 / 2^(bits-1)
};

inline std::optional<IntegerDepth> integer_depth(choc::audio::BitDepth depth) {
    using choc::audio::BitDepth;
    switch (depth) {
    case BitDepth::int8:
        return IntegerDepth{127.0, 1.0f / 128.0f};
    case BitDepth::int16:
        return IntegerDepth{32767.0, 1.0f / 32768.0f};
    case BitDepth::int24:
        return IntegerDepth{8388607.0, 1.0f / 8388608.0f};
    case BitDepth::int32:
        return IntegerDepth{2147483647.0, 1.0f / 2147483648.0f};
    default:
        return std::nullopt;
    }
}

// Frames of double scratch held at a time for an integer depth, so the
// transient stays bounded instead of doubling the caller's output.
inline constexpr std::uint32_t kPcmScalingChunkFrames = 65536u;

// Reads `frames` frames starting at `frame_index` into the planar float
// channels `out` (one pointer per channel, each with room for `frames`),
// in Pulp's scaling. Returns false when the reader could not deliver them.
inline bool read_pulp_float(choc::audio::AudioFileReader& reader, choc::audio::BitDepth depth,
                            std::uint64_t frame_index, float* const* out, std::uint32_t channels,
                            std::uint32_t frames) {
    const auto integer = integer_depth(depth);
    if (!integer) {
        auto view = choc::buffer::createChannelArrayView<float>(
            const_cast<float* const*>(out), channels,
            static_cast<choc::buffer::FrameCount>(frames));
        return reader.readFrames(frame_index, view);
    }

    choc::buffer::ChannelArrayBuffer<double> chunk(channels,
                                                   std::min(frames, kPcmScalingChunkFrames));
    for (std::uint32_t start = 0; start < frames; start += kPcmScalingChunkFrames) {
        const std::uint32_t count = std::min(kPcmScalingChunkFrames, frames - start);
        auto view = chunk.getView().getStart(count);
        if (!reader.readFrames(frame_index + start, view))
            return false;
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            const double* scaled = view.getChannel(ch).data.data;
            float* dest = out[ch] + start;
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto code = std::llround(scaled[i] * integer->choc_full_scale);
                dest[i] = static_cast<float>(code) * integer->scale;
            }
        }
    }
    return true;
}

} // namespace pulp::audio::detail
