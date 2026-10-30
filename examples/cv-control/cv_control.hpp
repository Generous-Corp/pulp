#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
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

/// Application-owned mapping state shared by every input kind. It publishes
/// one coherent scalar derived from one provider result; the audio thread only
/// performs atomic loads. No camera, sensor, OSC, or inference dependency is
/// required.
class SignalMapping {
  public:
    explicit SignalMapping(float confidence_gate = 0.6f, float smoothing = 0.25f) noexcept
        : confidence_gate_(confidence_gate), smoothing_(smoothing) {}

    void submit(ControlSample sample) noexcept {
        const bool valid = sample.valid && std::isfinite(sample.value) &&
                           std::isfinite(sample.confidence) &&
                           sample.confidence >= confidence_gate_;
        const float target = valid ? std::clamp(sample.value, 0.0f, 1.0f) : 0.0f;
        const float previous = value_.load(std::memory_order_relaxed);
        const float smoothed = previous + smoothing_ * (target - previous);
        value_.store(std::clamp(smoothed, 0.0f, 1.0f), std::memory_order_release);
        tracked_.store(valid, std::memory_order_release);
        sequence_.store(sample.sequence, std::memory_order_release);
    }

    /// Explicit loss/stale policy. Call from a provider/watchdog thread, never
    /// from Processor::process(). This makes replay and failure tests exact.
    void expire() noexcept {
        value_.store(0.0f, std::memory_order_release);
        tracked_.store(false, std::memory_order_release);
    }

    float value() const noexcept {
        return value_.load(std::memory_order_acquire);
    }
    bool valid() const noexcept {
        return tracked_.load(std::memory_order_acquire);
    }
    std::uint64_t sequence() const noexcept {
        return sequence_.load(std::memory_order_acquire);
    }

  private:
    float confidence_gate_;
    float smoothing_;
    std::atomic<float> value_{0.0f};
    std::atomic<bool> tracked_{false};
    std::atomic<std::uint64_t> sequence_{0};
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
