#pragma once

#include <cstdint>
#include <span>

#include <pulp/audio/buffer.hpp>

namespace pulp::audio {

struct VoiceSumInput {
    BufferView<const float> source;
    float gain = 1.0f;
    bool active = true;
};

struct VoiceSumOptions {
    bool accumulate = true;
};

struct VoiceSumResult {
    // Max destination frame index touched + 1. When accumulate is false,
    // destination clearing counts as touching the clamped frame range.
    std::uint64_t frames_mixed = 0;
    std::uint32_t inputs_mixed = 0;
    std::uint32_t inputs_skipped = 0;
    std::uint32_t nonfinite_gains = 0;
};

class VoiceSumMixer {
public:
    // Allocation-free static mixer for prepared voice scratch buffers. Source
    // and destination buffers must not overlap. Mono sources duplicate across
    // outputs; multichannel sources feed matching output channels only.
    static VoiceSumResult mix(std::span<const VoiceSumInput> inputs,
                              BufferView<float> destination,
                              std::uint64_t frames,
                              const VoiceSumOptions& options = {}) noexcept;
};

}  // namespace pulp::audio
