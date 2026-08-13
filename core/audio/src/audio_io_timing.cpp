#include <pulp/audio/device.hpp>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace pulp::audio {

#if (!defined(__APPLE__) || TARGET_OS_IPHONE) && !defined(_WIN32)
std::optional<AudioIoTiming> query_audio_io_timing(
    const AudioDevice&) noexcept {
    return std::nullopt;
}
#endif

} // namespace pulp::audio
