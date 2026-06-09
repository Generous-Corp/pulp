#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace pulp::audio {

enum class VoiceModulationTarget : std::uint8_t {
    Gain,
    PitchCents,
    Pan,
    Pressure,
    Timbre,
    Aux0,
    Aux1,
    Aux2,
    Aux3,
    Aux4,
    Aux5,
    Aux6,
    Aux7,
};

enum class VoiceModulationRate : std::uint8_t {
    Constant,
    AudioRate,
};

enum class VoiceModulationStatus : std::uint8_t {
    Ok,
    NotPrepared,
    InvalidFrameCount,
    InvalidTarget,
    NonFiniteValue,
    DuplicateTarget,
    LaneOverflow,
};

struct VoiceModulationResult {
    bool ok = false;
    VoiceModulationStatus status = VoiceModulationStatus::NotPrepared;
};

struct VoiceModulationAudioRateReservation {
    bool ok = false;
    VoiceModulationStatus status = VoiceModulationStatus::NotPrepared;
    float* values = nullptr;
    std::uint32_t frame_count = 0;
};

struct VoiceModulationBufferConfig {
    std::uint32_t max_lanes = 0;
    std::uint32_t max_frames = 0;
};

struct VoiceModulationLane {
    VoiceModulationTarget target = VoiceModulationTarget::Gain;
    VoiceModulationRate rate = VoiceModulationRate::Constant;
    float constant_value = 0.0f;
    const float* values = nullptr;
    std::uint32_t frame_count = 0;

    float value_at(std::uint32_t frame) const noexcept;
};

struct VoiceModulationBlock {
    std::span<const VoiceModulationLane> lanes{};
    std::uint32_t frame_count = 0;

    bool empty() const noexcept { return lanes.empty(); }
    const VoiceModulationLane* find(VoiceModulationTarget target) const noexcept;
    float value_at(VoiceModulationTarget target,
                   std::uint32_t frame,
                   float fallback = 0.0f) const noexcept;
};

class VoiceModulationBuffer {
public:
    VoiceModulationBuffer() = default;

    // Control/background-thread only. Allocates fixed lane and audio-rate
    // storage. The buffer stores resolved values; it does not apply routing,
    // scaling, smoothing, unit conversion, or range clamping.
    bool prepare(const VoiceModulationBufferConfig& config);
    // Control/background-thread only. Invalidates prepared storage.
    void release() noexcept;
    // RT-safe after prepare. Clears current block lanes and counters while
    // keeping prepared storage.
    void reset() noexcept;

    bool prepared() const noexcept { return prepared_; }
    std::uint32_t max_lanes() const noexcept { return max_lanes_; }
    std::uint32_t max_frames() const noexcept { return max_frames_; }
    std::uint32_t frame_count() const noexcept { return frame_count_; }
    // Lane-overflow count accumulated since prepare(), release(), or reset().
    std::uint32_t overflow_count() const noexcept { return overflow_count_; }

    // RT-safe after prepare. Failure always exposes an empty block.
    VoiceModulationResult begin_block(std::uint32_t frame_count) noexcept;

    // RT-safe after begin_block(). One lane per target is allowed per block.
    VoiceModulationResult add_constant(VoiceModulationTarget target,
                                       float value) noexcept;
    VoiceModulationResult add_audio_rate(VoiceModulationTarget target,
                                         std::span<const float> values) noexcept;
    // RT-safe after begin_block(). On success the returned writable buffer is
    // zero-filled and valid until the next begin_block(), prepare(), release(),
    // reset(), or destruction of this object. Callers must write finite values;
    // direct reservations intentionally avoid post-write validation.
    VoiceModulationAudioRateReservation reserve_audio_rate(
        VoiceModulationTarget target) noexcept;

    VoiceModulationBlock block() const noexcept;

private:
    static bool target_valid(VoiceModulationTarget target) noexcept;
    VoiceModulationResult append_lane(VoiceModulationTarget target,
                                      VoiceModulationRate rate,
                                      float constant_value,
                                      const float* values) noexcept;
    bool target_exists(VoiceModulationTarget target) const noexcept;
    float* values_for_lane(std::uint32_t lane_index) noexcept;

    std::vector<VoiceModulationLane> lanes_;
    std::vector<float> values_;
    std::uint32_t max_lanes_ = 0;
    std::uint32_t max_frames_ = 0;
    std::uint32_t frame_count_ = 0;
    std::uint32_t lane_count_ = 0;
    std::uint32_t overflow_count_ = 0;
    bool prepared_ = false;
};

}  // namespace pulp::audio
