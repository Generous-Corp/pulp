#include <pulp/audio/voice_modulation_buffer.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace pulp::audio {

namespace {

VoiceModulationResult ok_result() noexcept {
    return {true, VoiceModulationStatus::Ok};
}

VoiceModulationResult fail_result(VoiceModulationStatus status) noexcept {
    return {false, status};
}

bool finite(float value) noexcept {
    return std::isfinite(value);
}

bool checked_total_values(std::uint32_t max_lanes,
                          std::uint32_t max_frames,
                          std::size_t& out) noexcept {
    if (max_lanes == 0 || max_frames == 0) return false;
    const auto lanes = static_cast<std::size_t>(max_lanes);
    const auto frames = static_cast<std::size_t>(max_frames);
    if (frames > std::numeric_limits<std::size_t>::max() / lanes) return false;
    out = lanes * frames;
    return true;
}

}  // namespace

float VoiceModulationLane::value_at(std::uint32_t frame) const noexcept {
    if (rate == VoiceModulationRate::Constant ||
        values == nullptr ||
        frame_count == 0) {
        return constant_value;
    }
    const auto clamped = std::min(frame, frame_count - 1);
    return values[clamped];
}

const VoiceModulationLane* VoiceModulationBlock::find(
    VoiceModulationTarget target) const noexcept {
    for (const auto& lane : lanes) {
        if (lane.target == target) return &lane;
    }
    return nullptr;
}

float VoiceModulationBlock::value_at(VoiceModulationTarget target,
                                     std::uint32_t frame,
                                     float fallback) const noexcept {
    const auto* lane = find(target);
    return lane == nullptr ? fallback : lane->value_at(frame);
}

bool VoiceModulationBuffer::prepare(const VoiceModulationBufferConfig& config) {
    std::size_t total_values = 0;
    if (!checked_total_values(config.max_lanes,
                              config.max_frames,
                              total_values)) {
        release();
        return false;
    }

    std::vector<VoiceModulationLane> lanes(config.max_lanes);
    std::vector<float> values(total_values, 0.0f);

    lanes_ = std::move(lanes);
    values_ = std::move(values);
    max_lanes_ = config.max_lanes;
    max_frames_ = config.max_frames;
    frame_count_ = 0;
    lane_count_ = 0;
    overflow_count_ = 0;
    prepared_ = true;
    return true;
}

void VoiceModulationBuffer::release() noexcept {
    lanes_.clear();
    values_.clear();
    max_lanes_ = 0;
    max_frames_ = 0;
    frame_count_ = 0;
    lane_count_ = 0;
    overflow_count_ = 0;
    prepared_ = false;
}

void VoiceModulationBuffer::reset() noexcept {
    frame_count_ = 0;
    lane_count_ = 0;
    overflow_count_ = 0;
}

VoiceModulationResult VoiceModulationBuffer::begin_block(
    std::uint32_t frame_count) noexcept {
    lane_count_ = 0;
    frame_count_ = 0;

    if (!prepared_) return fail_result(VoiceModulationStatus::NotPrepared);
    if (frame_count == 0 || frame_count > max_frames_) {
        return fail_result(VoiceModulationStatus::InvalidFrameCount);
    }

    frame_count_ = frame_count;
    return ok_result();
}

VoiceModulationResult VoiceModulationBuffer::add_constant(
    VoiceModulationTarget target,
    float value) noexcept {
    if (!finite(value)) {
        return fail_result(VoiceModulationStatus::NonFiniteValue);
    }
    return append_lane(target,
                       VoiceModulationRate::Constant,
                       value,
                       nullptr);
}

