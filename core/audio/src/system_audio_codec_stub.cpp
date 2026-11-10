// System codec backend for platforms without one: every write is refused.

#include "system_audio_codec_backend.hpp"

namespace pulp::audio::detail {

bool system_backend_can_write(SystemAudioContainer, SystemAudioEncoding) {
    return false;
}

std::vector<std::uint32_t> system_backend_aac_sample_rates() {
    return {};
}

bool system_backend_write(const std::string&, const AudioFileData&,
                          const SystemAudioWriteOptions&) {
    return false;
}

} // namespace pulp::audio::detail
