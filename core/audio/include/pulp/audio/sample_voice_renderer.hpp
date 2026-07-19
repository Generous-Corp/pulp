#pragma once

#include <cstdint>
#include <span>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/instrument_envelope.hpp>
#include <pulp/audio/loop_types.hpp>
#include <pulp/audio/sample_pool.hpp>

namespace pulp::audio {

struct SampleVoiceRenderState {
    bool active = false;
    SamplePoolResolution sample{};
    double position_frames = 0.0;
    double host_sample_rate = 0.0;
    double playback_rate = 1.0;
    float gain = 1.0f;
    std::uint32_t fade_out_frames = 0;
    std::uint32_t fade_out_position = 0;
    bool use_playback_region = false;
    LoopRegion playback_region{};
};

struct SampleVoiceRenderOptions {
    bool accumulate = true;
    AhdsrEnvelope* envelope = nullptr;
    LoopInterpolationMode interpolation = LoopInterpolationMode::Linear;
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
    // This adapter owns pool resolution, optional envelope integration, steal
    // fades, and accumulate/overwrite behavior. Use LoopRenderer for new
    // cursor-owned rich-loop playback.
    // RT-safe when state.sample's borrowed store remains valid, destination and
    // channel_scratch are caller-owned, and any envelope has been prepared.
    // If provided, the envelope must be per-voice state, not shared across
    // concurrently rendered voices.
    // Without an explicit playback region this scalar path renders a full-sample
    // one-shot using options.interpolation. With a playback region it honors
    // start/end, playback_mode, source_sample_rate, and interpolation. Crossfade
    // and snap metadata are validated as part of LoopRegion, but crossfaded
    // rendering remains a separate loop-renderer concern.
    // Channel mapping follows LoopReader: mono sources duplicate across outputs,
    // while outputs beyond a multichannel source's channel count remain silent.
    //
    // position_frames is always a source-frame coordinate. Callers that use a
    // nonzero region start should initialize position_frames to region.start_frame
    // for forward/one-shot playback or end_frame - 1 for reverse playback.
    // playback_rate must be positive; reverse regions use a negative internal
    // step while keeping the public voice state policy simple.
    static SampleVoiceRenderResult render(
        SampleVoiceRenderState& state,
        BufferView<float> destination,
        std::uint64_t frames,
        std::span<const float*> channel_scratch,
        const SampleVoiceRenderOptions& options = {}) noexcept;

    static void begin_fade_out(SampleVoiceRenderState& state,
                               std::uint32_t fade_frames) noexcept;
};

}  // namespace pulp::audio
