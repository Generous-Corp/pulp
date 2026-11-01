#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace pulp::runtime {

enum class ExternalInputStatus : std::uint8_t { valid, invalid, unavailable, denied, paused };

template <std::size_t Capacity> struct ExternalInputFrame {
    static_assert(Capacity >= 1 && Capacity <= 64);
    std::uint32_t source_id = 0;
    std::uint32_t epoch = 0;
    std::uint64_t sequence = 0;
    std::uint64_t provider_time_ns = 0;
    ExternalInputStatus status = ExternalInputStatus::unavailable;
    float confidence = 0.0f;
    std::uint8_t count = 0;
    std::array<float, Capacity> values{};
    static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }
};

static_assert(std::is_trivially_copyable_v<ExternalInputFrame<1>>);
static_assert(std::is_trivially_copyable_v<ExternalInputFrame<64>>);
using ExternalInputScalar = ExternalInputFrame<1>;

struct ExternalInputPolicy {
    std::uint32_t source_id = 0;
    float minimum_confidence = 0.0f;
    std::uint64_t max_age_samples = 0;
    std::uint8_t expected_count = 0;
};

enum class ExternalInputVerdict : std::uint8_t {
    accepted,
    unchanged,
    rejected_status,
    rejected_source,
    rejected_order,
    rejected_malformed,
    rejected_confidence
};

struct ExternalInputCounters {
    std::uint64_t accepted = 0, unchanged = 0, rejected_status = 0, rejected_source = 0,
                  rejected_order = 0, rejected_malformed = 0, rejected_confidence = 0, expired = 0,
                  epochs = 0;
};

template <std::size_t Capacity> class ExternalInputGate {
  public:
    using Frame = ExternalInputFrame<Capacity>;

    ExternalInputVerdict submit(const Frame& frame, const ExternalInputPolicy& policy) noexcept {
        if (has_frame_ && frame.epoch == epoch_ && frame.sequence == sequence_) {
            ++counters_.unchanged;
            return ExternalInputVerdict::unchanged;
        }
        if (policy.source_id != 0 && frame.source_id != policy.source_id) {
            ++counters_.rejected_source;
            return ExternalInputVerdict::rejected_source;
        }
        last_status_ = frame.status;
        if (frame.status != ExternalInputStatus::valid) {
            expire();
            ++counters_.rejected_status;
            return ExternalInputVerdict::rejected_status;
        }
        if (frame.count == 0 || frame.count > Capacity ||
            (policy.expected_count != 0 && frame.count != policy.expected_count) ||
            !std::isfinite(frame.confidence)) {
            ++counters_.rejected_malformed;
            return ExternalInputVerdict::rejected_malformed;
        }
        for (std::size_t i = 0; i < frame.count; ++i) {
            if (!std::isfinite(frame.values[i])) {
                ++counters_.rejected_malformed;
                return ExternalInputVerdict::rejected_malformed;
            }
        }
        if (frame.confidence < 0.0f || frame.confidence > 1.0f ||
            frame.confidence < policy.minimum_confidence) {
            ++counters_.rejected_confidence;
            return ExternalInputVerdict::rejected_confidence;
        }
        if (has_frame_ && frame.epoch < epoch_) {
            ++counters_.rejected_order;
            return ExternalInputVerdict::rejected_order;
        }
        if (has_frame_ && frame.epoch == epoch_ &&
            (frame.sequence <= sequence_ || frame.provider_time_ns < provider_time_ns_)) {
            ++counters_.rejected_order;
            return ExternalInputVerdict::rejected_order;
        }
        values_ = frame.values;
        count_ = frame.count;
        has_frame_ = true;
        epoch_ = frame.epoch;
        sequence_ = frame.sequence;
        provider_time_ns_ = frame.provider_time_ns;
        age_samples_ = 0;
        last_status_ = frame.status;
        ++counters_.accepted;
        if (frame.epoch != prior_epoch_) {
            ++counters_.epochs;
            prior_epoch_ = frame.epoch;
        }
        return ExternalInputVerdict::accepted;
    }

    bool advance(std::uint64_t samples, const ExternalInputPolicy& policy) noexcept {
        if (!has_frame_ || policy.max_age_samples == 0)
            return false;
        age_samples_ = (UINT64_MAX - age_samples_ < samples) ? UINT64_MAX : age_samples_ + samples;
        if (age_samples_ >= policy.max_age_samples) {
            expire();
            ++counters_.expired;
            return true;
        }
        return false;
    }
    void expire() noexcept {
        has_frame_ = false;
        count_ = 0;
        age_samples_ = 0;
    }
    bool valid() const noexcept {
        return has_frame_;
    }
    std::span<const float> values() const noexcept {
        return {values_.data(), static_cast<std::size_t>(has_frame_ ? count_ : 0)};
    }
    float value(std::size_t index, float fallback) const noexcept {
        return has_frame_ && index < count_ ? values_[index] : fallback;
    }
    std::uint32_t epoch() const noexcept {
        return epoch_;
    }
    std::uint64_t sequence() const noexcept {
        return sequence_;
    }
    std::uint64_t age_samples() const noexcept {
        return age_samples_;
    }
    ExternalInputStatus last_status() const noexcept {
        return last_status_;
    }
    const ExternalInputCounters& counters() const noexcept {
        return counters_;
    }
    void reset_counters() noexcept {
        counters_ = {};
    }

  private:
    std::array<float, Capacity> values_{};
    ExternalInputCounters counters_{};
    ExternalInputStatus last_status_ = ExternalInputStatus::unavailable;
    std::uint32_t epoch_ = 0, prior_epoch_ = 0;
    std::uint64_t sequence_ = 0, provider_time_ns_ = 0, age_samples_ = 0;
    std::uint8_t count_ = 0;
    bool has_frame_ = false;
};

} // namespace pulp::runtime
