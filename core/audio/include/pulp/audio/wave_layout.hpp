#pragma once

/// @file wave_layout.hpp
/// The RIFF/WAVE chunk walk and acceptance policy every Pulp WAV reader
/// shares: the byte-span decoder, the format registry, the streaming
/// AudioReader, the memory-mapped reader and the layout report.
///
/// The rule: a header that contradicts itself is refused; a header that
/// merely declares more than the file holds is recovered by the whole frames
/// present and reported as truncated, never silently. Concretely:
///
/// - Accepted: RIFF, and RF64 or BW64 with a leading ds64 chunk (EBU Tech
///   3306, ITU-R BS.2088); a data size of 0xFFFFFFFF resolves through ds64,
///   and an RF64 file under 4 GiB with real 32-bit sizes is legal. A fmt
///   chunk after the data chunk, and a byte-for-byte repeat of the fmt
///   chunk, are accepted with a warning.
/// - Recovered and reported truncated: a data chunk running past the end of
///   the file, a data size of 0xFFFFFFFF that no ds64 entry resolves (no
///   ds64, or a ds64 data size of zero left by a recorder that never
///   back-filled it), and a data size that is not a whole number of frames.
///   A RIFF size wrong in either direction, and an odd chunk ending the file
///   without its pad byte, are warnings only.
/// - Refused: no RIFF/RF64/BW64 WAVE header (Sony Wave64 included), no fmt
///   or no data chunk, a fmt payload under 16 bytes, a second fmt that
///   differs from the first in a field the decoder interprets, an RF64 data
///   size whose 32-bit field and ds64 entry disagree, zero channels or zero
///   rate, more than 256 channels, a block align smaller than the frame or
///   ADPCM block header it must hold, and a data chunk that declares bytes
///   but holds no whole frame.
///
/// The authoring capsule's canonical PCM parser
/// (core/authoring_capsule/src/canonical_pcm.cpp) is the one deliberate
/// exception: it hashes the bytes it seals, so it parses WAV strictly on its
/// own and refuses anything this policy would recover.
///
/// This header is dependency-free (no CHOC, no dr_wav, no iostream, no
/// exceptions) so the web lanes compile it with the portable decoder.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace pulp::audio {

/// Specification violations a container walk can prove from bytes alone.
/// Anything not on this list is a fact or a decoder refusal, never a warning.
enum class LayoutWarningKind {
    envelope_size_mismatch,           ///< declared envelope end differs from the file size
    missing_pad_byte,                 ///< odd-sized chunk ends the file without its pad byte
    duplicate_fmt,                    ///< more than one `fmt ` chunk
    fmt_after_data,                   ///< `fmt ` follows `data`
    truncated_data,                   ///< `data` payload shorter than declared or not whole frames
    sentinel_size_without_ds64,       ///< 0xFFFFFFFF size with no ds64 entry to resolve it
    chunk_overlap,                    ///< two regions claim the same bytes
    chunk_past_eof,                   ///< a non-data region extends past end of file
    extensible_mask_channel_mismatch, ///< channel mask population differs from the channel count
    valid_bits_above_container,       ///< wValidBitsPerSample exceeds wBitsPerSample
};

/// Random-access reads over the bytes being walked: a span, an
/// AudioByteSource, or the layout report's bounded reader. `read_at` copies
/// up to `count` bytes from `offset` and returns how many it copied, which
/// is short only at the end of the input. It receives the cursor, so a
/// reader keeps its state in `context` and may use `size`.
struct WaveByteCursor {
    using ReadAt = std::size_t (*)(const WaveByteCursor& cursor, std::uint64_t offset,
                                   std::uint8_t* out, std::size_t count) noexcept;

    void* context = nullptr;
    ReadAt read_at = nullptr;
    std::uint64_t size = 0u;

    /// A cursor over an in-memory span, which must outlive the cursor.
    [[nodiscard]] static WaveByteCursor over(std::span<const std::uint8_t> bytes) noexcept;

    /// Copy up to `count` bytes at `offset`; 0 past the end or without a reader.
    [[nodiscard]] std::size_t read(std::uint64_t offset, std::uint8_t* out,
                                   std::size_t count) const noexcept;
};

/// Envelope the file declares.
enum class WaveEnvelope : std::uint8_t { riff, rf64, bw64 };

/// Why a WAV is refused. `none` means it decodes.
enum class WaveRefusal : std::uint8_t {
    none,
    not_wave,              ///< no RIFF, RF64 or BW64 WAVE header in the first 12 bytes
    no_fmt,                ///< no fmt chunk
    fmt_too_short,         ///< fmt payload under 16 bytes
    conflicting_fmt,       ///< a second fmt differs in an interpreted field
    conflicting_data_size, ///< RF64 data size field and ds64 entry disagree
    no_data,               ///< no data chunk
    unsupported_format,    ///< the decoder has no reader for the fmt chunk
    zero_channels,
    zero_sample_rate,
    block_align_too_small, ///< nBlockAlign smaller than the frame the fmt describes
    no_whole_frame,        ///< the data chunk declares bytes but holds no whole frame
    too_many_channels,     ///< more channels than the decoder reads (256)
};

/// One sentence naming the refusal, for reports and logs.
[[nodiscard]] const char* wave_refusal_reason(WaveRefusal refusal) noexcept;

