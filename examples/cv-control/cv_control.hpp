#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

namespace pulp::examples::cv_control {

/// Provider-owned coherent result. CV, IMU, MIDI/OSC bridges, and sensor
/// adapters can each define their own frame and reduce it to this sample.
struct ControlSample {
    std::uint64_t sequence = 0;
    float value = 0.0f;
    float confidence = 0.0f;
    bool valid = false;
};

struct FaceFrame {
    std::uint64_t sequence = 0;
    float mouth_open = 0.0f;
    float confidence = 0.0f;
    bool tracked = false;
};

/// This example reserves channel-16 pitch bend as one control packet.
/// The upper data bit is validity; the other 13 bits carry a normalized value.
/// MediaPipe's current page reports presence (0/1), not graded confidence.
inline std::optional<ControlSample> decode_midi_control(std::uint8_t status, std::uint8_t low,
                                                        std::uint8_t high,
                                                        std::uint64_t arrival) noexcept {
    if (status != 0xEF || low > 0x7F || high > 0x7F)
        return std::nullopt;
    const auto quantized = static_cast<unsigned>(low) | (static_cast<unsigned>(high & 0x3F) << 7);
    const bool valid = (high & 0x40) != 0;
    return ControlSample{arrival, static_cast<float>(quantized) / 8191.0f, valid ? 1.0f : 0.0f,
                         valid};
}

/// Application-owned mapping state shared by every input kind. The processor's
/// audio thread owns it; provider threads publish complete samples through a
/// separate handoff. No camera, sensor, OSC, or inference dependency is required.
class SignalMapping {
  public:
    explicit SignalMapping(float confidence_gate = 0.6f, float smoothing = 0.25f) noexcept
        : confidence_gate_(confidence_gate), smoothing_(smoothing) {}

    void submit(ControlSample sample) noexcept {
        const bool valid = sample.valid && std::isfinite(sample.value) &&
                           std::isfinite(sample.confidence) &&
                           sample.confidence >= confidence_gate_;
        const float target = valid ? std::clamp(sample.value, 0.0f, 1.0f) : 0.0f;
        value_ = std::clamp(value_ + smoothing_ * (target - value_), 0.0f, 1.0f);
        tracked_ = valid;
        sequence_ = sample.sequence;
        age_samples_ = 0;
    }

    /// Audio sample counts make provider stalls deterministic in every host.
    void advance(std::uint64_t samples, std::uint64_t timeout) noexcept {
        if (samples >= timeout || age_samples_ >= timeout - samples) {
            expire();
        } else {
            age_samples_ += samples;
        }
    }

    void expire() noexcept {
        value_ = 0.0f;
        tracked_ = false;
        age_samples_ = 0;
    }

    float value() const noexcept {
        return value_;
    }
    bool valid() const noexcept {
        return tracked_;
    }
    std::uint64_t sequence() const noexcept {
        return sequence_;
    }

  private:
    float confidence_gate_;
    float smoothing_;
    float value_ = 0.0f;
    bool tracked_ = false;
    std::uint64_t sequence_ = 0;
    std::uint64_t age_samples_ = 0;
};

/// Deterministic mock/replay source used by tests and camera-free development.
/// The provider owns the frame storage; replay only walks one coherent sample
/// at a time and never touches the audio thread.
class ControlReplay {
  public:
    explicit ControlReplay(std::span<const ControlSample> samples) noexcept : samples_(samples) {}

    bool next(SignalMapping& mapping) noexcept {
        if (cursor_ >= samples_.size())
            return false;
        mapping.submit(samples_[cursor_++]);
        return true;
    }

    void reset() noexcept {
        cursor_ = 0;
    }
    std::size_t position() const noexcept {
        return cursor_;
    }

  private:
    std::span<const ControlSample> samples_;
    std::size_t cursor_ = 0;
};

// Compatibility name for the first face example; the implementation is now
// explicitly reusable by motion, proximity, MIDI, and custom ML inputs.
using FaceMapping = SignalMapping;

} // namespace pulp::examples::cv_control
