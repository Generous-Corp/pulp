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

/// Stream facts the decoder derives from a `fmt ` payload and a data size.
struct WavFormatFacts {
    std::uint16_t format_tag = 0u;            ///< wFormatTag as written
    std::uint16_t translated_format_tag = 0u; ///< sub-format tag when wFormatTag is EXTENSIBLE
    std::uint32_t channels = 0u;
    std::uint32_t sample_rate = 0u;
    std::uint32_t bits_per_sample = 0u; ///< container width (wBitsPerSample)
    std::uint32_t bytes_per_frame = 0u; ///< 0 for compressed formats
    std::uint64_t total_frames =
        0u; ///< data_size / bytes_per_frame, or the fact count for compressed formats
};

/// Interpret a `fmt ` payload the way `inspect_wav` and `decode_wav` do,
/// without any sample data present.
///
/// `data_size` is the `data` chunk payload size; `fact_sample_count` is the
/// `fact` chunk value when the file carries one. The result is what the
/// decoder would report for a file built from exactly these headers, so a
/// layout inspector that reads only the header window reaches the same
/// facts as a decoder holding the whole file. Returns nullopt when the
/// decoder would refuse the format.
[[nodiscard]] std::optional<WavFormatFacts>
interpret_wav_format(std::span<const std::uint8_t> fmt_payload, std::uint64_t data_size,
                     std::optional<std::uint64_t> fact_sample_count = std::nullopt);

/// Decode a complete in-memory RIFF/WAVE file into deinterleaved float channels.
///
/// This surface performs no file I/O and compiles with exceptions disabled.
/// It rejects truncated streams and checks frame, channel, and decoded-byte
/// ceilings before allocating output storage.
[[nodiscard]] std::optional<AudioFileData> decode_wav(
    std::span<const std::uint8_t> bytes,
    WavDecodeLimits limits = {});

} // namespace pulp::audio
