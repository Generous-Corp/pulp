#pragma once

// Platform backend for write_system_audio_file(). Exactly one of
// system_audio_codec_apple.mm, system_audio_codec_win.cpp and
// system_audio_codec_stub.cpp is compiled into pulp-audio.

#include <pulp/audio/system_audio_codec.hpp>

namespace pulp::audio::detail {

/// Whether the backend can write `encoding` into `container`.
bool system_backend_can_write(SystemAudioContainer container, SystemAudioEncoding encoding);

/// Encodes already-validated data: at least one channel, every channel the
/// same non-zero length, a non-zero sample rate, and a supported combination.
bool system_backend_write(const std::string& path, const AudioFileData& data,
                          const SystemAudioWriteOptions& options);

} // namespace pulp::audio::detail
