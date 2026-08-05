#pragma once

#include <pulp/audio/audio_io_timing.hpp>
#include <CoreAudio/CoreAudio.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace pulp::audio::mac {

/// Stable CoreAudio callback client-data whose lifetime is independent of the
/// owning device. Detach clears the owner first and then drains callbacks that
/// may already have loaded it. A failed CoreAudio unregister can therefore
/// retain this context safely without retaining or touching the device.
class CoreAudioTimingListenerContext {
public:
    using NotifyFn = void (*)(void*) noexcept;

    CoreAudioTimingListenerContext(void* owner, NotifyFn notify) noexcept
        : owner_(owner), notify_(notify) {}

    void notify() noexcept {
        in_flight_.fetch_add(1, std::memory_order_acq_rel);
        if (auto* owner = owner_.load(std::memory_order_acquire)) {
            notify_(owner);
        }
        if (in_flight_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lock(drain_mutex_);
            drained_.notify_all();
        }
    }

    void detach_and_wait() noexcept {
        owner_.store(nullptr, std::memory_order_release);
        std::unique_lock<std::mutex> lock(drain_mutex_);
        drained_.wait(lock, [this] {
            return in_flight_.load(std::memory_order_acquire) == 0;
        });
    }

    void attach(void* owner) noexcept {
        owner_.store(owner, std::memory_order_release);
    }

    bool is_attached() const noexcept {
        return owner_.load(std::memory_order_acquire) != nullptr;
    }

private:
    std::atomic<void*> owner_;
    NotifyFn notify_;
    std::atomic<std::uint32_t> in_flight_{0};
    std::mutex drain_mutex_;
    std::condition_variable drained_;
};

template <std::size_t N, typename RemoveFn>
bool remove_coreaudio_timing_listener_registrations(
    std::array<bool, N>& installed,
    RemoveFn&& remove) {
    bool complete = true;
    for (std::size_t i = 0; i < installed.size(); ++i) {
        if (!installed[i]) continue;
        if (remove(i)) {
            installed[i] = false;
        } else {
            complete = false;
        }
    }
    return complete;
}

constexpr bool coreaudio_timing_listener_context_reusable(
    bool removal_complete) noexcept {
    return removal_complete;
}

inline bool advance_coreaudio_timing_generation(
    std::atomic<std::uint64_t>& generation) noexcept {
    auto observed = generation.load(std::memory_order_relaxed);
    for (;;) {
        const auto next = next_calibration_generation(observed);
        if (!next) return false;
        if (generation.compare_exchange_weak(
                observed, *next, std::memory_order_release,
                std::memory_order_relaxed)) {
            return true;
        }
    }
}

template <typename QueryFn>
std::optional<AudioIoTiming> refresh_coreaudio_timing_bounded(
    std::atomic<std::uint64_t>& generation,
    std::atomic<bool>& generation_exhausted,
    std::atomic<bool>& dirty,
    QueryFn&& query) {
    constexpr unsigned kMaxAttempts = 3;
    for (unsigned attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (generation_exhausted.load(std::memory_order_acquire)) {
            dirty.store(false, std::memory_order_release);
            return std::nullopt;
        }

        dirty.store(false, std::memory_order_release);
        const auto observed = generation.load(std::memory_order_acquire);
        auto timing = query(observed);
        if (observed == generation.load(std::memory_order_acquire) &&
            !dirty.load(std::memory_order_acquire)) {
            return timing;
        }
    }

    // Continued property churn must not pin the control thread in this getter.
    // Refuse a possibly stale report and preserve the retry request for the
    // next call, when the route may have stabilized.
    dirty.store(true, std::memory_order_release);
    return std::nullopt;
}

struct CoreAudioTimingPropertyValues {
    std::optional<std::uint32_t> input_latency_frames;
    std::optional<std::uint32_t> output_latency_frames;
    std::optional<std::uint32_t> input_safety_offset_frames;
    std::optional<std::uint32_t> output_safety_offset_frames;
    std::optional<std::uint32_t> io_buffer_frames;
    double sample_rate_hz = 0.0;
};

constexpr std::optional<AudioIoTiming> make_coreaudio_audio_io_timing(
    const CoreAudioTimingPropertyValues& values,
    std::uint64_t route_instance_token,
    std::uint64_t calibration_generation) noexcept {
    if (!is_supported_audio_sample_rate(values.sample_rate_hz) ||
        route_instance_token == 0 || calibration_generation == 0 ||
        !values.io_buffer_frames || *values.io_buffer_frames == 0 ||
        (!values.input_latency_frames && !values.output_latency_frames &&
         !values.input_safety_offset_frames &&
         !values.output_safety_offset_frames)) {
        return std::nullopt;
    }

    AudioIoTiming timing{};
    timing.input_latency_frames = values.input_latency_frames;
    timing.output_latency_frames = values.output_latency_frames;
    timing.input_safety_offset_frames = values.input_safety_offset_frames;
    timing.output_safety_offset_frames = values.output_safety_offset_frames;
    timing.io_buffer_frames = values.io_buffer_frames;
    timing.sample_rate_hz = values.sample_rate_hz;
    timing.timestamp_domain = AudioTimestampDomain::device_sample_frames;
    timing.timestamp_source = AudioTimingSource::device_clock;
    timing.confidence = AudioTimingConfidence::reported;
    timing.route_instance_token = route_instance_token;
    timing.calibration_generation = calibration_generation;
    return timing;
}

constexpr bool coreaudio_timing_listener_coverage_complete(
    const std::array<bool, 6>& installed,
    const AudioIoTiming& timing) noexcept {
    // Indices mirror CoreAudioDevice::timing_property_addresses_: input/output
    // latency, input/output safety, nominal rate, then buffer size.
    return installed[4] && installed[5] &&
           (!timing.input_latency_frames || installed[0]) &&
           (!timing.output_latency_frames || installed[1]) &&
           (!timing.input_safety_offset_frames || installed[2]) &&
           (!timing.output_safety_offset_frames || installed[3]);
}

template <typename QueryFn>
std::optional<AudioIoTiming> refresh_coreaudio_timing_with_listener_coverage(
    std::atomic<std::uint64_t>& generation,
    std::atomic<bool>& generation_exhausted,
    std::atomic<bool>& dirty,
    const std::array<bool, 6>& installed,
    QueryFn&& query) {
    auto timing = refresh_coreaudio_timing_bounded(
        generation, generation_exhausted, dirty,
        std::forward<QueryFn>(query));
    if (timing &&
        !coreaudio_timing_listener_coverage_complete(installed, *timing)) {
        timing.reset();
    }
    return timing;
}

bool read_coreaudio_nominal_sample_rate(
    AudioDeviceID device_id,
    double& sample_rate);
bool coreaudio_device_exists(AudioDeviceID device_id);

} // namespace pulp::audio::mac