VoiceModulationResult VoiceModulationBuffer::add_audio_rate(
    VoiceModulationTarget target,
    std::span<const float> values) noexcept {
    if (!prepared_) return fail_result(VoiceModulationStatus::NotPrepared);
    if (frame_count_ == 0 || values.size() != frame_count_) {
        return fail_result(VoiceModulationStatus::InvalidFrameCount);
    }
    if (!target_valid(target)) {
        return fail_result(VoiceModulationStatus::InvalidTarget);
    }
    if (target_exists(target)) {
        return fail_result(VoiceModulationStatus::DuplicateTarget);
    }
    if (lane_count_ >= max_lanes_) {
        ++overflow_count_;
        return fail_result(VoiceModulationStatus::LaneOverflow);
    }

    for (const auto value : values) {
        if (!finite(value)) {
            return fail_result(VoiceModulationStatus::NonFiniteValue);
        }
    }

    auto* destination = values_for_lane(lane_count_);
    std::copy(values.begin(), values.end(), destination);
    return append_lane(target,
                       VoiceModulationRate::AudioRate,
                       0.0f,
                       destination);
}

VoiceModulationAudioRateReservation
VoiceModulationBuffer::reserve_audio_rate(VoiceModulationTarget target) noexcept {
    VoiceModulationAudioRateReservation reservation;

    if (!prepared_) {
        reservation.status = VoiceModulationStatus::NotPrepared;
        return reservation;
    }
    if (frame_count_ == 0) {
        reservation.status = VoiceModulationStatus::InvalidFrameCount;
        return reservation;
    }
    if (!target_valid(target)) {
        reservation.status = VoiceModulationStatus::InvalidTarget;
        return reservation;
    }
    if (target_exists(target)) {
        reservation.status = VoiceModulationStatus::DuplicateTarget;
        return reservation;
    }
    if (lane_count_ >= max_lanes_) {
        ++overflow_count_;
        reservation.status = VoiceModulationStatus::LaneOverflow;
        return reservation;
    }

    auto* values = values_for_lane(lane_count_);
    std::fill_n(values, frame_count_, 0.0f);
    const auto result = append_lane(target,
                                    VoiceModulationRate::AudioRate,
                                    0.0f,
                                    values);
    reservation.ok = result.ok;
    reservation.status = result.status;
    reservation.values = result.ok ? values : nullptr;
    reservation.frame_count = result.ok ? frame_count_ : 0;
    return reservation;
}

VoiceModulationBlock VoiceModulationBuffer::block() const noexcept {
    if (!prepared_ || frame_count_ == 0) {
        return {};
    }
    return {
        .lanes = std::span<const VoiceModulationLane>(lanes_.data(), lane_count_),
        .frame_count = frame_count_,
    };
}

bool VoiceModulationBuffer::target_valid(VoiceModulationTarget target) noexcept {
    const auto value = static_cast<std::uint8_t>(target);
    return value <= static_cast<std::uint8_t>(VoiceModulationTarget::Aux7);
}

VoiceModulationResult VoiceModulationBuffer::append_lane(
    VoiceModulationTarget target,
    VoiceModulationRate rate,
    float constant_value,
    const float* values) noexcept {
    if (!prepared_) return fail_result(VoiceModulationStatus::NotPrepared);
    if (frame_count_ == 0) {
        return fail_result(VoiceModulationStatus::InvalidFrameCount);
    }
    if (!target_valid(target)) {
        return fail_result(VoiceModulationStatus::InvalidTarget);
    }
    if (target_exists(target)) {
        return fail_result(VoiceModulationStatus::DuplicateTarget);
    }
    if (lane_count_ >= max_lanes_) {
        ++overflow_count_;
        return fail_result(VoiceModulationStatus::LaneOverflow);
    }

    lanes_[lane_count_] = VoiceModulationLane{
        .target = target,
        .rate = rate,
        .constant_value = constant_value,
        .values = values,
        .frame_count = frame_count_,
    };
    ++lane_count_;
    return ok_result();
}

bool VoiceModulationBuffer::target_exists(
    VoiceModulationTarget target) const noexcept {
    for (std::uint32_t i = 0; i < lane_count_; ++i) {
        if (lanes_[i].target == target) return true;
    }
    return false;
}

float* VoiceModulationBuffer::values_for_lane(
    std::uint32_t lane_index) noexcept {
    return values_.data() +
           static_cast<std::size_t>(lane_index) *
               static_cast<std::size_t>(max_frames_);
}

}  // namespace pulp::audio
