#include <pulp/audio/device.hpp>

#include <atomic>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#elif defined(__linux__)
#include "../platform/linux/alsa_device.hpp"
#ifdef PULP_HAS_JACK
#include "../platform/linux/jack_device.hpp"
#endif
#endif

#if defined(__linux__)
namespace pulp::audio::linux_platform::detail {

std::uint64_t next_linux_audio_route_instance_token() noexcept {
    static std::atomic<std::uint64_t> next{1};
    auto candidate = next.load(std::memory_order_relaxed);
    while (candidate != std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(
                candidate, candidate + 1,
                std::memory_order_relaxed, std::memory_order_relaxed)) {
            return candidate;
        }
    }
    return 0;
}

} // namespace pulp::audio::linux_platform::detail
#endif

namespace pulp::audio {

#if defined(__linux__)
std::optional<AudioIoTiming> query_audio_io_timing(
    const AudioDevice& device) noexcept {
    if (const auto* alsa =
            dynamic_cast<const linux_platform::AlsaDevice*>(&device)) {
        return alsa->audio_io_timing();
    }
#ifdef PULP_HAS_JACK
    if (const auto* jack =
            dynamic_cast<const linux_platform::JackDevice*>(&device)) {
        return jack->audio_io_timing();
    }
#endif
    return std::nullopt;
}
#elif (!defined(__APPLE__) || TARGET_OS_IPHONE) && !defined(_WIN32)
std::optional<AudioIoTiming> query_audio_io_timing(
    const AudioDevice&) noexcept {
    return std::nullopt;
}
#endif

} // namespace pulp::audio