/// A warning with the numbers its message needs. `offset` is the chunk
/// header (or 0 for the envelope); `declared` and `actual` are the two sides
/// of the disagreement in bytes.
struct WaveWarning {
    LayoutWarningKind kind = LayoutWarningKind::envelope_size_mismatch;
    std::uint64_t offset = 0u;
    std::uint64_t declared = 0u;
    std::uint64_t actual = 0u;
};

/// The first fmt chunk: where it is, what it declares, and up to the first
/// 40 payload bytes (the WAVE_FORMAT_EXTENSIBLE size; nothing past it is
/// interpreted).
struct WaveFormatChunk {
    static constexpr std::size_t kMaxPayload = 40u;
    std::uint64_t offset = 0u;
    std::uint64_t declared = 0u;
    std::array<std::uint8_t, kMaxPayload> payload{};
    std::uint32_t payload_size = 0u;

    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        return {payload.data(), payload_size};
    }
};

/// What the walk found. Reads only chunk headers and the fmt, ds64 and fact
/// payloads, so a multi-gigabyte file costs a handful of small reads.
struct WaveLayout {
    WaveEnvelope envelope = WaveEnvelope::riff;
    std::uint64_t file_size = 0u;
    /// The envelope's declared size (the RIFF size field, or ds64's when the
    /// field is the 0xFFFFFFFF sentinel); the envelope ends 8 bytes later.
    std::uint64_t riff_size = 0u;

    bool has_ds64 = false;
    std::uint64_t ds64_data_size = 0u;
    std::uint64_t ds64_sample_count = 0u;

    bool has_fmt = false;
    WaveFormatChunk fmt;
    std::optional<std::uint64_t> fact_sample_count;

    bool has_data = false;
    std::uint64_t data_header_offset = 0u;
    std::uint64_t data_offset = 0u;   ///< first payload byte
    std::uint64_t data_declared = 0u; ///< payload size the header declares, ds64-resolved
    std::uint64_t data_present = 0u;  ///< bytes of that payload the file holds

    std::vector<WaveWarning> warnings;
    /// A refusal the walk alone can prove; `decide_wave` adds the ones that
    /// need the interpreted format.
    WaveRefusal refusal = WaveRefusal::none;

    [[nodiscard]] bool has_warning(LayoutWarningKind kind) const noexcept;
};

/// Walk the chunks of the bytes behind `cursor`.
[[nodiscard]] WaveLayout walk_wave_layout(const WaveByteCursor& cursor);

/// Stream facts the decoder derives from a fmt payload and a data size.
struct WavFormatFacts {
    std::uint16_t format_tag = 0u;            ///< wFormatTag as written
    std::uint16_t translated_format_tag = 0u; ///< sub-format tag when wFormatTag is EXTENSIBLE
    std::uint32_t channels = 0u;
    std::uint32_t sample_rate = 0u;
    std::uint32_t bits_per_sample = 0u; ///< container width (wBitsPerSample)
    std::uint32_t bytes_per_frame = 0u; ///< 0 for compressed formats
    /// Whole frames the data size holds: the frames a decode produces.
    std::uint64_t total_frames = 0u;
    /// True when the data size declares more than `total_frames` whole frames
    /// (a partial last frame); `walk_wave_layout` callers also count bytes the
    /// file does not hold.
    bool truncated = false;
    /// Frames the data size declares, a partial last frame counted.
    std::uint64_t declared_frames = 0u;
};

/// The acceptance decision for one walked file.
struct WaveDecision {
    WaveRefusal refusal = WaveRefusal::none;
    std::uint64_t recoverable_frames = 0u;     ///< whole frames present: what decodes
    std::uint64_t recoverable_data_bytes = 0u; ///< bytes those frames occupy
    std::uint64_t declared_frames = 0u;        ///< frames the header declares
    bool truncated = false;                    ///< recoverable_frames < declared_frames

    [[nodiscard]] bool accepted() const noexcept {
        return refusal == WaveRefusal::none;
    }
};

/// Decide what a walked file decodes to. `facts` is the decoder's
/// interpretation of `layout.fmt` (nullopt when it has no reader for it).
/// Pure: reads nothing.
[[nodiscard]] WaveDecision decide_wave(const WaveLayout& layout,
                                       const std::optional<WavFormatFacts>& facts) noexcept;

/// A minimal RIFF (or, past 4 GiB of data, RF64) header: envelope, ds64
/// when needed, a fmt chunk of 16 bytes (40 for WAVE_FORMAT_EXTENSIBLE), and
/// the data chunk header declaring `data_bytes`. The decoder reads this
/// header followed by exactly `data_bytes` of the file's data payload, so it
/// only ever sees a canonical stream.
struct CanonicalWaveHeader {
    std::array<std::uint8_t, 112> bytes{};
    std::size_t size = 0u;

    [[nodiscard]] std::span<const std::uint8_t> view() const noexcept {
        return {bytes.data(), size};
    }
};

/// Header for a fmt payload (at least 16 bytes) and a data size.
[[nodiscard]] CanonicalWaveHeader canonical_wave_header(std::span<const std::uint8_t> fmt_payload,
                                                        std::uint64_t data_bytes) noexcept;

/// Header for a walked layout's fmt and a data size.
[[nodiscard]] CanonicalWaveHeader canonical_wave_header(const WaveLayout& layout,
                                                        std::uint64_t data_bytes) noexcept;

} // namespace pulp::audio
