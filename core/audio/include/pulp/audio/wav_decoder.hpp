#pragma once

#include <pulp/audio/audio_file.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace pulp::audio {

/// Resource ceilings checked before the decoder allocates output storage.
/// Import surfaces handling untrusted media should set limits appropriate to
/// their own memory budget.
struct WavDecodeLimits {
    std::uint64_t max_frames = 100'000'000u;
    std::uint32_t max_channels = 64u;
    std::uint64_t max_output_bytes = 512u * 1024u * 1024u;
};

/// Inspect a complete in-memory RIFF/WAVE file without decoding its samples.
[[nodiscard]] std::optional<AudioFileInfo> inspect_wav(
    std::span<const std::uint8_t> bytes);

/// Decode a complete in-memory RIFF/WAVE file into deinterleaved float channels.
///
/// This surface performs no file I/O and compiles with exceptions disabled.
/// It rejects truncated streams and checks frame, channel, and decoded-byte
/// ceilings before allocating output storage.
[[nodiscard]] std::optional<AudioFileData> decode_wav(
    std::span<const std::uint8_t> bytes,
    WavDecodeLimits limits = {});

} // namespace pulp::audio
