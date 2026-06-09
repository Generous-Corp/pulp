#pragma once

#include <cstdint>
#include <span>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/instrument_envelope.hpp>
#include <pulp/audio/sample_pool.hpp>

namespace pulp::audio {

struct SampleVoiceRenderState {
    bool active = false;
    SamplePoolResolution sample{};
    double position_frames = 0.0;
    double playback_rate = 1.0;
    float gain = 1.0f;
};

struct SampleVoiceRenderOptions {
    bool accumulate = true;
    AhdsrEnvelope* envelope = nullptr;
};

struct SampleVoiceRenderResult {
    // Frames where sample playback advanced and wrote/accumulated a value.
    std::uint64_t rendered_frames = 0;
    // Frames not rendered because the voice was inactive, invalid, or ended
    // before the requested block completed.
    std::uint64_t silent_frames = 0;
    bool finished = false;
};

class SampleVoiceRenderer {
public:
    // RT-safe when state.sample's borrowed store remains valid, destination and
    // channel_scratch are caller-owned, and any envelope has been prepared.
    // If provided, the envelope must be per-voice state, not shared across
    // concurrently rendered voices.
    // This scalar path handles one-shot forward playback only; looping,
    // streaming, interpolation policy, and SIMD voice summing remain separate
    // sampler runtime slices.
    static SampleVoiceRenderResult render(
        SampleVoiceRenderState& state,
        BufferView<float> destination,
        std::uint64_t frames,
        std::span<const float*> channel_scratch,
        const SampleVoiceRenderOptions& options = {}) noexcept;
};

}  // namespace pulp::audio
