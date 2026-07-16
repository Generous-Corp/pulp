#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace pulp::audio {

enum class SamplePreloadContractStatus : std::uint8_t {
    Ok,
    InvalidSourceSampleRate,
    InvalidHostSampleRate,
    InvalidPlaybackRatio,
    InvalidLatency,
    Overflow,
};

struct SamplePreloadContract {
    double source_sample_rate = 0.0;
    double host_sample_rate = 0.0;
    double maximum_playback_ratio = 1.0;
    double certified_io_latency_seconds = 0.0;
    double scheduler_margin_seconds = 0.0;
    double decoder_latency_seconds = 0.0;
    std::uint64_t maximum_host_block_frames = 0;
    std::uint64_t interpolation_guard_frames = 0;
    std::uint64_t loop_prefetch_guard_frames = 0;
    std::uint64_t configured_preload_frames = 0;
};

struct SamplePreloadContractResult {
    SamplePreloadContractStatus status = SamplePreloadContractStatus::InvalidSourceSampleRate;
    std::uint64_t latency_guard_frames = 0;
    std::uint64_t block_guard_frames = 0;
    std::uint64_t required_preload_frames = 0;
    bool sufficient = false;

    bool valid() const noexcept { return status == SamplePreloadContractStatus::Ok; }
};

/// Calculate the resident head required to cover a certified streaming service
/// interval at the fastest supported source-consumption ratio. The result also
/// reserves one maximum host block plus interpolation and loop-prefetch guards.
/// This is a control/import-thread calculation; it does not measure storage.
inline SamplePreloadContractResult evaluate_sample_preload_contract(
    const SamplePreloadContract& contract) noexcept {
    SamplePreloadContractResult result;

    const auto positive_finite = [](double value) noexcept {
        return value > 0.0 && std::isfinite(value);
    };
    const auto nonnegative_finite = [](double value) noexcept {
        return value >= 0.0 && std::isfinite(value);
    };
    if (!positive_finite(contract.source_sample_rate)) {
        result.status = SamplePreloadContractStatus::InvalidSourceSampleRate;
        return result;
    }
    if (!positive_finite(contract.host_sample_rate)) {
        result.status = SamplePreloadContractStatus::InvalidHostSampleRate;
        return result;
    }
    if (!positive_finite(contract.maximum_playback_ratio)) {
        result.status = SamplePreloadContractStatus::InvalidPlaybackRatio;
        return result;
    }
    if (!nonnegative_finite(contract.certified_io_latency_seconds) ||
        !nonnegative_finite(contract.scheduler_margin_seconds) ||
        !nonnegative_finite(contract.decoder_latency_seconds)) {
        result.status = SamplePreloadContractStatus::InvalidLatency;
        return result;
    }

    const double latency_seconds = contract.certified_io_latency_seconds +
                                   contract.scheduler_margin_seconds +
                                   contract.decoder_latency_seconds;
    if (!std::isfinite(latency_seconds)) {
        result.status = SamplePreloadContractStatus::Overflow;
        return result;
    }

    const double latency_frames = std::ceil(
        latency_seconds * contract.source_sample_rate * contract.maximum_playback_ratio);
    const double block_frames = std::ceil(
        static_cast<double>(contract.maximum_host_block_frames) *
        contract.source_sample_rate / contract.host_sample_rate *
        contract.maximum_playback_ratio);
    constexpr double kMaxFrames =
        static_cast<double>(std::numeric_limits<std::uint64_t>::max());
    if (!std::isfinite(latency_frames) || latency_frames >= kMaxFrames ||
        !std::isfinite(block_frames) || block_frames >= kMaxFrames) {
        result.status = SamplePreloadContractStatus::Overflow;
        return result;
    }

    result.latency_guard_frames = static_cast<std::uint64_t>(latency_frames);
    result.block_guard_frames = static_cast<std::uint64_t>(block_frames);

    const auto add_checked = [](std::uint64_t left,
                                std::uint64_t right,
                                std::uint64_t& sum) noexcept {
        if (right > std::numeric_limits<std::uint64_t>::max() - left) return false;
        sum = left + right;
        return true;
    };
    std::uint64_t required = 0;
    if (!add_checked(result.latency_guard_frames, result.block_guard_frames, required) ||
        !add_checked(required, contract.interpolation_guard_frames, required) ||
        !add_checked(required, contract.loop_prefetch_guard_frames, required)) {
        result.status = SamplePreloadContractStatus::Overflow;
        return result;
    }

    result.status = SamplePreloadContractStatus::Ok;
    result.required_preload_frames = required;
    result.sufficient = contract.configured_preload_frames >= required;
    return result;
}

}  // namespace pulp::audio
