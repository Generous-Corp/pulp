#pragma once

#include <cstdint>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/loop_types.hpp>

namespace pulp::audio {

class LoopReader {
public:
    /// Resolves an interpolation tap according to the traversal boundary:
    /// bounded modes clamp, ping-pong reflects, and steady loops wrap.
    static std::uint64_t source_frame_for_tap(const LoopRegion& region,
                                              std::uint64_t source_frames,
                                              std::int64_t signed_frame) noexcept;

    static double normalize_position(const LoopRegion& region,
                                     double position) noexcept;

    static float read(BufferView<const float> source,
                      const LoopRegion& region,
                      std::uint32_t output_channel,
                      double position) noexcept;

    // Fast path for renderers that already validated `region` against the
    // source length for the current block. Public `read()` remains the safe
    // guard rail for arbitrary callers.
    static float read_validated(BufferView<const float> source,
                                const LoopRegion& region,
                                std::uint32_t output_channel,
                                double position) noexcept;
};

}  // namespace pulp::audio
