#pragma once

#include <cstdint>
#include <span>

namespace pulp::audio {

enum class EnvelopeStage : std::uint8_t {
    Idle,
    Attack,
    Hold,
    Decay,
    Sustain,
    Release,
};

struct AhdsrEnvelopeConfig {
    double sample_rate = 48000.0;
    double attack_seconds = 0.005;
    double hold_seconds = 0.0;
    double decay_seconds = 0.050;
    double sustain_level = 1.0;
    double release_seconds = 0.050;
};

class AhdsrEnvelope {
public:
    AhdsrEnvelope() = default;

    // Call prepare() off the audio thread before realtime use. Invalid
    // prepare() calls leave the previous configuration/state untouched.
    bool prepare(const AhdsrEnvelopeConfig& config) noexcept;
    void reset() noexcept;

    // RT-safe after prepare(). note_on() restarts from zero for non-zero
    // attacks; the first emitted attack sample is advanced by one frame.
    void note_on() noexcept;
    void note_off() noexcept;

    float next_sample() noexcept;
    // Writes envelope gain values into destination; does not multiply audio.
    void render(std::span<float> destination) noexcept;

    EnvelopeStage stage() const noexcept { return stage_; }
    float value() const noexcept { return value_; }
    bool active() const noexcept { return stage_ != EnvelopeStage::Idle; }

private:
    static bool config_valid(const AhdsrEnvelopeConfig& config) noexcept;
    static bool duration_frames_valid(double seconds,
                                      double sample_rate) noexcept;
    static std::uint64_t seconds_to_frames(double seconds,
                                           double sample_rate) noexcept;

    void enter_stage(EnvelopeStage stage) noexcept;
    void advance_from_peak_stage(bool preserve_current_value) noexcept;

    AhdsrEnvelopeConfig config_{};
    EnvelopeStage stage_ = EnvelopeStage::Idle;
    std::uint64_t stage_frame_ = 0;
    std::uint64_t attack_frames_ = 0;
    std::uint64_t hold_frames_ = 0;
    std::uint64_t decay_frames_ = 0;
    std::uint64_t release_frames_ = 0;
    float value_ = 0.0f;
    float release_start_ = 0.0f;
};

}  // namespace pulp::audio
