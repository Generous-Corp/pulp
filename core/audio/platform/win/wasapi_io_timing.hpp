#pragma once

#include <pulp/audio/audio_io_timing.hpp>

#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#if defined(_WIN32)
#include <audioclient.h>
#include <windows.h>
#endif

namespace pulp::audio::win {

enum class WasapiTimingDirection : std::uint8_t {
    render,
    capture,
};

struct WasapiTimingValues {
    std::optional<std::uint64_t> stream_latency_100ns;
    std::optional<std::uint32_t> endpoint_buffer_frames;
    std::uint32_t sample_rate_hz = 0;
    WasapiTimingDirection direction = WasapiTimingDirection::render;
};

/// Convert a WASAPI 100-nanosecond duration to a conservative frame count.
/// Ceiling preserves the API's maximum-latency guarantee instead of rounding a
/// fractional frame below the reported bound.
constexpr std::optional<std::uint32_t>
wasapi_duration_to_frames(std::uint64_t duration_100ns, std::uint32_t sample_rate_hz) noexcept {
    constexpr std::uint64_t units_per_second = 10'000'000;
    constexpr auto max_frames = std::numeric_limits<std::uint32_t>::max();

    if (!is_supported_audio_sample_rate(static_cast<double>(sample_rate_hz))) {
        return std::nullopt;
    }

    const auto whole_seconds = duration_100ns / units_per_second;
    if (whole_seconds > max_frames / sample_rate_hz) {
        return std::nullopt;
    }

    const auto remainder = duration_100ns % units_per_second;
    const auto remainder_product = remainder * sample_rate_hz;
    const auto fractional_frames =
        remainder_product / units_per_second + (remainder_product % units_per_second != 0 ? 1 : 0);
    const auto whole_frames = whole_seconds * sample_rate_hz;
    if (fractional_frames > max_frames - whole_frames) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(whole_frames + fractional_frames);
}

/// Assemble the immutable report cached by one initialized IAudioClient.
/// GetStreamLatency already includes the endpoint buffer in its maximum path.
/// AudioIoTiming composes its buffer separately, so the latency field carries
/// only the non-buffer residual and reconstructs the reported maximum exactly.
constexpr std::optional<AudioIoTiming>
make_wasapi_audio_io_timing(const WasapiTimingValues& values, std::uint64_t route_instance_token,
                            std::uint64_t calibration_generation) noexcept {
    if (!values.stream_latency_100ns || !values.endpoint_buffer_frames ||
        *values.endpoint_buffer_frames == 0 || route_instance_token == 0 ||
        calibration_generation == 0) {
        return std::nullopt;
    }

    const auto maximum_frames =
        wasapi_duration_to_frames(*values.stream_latency_100ns, values.sample_rate_hz);
    if (!maximum_frames || *maximum_frames < *values.endpoint_buffer_frames) {
        return std::nullopt;
    }

    AudioIoTiming timing{};
    const auto residual = *maximum_frames - *values.endpoint_buffer_frames;
    if (values.direction == WasapiTimingDirection::capture) {
        timing.input_latency_frames = residual;
        timing.input_safety_offset_frames = 0;
    } else {
        timing.output_latency_frames = residual;
        timing.output_safety_offset_frames = 0;
    }
    timing.io_buffer_frames = *values.endpoint_buffer_frames;
    timing.sample_rate_hz = static_cast<double>(values.sample_rate_hz);
    timing.timestamp_domain = AudioTimestampDomain::device_sample_frames;
    timing.timestamp_source = AudioTimingSource::os_estimate;
    timing.confidence = AudioTimingConfidence::reported;
    timing.route_instance_token = route_instance_token;
    timing.calibration_generation = calibration_generation;
    return timing;
}

/// Control-thread publication with an invalidation path safe for the WASAPI
/// I/O thread. The report remains immutable while valid; invalidation only
/// withdraws publication and never mutates storage read by another thread.
class WasapiTimingPublication {
  public:
    void publish(std::optional<AudioIoTiming> timing) noexcept {
        timing_ = std::move(timing);
        valid_.store(timing_.has_value(), std::memory_order_release);
    }

    void invalidate() noexcept {
        valid_.store(false, std::memory_order_release);
    }

    std::optional<AudioIoTiming> read() const noexcept {
        return valid_.load(std::memory_order_acquire) ? timing_ : std::nullopt;
    }

  private:
    std::optional<AudioIoTiming> timing_;
    std::atomic<bool> valid_{false};
};

/// Per-open idempotence for route-loss notification. Cleanup may observe the
/// same terminal result again after the I/O thread has already notified.
class WasapiRouteInvalidationGate {
  public:
    void reset() noexcept {
        state_.store(State::ready, std::memory_order_release);
    }

    void mark_pending() noexcept {
        auto expected = State::ready;
        state_.compare_exchange_strong(expected, State::pending, std::memory_order_acq_rel);
    }

    bool take_notification() noexcept {
        auto expected = State::pending;
        return state_.compare_exchange_strong(expected, State::notified, std::memory_order_acq_rel);
    }

  private:
    enum class State : std::uint8_t {
        ready,
        pending,
        notified,
    };
    std::atomic<State> state_{State::ready};
};

#if defined(_WIN32)
constexpr bool wasapi_result_invalidates_route(HRESULT result) noexcept {
    return result == AUDCLNT_E_DEVICE_INVALIDATED || result == AUDCLNT_E_RESOURCES_INVALIDATED ||
           result == AUDCLNT_E_SERVICE_NOT_RUNNING;
}
#endif

} // namespace pulp::audio::win
