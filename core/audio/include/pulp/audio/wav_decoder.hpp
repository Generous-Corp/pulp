#pragma once

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/wave_layout.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace pulp::audio {

/// Resource ceilings checked before the decoder allocates output storage.
/// Import surfaces handling untrusted media should set limits appropriate to
/// their own memory budget. The ceilings bind the frames the file holds and
/// a decode produces, never a larger count its header declares.
struct WavDecodeLimits {
    std::uint64_t max_frames = 100'000'000u;
    std::uint32_t max_channels = 64u;
    std::uint64_t max_output_bytes = 512u * 1024u * 1024u;
    /// Refuse a file whose header declares more frames than it holds,
    /// instead of decoding the whole frames present. For a caller that
    /// compares the media against a length declared elsewhere.
    bool refuse_truncated = false;
};

/// Inspect a complete in-memory RIFF/WAVE, RF64 or BW64 file without
/// decoding its samples, under the acceptance policy in wave_layout.hpp.
/// `num_frames` is the whole frames present; a file whose header declares
/// more reports `truncated` and the declared count in `declared_frames`.
[[nodiscard]] std::optional<AudioFileInfo> inspect_wav(
    std::span<const std::uint8_t> bytes);

/// Whether `decode_wav(bytes, limits)` decodes the file `inspect_wav(bytes)`
/// described as `info`: inside every ceiling, and not truncated when
/// `limits.refuse_truncated` is set. The one rule for strict callers that
/// inspect without decoding.
[[nodiscard]] bool wav_decode_admits(const AudioFileInfo& info,
                                     const WavDecodeLimits& limits) noexcept;

/// Interpret a `fmt ` payload the way `inspect_wav` and `decode_wav` do,
/// without any sample data present. `data_size` is the `data` chunk
/// payload size; `total_frames` is the whole frames it holds. Returns
/// nullopt when the decoder has no reader for the format.
[[nodiscard]] std::optional<WavFormatFacts>
interpret_wav_format(std::span<const std::uint8_t> fmt_payload, std::uint64_t data_size);

/// The fact chunk's sample count never changes the interpretation.
[[deprecated("the fact sample count is ignored; call interpret_wav_format(fmt_payload, "
             "data_size)")]] [[nodiscard]] std::optional<WavFormatFacts>
interpret_wav_format(std::span<const std::uint8_t> fmt_payload, std::uint64_t data_size,
                     std::optional<std::uint64_t> fact_sample_count);

/// Decode a complete in-memory RIFF/WAVE, RF64 or BW64 file into
/// deinterleaved float channels: the whole frames present, as `inspect_wav`
/// reports them, so a truncated file decodes the frames it holds unless
/// `limits.refuse_truncated` is set.
///
/// This surface performs no file I/O and compiles with exceptions disabled.
/// It checks frame, channel, and decoded-byte ceilings before allocating
/// output storage.
[[nodiscard]] std::optional<AudioFileData> decode_wav(
    std::span<const std::uint8_t> bytes,
    WavDecodeLimits limits = {});

} // namespace pulp::audio
