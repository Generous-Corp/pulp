#pragma once

#include <cmath>
#include <cstdint>

namespace pulp::audio {

enum class SampleStreamConsumptionStatus : std::uint8_t {
    Allowed,
    LegacyRejected,
    HeritageRejected,
};

struct SampleStreamConsumptionFactors {
    double pitch_ratio = 1.0;
    double clock_ratio = 1.0;
    double live_source_consumption_ratio = 1.0;
};

struct SampleStreamConsumptionEvaluation {
    SampleStreamConsumptionStatus status =
        SampleStreamConsumptionStatus::LegacyRejected;
    double effective_ratio = 0.0;

    bool allowed() const noexcept {
        return status == SampleStreamConsumptionStatus::Allowed;
    }
};

inline SampleStreamConsumptionEvaluation evaluate_sample_stream_consumption(
    const SampleStreamConsumptionFactors& factors,
    double maximum_ratio) noexcept {
    SampleStreamConsumptionEvaluation result;
    const auto positive_finite = [](double value) noexcept {
        return value > 0.0 && std::isfinite(value);
    };
    if (!positive_finite(factors.pitch_ratio) ||
        !positive_finite(maximum_ratio)) {
        return result;
    }
    if (!positive_finite(factors.clock_ratio) ||
        !positive_finite(factors.live_source_consumption_ratio)) {
        result.status = SampleStreamConsumptionStatus::HeritageRejected;
        return result;
    }

    const auto clocked = factors.pitch_ratio * factors.clock_ratio;
    const auto effective = clocked * factors.live_source_consumption_ratio;
    if (!positive_finite(clocked) || !positive_finite(effective) ||
        effective > maximum_ratio) {
        result.status = factors.clock_ratio == 1.0 &&
                                factors.live_source_consumption_ratio == 1.0
                            ? SampleStreamConsumptionStatus::LegacyRejected
                            : SampleStreamConsumptionStatus::HeritageRejected;
        return result;
    }
    result.status = SampleStreamConsumptionStatus::Allowed;
    result.effective_ratio = effective;
    return result;
}

struct SampleStreamConsumptionDeclaration {
    double source_frames_per_second = 0.0;
};

}  // namespace pulp::audio
