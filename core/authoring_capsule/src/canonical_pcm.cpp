#include <pulp/authoring_capsule/canonical_pcm.hpp>

#include <pulp/authoring_capsule/limits.hpp>
#include <pulp/runtime/crypto.hpp>

// Declarations only: the dr_flac implementation is compiled into pulp::audio
// (core/audio/src/codecs.c), which this module already links privately. The
// vendored header is named by path because no linked target publishes it.
#include "../../../external/dr_libs/dr_flac.h"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

// The WAV and AIFF decode paths are self-contained chunk readers rather than
// calls into `pulp::audio`'s file readers. Those surfaces return only frames,
// channels, and a rate: they expose no format tag or compression type, so an
// IMA-ADPCM, A-law, or mu-law member would come back as plausible float
// samples instead of a refusal, and no `found` string could name what was
// actually seen. The capsule contract requires the opposite — a named refusal
// over a silent approximation — and the integer-to-float constants below are
// part of `kCanonicalPcmDecoderVersion`, so this module owns them rather than
// inheriting whatever a vendored decoder does today. FLAC is the exception
// where reimplementation would be reckless: dr_flac's frame decode is integer
// arithmetic that is bit-exact across ISAs, and its s32 output is taken raw so
// the float conversion still uses this module's own constants.

namespace pulp::authoring_capsule {
namespace {

static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == 4,
              "Canonical PCM is IEEE-754 binary32; the byte layout assumes it.");

// WAVE format tags (mmreg.h). Only PCM and IEEE float are admissible; the rest
// exist here so a refusal can name the codec the author actually handed us.
constexpr std::uint16_t kWaveFormatPcm = 0x0001;
constexpr std::uint16_t kWaveFormatAdpcm = 0x0002;
constexpr std::uint16_t kWaveFormatIeeeFloat = 0x0003;
constexpr std::uint16_t kWaveFormatAlaw = 0x0006;
constexpr std::uint16_t kWaveFormatMulaw = 0x0007;
constexpr std::uint16_t kWaveFormatImaAdpcm = 0x0011;
constexpr std::uint16_t kWaveFormatGsm610 = 0x0031;
constexpr std::uint16_t kWaveFormatMpegLayer3 = 0x0055;
constexpr std::uint16_t kWaveFormatExtensible = 0xFFFE;

/// Bytes 4..15 of every KSDATAFORMAT_SUBTYPE_* GUID that WAVE_FORMAT_EXTENSIBLE
/// uses. Bytes 0..3 carry the format tag, so a subformat is mappable exactly
/// when this tail matches and that tag is one we admit.
constexpr std::uint8_t kKsdataformatSubtypeTail[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
                                                       0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

constexpr std::size_t kFormHeaderBytes = 12;  // RIFF and AIFF FORM share the shape.
constexpr std::size_t kChunkHeaderBytes = 8;
constexpr std::size_t kMinFmtChunkBytes = 16;
constexpr std::size_t kExtensibleFmtChunkBytes = 40;
constexpr std::size_t kMinCommChunkBytes = 18;
constexpr std::size_t kMinAifcCommChunkBytes = 22;  // COMM plus the compression fourcc.
constexpr std::size_t kSsndPrologBytes = 8;         // offset + blockSize precede the samples.

enum class SourceFormat : std::uint8_t { pcm16, pcm24, pcm32, float32 };

struct WaveFormat {
    SourceFormat format = SourceFormat::pcm16;
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t bits_per_sample = 0;
    std::uint32_t block_align = 0;
};

CapsuleError make_error(CapsuleStatus status, std::string subject, std::string required,
                        std::string found) {
    CapsuleError error;
    error.status = status;
    error.subject = std::move(subject);
    error.required = std::move(required);
    error.found = std::move(found);
    return error;
}

CapsuleError unsupported(std::string subject, std::string required, std::string found) {
    return make_error(CapsuleStatus::decode_unsupported, std::move(subject), std::move(required),
                      std::move(found));
}

std::string hex_u16(std::uint16_t value) {
    static const char digits[] = "0123456789abcdef";
    std::string out = "0x";
    for (int shift = 12; shift >= 0; shift -= 4)
        out.push_back(digits[(value >> shift) & 0xF]);
    return out;
}

std::string hex_bytes(std::span<const std::uint8_t> bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0xF]);
    }
    return out;
}

std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset]) |
                                      static_cast<std::uint16_t>(bytes[offset + 1] << 8));
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint64_t read_u64(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i)
        value = (value << 8) | bytes[offset + static_cast<std::size_t>(i)];
    return value;
}

std::uint16_t read_u16_be(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8) |
                                      static_cast<std::uint16_t>(bytes[offset + 1]));
}

std::uint32_t read_u32_be(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

bool has_id(std::span<const std::uint8_t> bytes, std::size_t offset, const char (&id)[5]) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        return false;
    for (std::size_t i = 0; i < 4; ++i) {
        if (bytes[offset + i] != static_cast<std::uint8_t>(id[i]))
            return false;
    }
    return true;
}

/// Render a four-character chunk or form identifier as text when it is
/// printable ASCII, and as hex when it is not, so a `found` string is readable
/// for a real container and still unambiguous for a corrupt one.
std::string fourcc(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        return "truncated";
    std::string text;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto byte = bytes[offset + i];
        if (byte < 0x20 || byte > 0x7E)
            return hex_bytes(bytes.subspan(offset, 4));
        text.push_back(static_cast<char>(byte));
    }
    return text;
}

/// Name the container that was handed to us instead of the one we wanted, so
/// the refusal tells the author which file to convert.
std::string describe_container(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 4)
        return "truncated:" + std::to_string(bytes.size()) + "_bytes";
    if (has_id(bytes, 0, "RIFF")) {
        if (bytes.size() < kFormHeaderBytes)
            return "riff:truncated";
        return "riff:" + fourcc(bytes, 8);
    }
    if (has_id(bytes, 0, "RF64"))
        return "rf64";
    if (has_id(bytes, 0, "BW64"))
        return "bw64";
    if (has_id(bytes, 0, "OggS"))
        return "ogg";
    if (has_id(bytes, 0, "caff"))
        return "caf";
    if (bytes.size() >= 8 && has_id(bytes, 4, "ftyp"))
        return "iso-bmff";
    if (bytes.size() >= 3 && bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3')
        return "mp3:id3";
    return fourcc(bytes, 0);
}

std::string describe_format_tag(std::uint16_t tag) {
    const auto suffix = "(" + hex_u16(tag) + ")";
    switch (tag) {
        case kWaveFormatPcm: return "wave_format_pcm" + suffix;
        case kWaveFormatAdpcm: return "wave_format_adpcm" + suffix;
        case kWaveFormatIeeeFloat: return "wave_format_ieee_float" + suffix;
        case kWaveFormatAlaw: return "wave_format_alaw" + suffix;
        case kWaveFormatMulaw: return "wave_format_mulaw" + suffix;
        case kWaveFormatImaAdpcm: return "wave_format_ima_adpcm" + suffix;
        case kWaveFormatGsm610: return "wave_format_gsm610" + suffix;
        case kWaveFormatMpegLayer3: return "wave_format_mpeglayer3" + suffix;
        case kWaveFormatExtensible: return "wave_format_extensible" + suffix;
        default: return hex_u16(tag);
    }
}

struct ChunkSpan {
    std::size_t offset = 0;
    std::size_t size = 0;
    bool found = false;
};

struct IffChunks {
    ChunkSpan first;
    ChunkSpan second;
};

/// Walk an IFF-shaped chunk list — RIFF (little-endian sizes) or AIFF FORM
/// (big-endian sizes) — taking the first occurrence of each wanted chunk.
/// Every chunk must lie wholly inside the buffer: a declared size that runs
/// past the end is a truncated file, and guessing at the missing tail would be
/// the approximation this decoder refuses to make.
runtime::Result<IffChunks, CapsuleError> walk_chunks(std::span<const std::uint8_t> bytes,
                                                     bool big_endian, const char (&first_id)[5],
                                                     const char (&second_id)[5],
                                                     const std::string& container) {
    // A form size field that disagrees with the file length is common (streamed
    // writers, appended metadata). Clamping to the real length keeps those
    // readable while still bounding every chunk by bytes actually present.
    const auto read_size = [&](std::size_t offset) {
        return big_endian ? read_u32_be(bytes, offset) : read_u32(bytes, offset);
    };
    const auto declared = static_cast<std::uint64_t>(read_size(4)) + 8;
    const auto end = static_cast<std::size_t>(
        declared < bytes.size() ? declared : static_cast<std::uint64_t>(bytes.size()));
    if (end < kFormHeaderBytes) {
        return runtime::Err(unsupported(container + ".size", "at_least_12",
                                        std::to_string(declared)));
    }

    IffChunks chunks;
    std::size_t offset = kFormHeaderBytes;
    while (end - offset >= kChunkHeaderBytes) {
        const auto size = static_cast<std::size_t>(read_size(offset + 4));
        const auto payload = offset + kChunkHeaderBytes;
        if (size > end - payload) {
            return runtime::Err(unsupported(container + ".chunk", "chunk_within_file",
                                            fourcc(bytes, offset) + ":size=" +
                                                std::to_string(size)));
        }
        if (!chunks.first.found && has_id(bytes, offset, first_id)) {
            chunks.first = {payload, size, true};
        } else if (!chunks.second.found && has_id(bytes, offset, second_id)) {
            chunks.second = {payload, size, true};
        }
        const auto padding = size & 1u;
        const auto advance = kChunkHeaderBytes + size + padding;
        if (advance > end - offset)
            break;  // A pad byte elided at end of file ends the walk cleanly.
        offset += advance;
    }
    return runtime::Ok(chunks);
}

/// Resolve a `fmt ` chunk to one of the four admissible source formats, or
/// refuse naming the tag, depth, channel count, or subformat GUID seen.
runtime::Result<WaveFormat, CapsuleError> parse_format(std::span<const std::uint8_t> bytes,
                                                       const ChunkSpan& chunk) {
    if (chunk.size < kMinFmtChunkBytes) {
        return runtime::Err(unsupported("wav.fmt", "at_least_16_bytes",
                                        std::to_string(chunk.size) + "_bytes"));
    }
    const auto fmt = bytes.subspan(chunk.offset, chunk.size);

    auto tag = read_u16(fmt, 0);
    WaveFormat format;
    format.channels = read_u16(fmt, 2);
    format.sample_rate = read_u32(fmt, 4);
    format.block_align = read_u16(fmt, 12);
    format.bits_per_sample = read_u16(fmt, 14);

    if (tag == kWaveFormatExtensible) {
        if (chunk.size < kExtensibleFmtChunkBytes) {
            return runtime::Err(unsupported("wav.fmt.sub_format", "40_byte_extensible_fmt",
                                            std::to_string(chunk.size) + "_bytes"));
        }
        const auto guid = fmt.subspan(24, 16);
        if (std::memcmp(guid.data() + 4, kKsdataformatSubtypeTail,
                        sizeof(kKsdataformatSubtypeTail)) != 0 ||
            read_u16(guid, 2) != 0) {
            return runtime::Err(unsupported("wav.fmt.sub_format",
                                            "ksdataformat_subtype_pcm|ksdataformat_subtype_ieee_"
                                            "float",
                                            hex_bytes(guid)));
        }
        // The valid-bits field is advisory: extensible samples are left
        // justified in the container, so a 20-in-24 or 24-in-32 file decodes
        // correctly at the container depth. Only a value that exceeds the
        // container is malformed.
        const auto valid_bits = read_u16(fmt, 18);
        if (valid_bits > format.bits_per_sample) {
            return runtime::Err(unsupported("wav.fmt.valid_bits_per_sample",
                                            "at_most_" + std::to_string(format.bits_per_sample),
                                            std::to_string(valid_bits)));
        }
        tag = read_u16(guid, 0);
    }

    if (tag == kWaveFormatPcm) {
        switch (format.bits_per_sample) {
            case 16: format.format = SourceFormat::pcm16; break;
            case 24: format.format = SourceFormat::pcm24; break;
            case 32: format.format = SourceFormat::pcm32; break;
            default:
                return runtime::Err(unsupported("wav.fmt.bits_per_sample", "16|24|32",
                                                std::to_string(format.bits_per_sample)));
        }
    } else if (tag == kWaveFormatIeeeFloat) {
        if (format.bits_per_sample != 32) {
            return runtime::Err(unsupported("wav.fmt.bits_per_sample", "32",
                                            std::to_string(format.bits_per_sample)));
        }
        format.format = SourceFormat::float32;
    } else {
        return runtime::Err(unsupported("wav.fmt.audio_format",
                                        "wave_format_pcm|wave_format_ieee_float",
                                        describe_format_tag(tag)));
    }
    return runtime::Ok(format);
}

std::uint32_t bytes_per_sample(SourceFormat format) noexcept {
    switch (format) {
        case SourceFormat::pcm16: return 2;
        case SourceFormat::pcm24: return 3;
        case SourceFormat::pcm32: return 4;
        case SourceFormat::float32: return 4;
    }
    return 0;
}

// Integer-to-float scaling uses the full-scale magnitude 2^(bits-1), never
// 2^(bits-1) - 1. Three properties follow, and every audio member's identity
// depends on all three:
//   * the most negative code maps to exactly -1.0;
//   * the divisor is a power of two, so the scaling itself introduces no
//     rounding at all — it only decrements the exponent;
//   * for 16- and 24-bit the integer is exactly representable in binary32, so
//     the whole conversion is exact. Only 32-bit needs 31 significand bits and
//     rounds once, round-to-nearest-even, in the single narrowing step below.
// The cost is that positive full scale reads 1.0 - 2^-(bits-1) rather than 1.0,
// which is the standard asymmetry of two's-complement PCM and is preferred over
// a divisor that would make -1.0 inexact. At 32 bits that difference falls
// below binary32's resolution, so the largest positive code does round to
// exactly 1.0 — a property of the canonical float format, not of the scaling.
constexpr float kPcm16Scale = 1.0f / 32768.0f;      // 2^-15
constexpr float kPcm24Scale = 1.0f / 8388608.0f;    // 2^-23
constexpr double kPcm32Scale = 1.0 / 2147483648.0;  // 2^-31

float decode_sample(std::span<const std::uint8_t> data, std::size_t offset, SourceFormat format,
                    bool big_endian) noexcept {
    switch (format) {
        case SourceFormat::pcm16: {
            const auto raw = big_endian ? read_u16_be(data, offset) : read_u16(data, offset);
            auto value = static_cast<std::int32_t>(raw);
            if (value & 0x8000)
                value -= 0x10000;
            return static_cast<float>(value) * kPcm16Scale;
        }
        case SourceFormat::pcm24: {
            const auto raw =
                big_endian
                    ? ((static_cast<std::uint32_t>(data[offset]) << 16) |
                       (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
                       static_cast<std::uint32_t>(data[offset + 2]))
                    : (static_cast<std::uint32_t>(data[offset]) |
                       (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
                       (static_cast<std::uint32_t>(data[offset + 2]) << 16));
            auto value = static_cast<std::int32_t>(raw);
            if (value & 0x800000)
                value -= 0x1000000;
            return static_cast<float>(value) * kPcm24Scale;
        }
        case SourceFormat::pcm32: {
            const auto raw = big_endian ? read_u32_be(data, offset) : read_u32(data, offset);
            const auto value = static_cast<std::int32_t>(raw);
            return static_cast<float>(static_cast<double>(value) * kPcm32Scale);
        }
        case SourceFormat::float32: {
            const auto raw = big_endian ? read_u32_be(data, offset) : read_u32(data, offset);
            return std::bit_cast<float>(raw);
        }
    }
    return 0.0f;
}

/// Append `value` little-endian so the header's layout is fixed on every host
/// regardless of the host's own byte order.
void append_le(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

void append_le(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
}

/// Shared tail of every PCM decode path: budget-check before allocating,
/// convert sample by sample, validate. `data` must hold exactly
/// `frames * channels * bytes_per_sample(format)` bytes; callers prove that
/// from their own container's declarations before arriving here.
runtime::Result<CanonicalPcm, CapsuleError> decode_frames(std::span<const std::uint8_t> data,
                                                          SourceFormat format, bool big_endian,
                                                          std::uint32_t channels,
                                                          std::uint32_t sample_rate,
                                                          std::uint64_t frames) {
    const auto sample_bytes = bytes_per_sample(format);
    const auto samples = frames * channels;
    const auto canonical_bytes = samples * sizeof(float);
    // A canonical rendition too large to be one archive member cannot travel in
    // a capsule at all, so refuse before allocating rather than after.
    if (canonical_bytes > kCapsuleLimitsV1.max_member_expanded_bytes) {
        return runtime::Err(make_error(CapsuleStatus::archive_budget_exceeded,
                                       "canonical_pcm.bytes",
                                       std::to_string(kCapsuleLimitsV1.max_member_expanded_bytes),
                                       std::to_string(canonical_bytes)));
    }

    CanonicalPcm pcm;
    pcm.channels = channels;
    pcm.sample_rate = sample_rate;
    pcm.frame_count = frames;
    pcm.samples.resize(static_cast<std::size_t>(samples));
    for (std::size_t i = 0; i < pcm.samples.size(); ++i)
        pcm.samples[i] = decode_sample(data, i * sample_bytes, format, big_endian);

    // A float32 source can carry NaN or infinity straight through; the
    // canonical form cannot.
    auto valid = validate_canonical(pcm);
    if (valid.is_err())
        return runtime::Err(std::move(valid).error());
    return runtime::Ok(std::move(pcm));
}

runtime::Result<CanonicalPcm, CapsuleError> decode_wav(std::span<const std::uint8_t> wav) {
    auto chunks = walk_chunks(wav, /*big_endian=*/false, "fmt ", "data", "wav");
    if (chunks.is_err())
        return runtime::Err(std::move(chunks).error());
    if (!chunks->first.found)
        return runtime::Err(unsupported("wav.fmt", "present", "absent"));
    if (!chunks->second.found)
        return runtime::Err(unsupported("wav.data", "present", "absent"));

    auto format = parse_format(wav, chunks->first);
    if (format.is_err())
        return runtime::Err(std::move(format).error());

    if (format->channels == 0 || format->channels > kMaxCanonicalChannels) {
        return runtime::Err(unsupported("wav.fmt.channels",
                                        "1.." + std::to_string(kMaxCanonicalChannels),
                                        std::to_string(format->channels)));
    }
    if (format->sample_rate < kMinCanonicalSampleRate ||
        format->sample_rate > kMaxCanonicalSampleRate) {
        // No resampling here by design: the declared source rate travels with
        // the audio in the member header and the host resamples at play time.
        // A rate outside the admissible range is therefore a refusal, not a
        // conversion.
        return runtime::Err(unsupported("wav.fmt.sample_rate",
                                        std::to_string(kMinCanonicalSampleRate) + ".." +
                                            std::to_string(kMaxCanonicalSampleRate),
                                        std::to_string(format->sample_rate)));
    }

    const auto frame_bytes = bytes_per_sample(format->format) * format->channels;
    if (format->block_align != 0 && format->block_align != frame_bytes) {
        return runtime::Err(unsupported("wav.fmt.block_align", std::to_string(frame_bytes),
                                        std::to_string(format->block_align)));
    }
    const auto data_size = chunks->second.size;
    if (data_size % frame_bytes != 0) {
        // A trailing partial frame would have to be dropped or padded. Either
        // is a change to the audio the manifest will hash, so name it instead.
        return runtime::Err(unsupported("wav.data", "multiple_of_" + std::to_string(frame_bytes),
                                        std::to_string(data_size) + "_bytes"));
    }

    return decode_frames(wav.subspan(chunks->second.offset, data_size), format->format,
                         /*big_endian=*/false, format->channels, format->sample_rate,
                         static_cast<std::uint64_t>(data_size / frame_bytes));
}

/// Parse the COMM chunk's 80-bit extended-precision sample rate. Sample rates
/// are exact small integers; a fractional, negative, or non-finite value is a
/// refusal rather than a rounding, because the rate lands inside the hashed
/// canonical header.
runtime::Result<std::uint64_t, CapsuleError> parse_extended_rate(
    std::span<const std::uint8_t> comm, std::size_t offset) {
    const auto sign_exponent = read_u16_be(comm, offset);
    std::uint64_t mantissa = 0;
    for (std::size_t i = 0; i < 8; ++i)
        mantissa = (mantissa << 8) | comm[offset + 2 + i];

    if (sign_exponent & 0x8000)
        return runtime::Err(unsupported("aiff.comm.sample_rate", "positive", "negative"));
    const auto biased = static_cast<std::uint32_t>(sign_exponent & 0x7FFF);
    if (biased == 0x7FFF)
        return runtime::Err(unsupported("aiff.comm.sample_rate", "finite", "infinity_or_nan"));
    if (mantissa == 0)
        return runtime::Ok(std::uint64_t{0});

    // value = mantissa * 2^(biased - 16383 - 63); the integer bit is explicit.
    const int shift = static_cast<int>(biased) - 16383 - 63;
    if (shift > 0) {
        if (shift >= 64 || (mantissa >> (64 - shift)) != 0) {
            return runtime::Err(unsupported("aiff.comm.sample_rate",
                                            "at_most_" +
                                                std::to_string(kMaxCanonicalSampleRate),
                                            "wider_than_64_bits"));
        }
        return runtime::Ok(mantissa << shift);
    }
    if (shift < -63 || (mantissa & ((std::uint64_t{1} << -shift) - 1)) != 0)
        return runtime::Err(unsupported("aiff.comm.sample_rate", "integer_hz", "fractional_hz"));
    return runtime::Ok(mantissa >> -shift);
}

runtime::Result<CanonicalPcm, CapsuleError> decode_aiff(std::span<const std::uint8_t> bytes) {
    const bool aifc = has_id(bytes, 8, "AIFC");
    if (!aifc && !has_id(bytes, 8, "AIFF"))
        return runtime::Err(unsupported("aiff.form", "aiff|aifc", fourcc(bytes, 8)));

    auto chunks = walk_chunks(bytes, /*big_endian=*/true, "COMM", "SSND", "aiff");
    if (chunks.is_err())
        return runtime::Err(std::move(chunks).error());
    const auto& comm_chunk = chunks->first;
    const auto& ssnd_chunk = chunks->second;
    if (!comm_chunk.found)
        return runtime::Err(unsupported("aiff.comm", "present", "absent"));
    if (!ssnd_chunk.found)
        return runtime::Err(unsupported("aiff.ssnd", "present", "absent"));

    const auto min_comm = aifc ? kMinAifcCommChunkBytes : kMinCommChunkBytes;
    if (comm_chunk.size < min_comm) {
        return runtime::Err(unsupported("aiff.comm", "at_least_" + std::to_string(min_comm) +
                                                         "_bytes",
                                        std::to_string(comm_chunk.size) + "_bytes"));
    }
    const auto comm = bytes.subspan(comm_chunk.offset, comm_chunk.size);
    const auto channels = static_cast<std::uint32_t>(read_u16_be(comm, 0));
    const auto frames = static_cast<std::uint64_t>(read_u32_be(comm, 2));
    const auto bits = static_cast<std::uint32_t>(read_u16_be(comm, 6));
    auto rate = parse_extended_rate(comm, 8);
    if (rate.is_err())
        return runtime::Err(std::move(rate).error());

    // Resolve the sample encoding. Plain AIFF is always big-endian PCM; AIFF-C
    // declares a compression fourcc, of which only the uncompressed spellings
    // are admissible — anything else (ulaw, ima4, aac, ...) is a codec and is
    // refused by name.
    bool big_endian = true;
    bool is_float = false;
    if (aifc) {
        if (has_id(comm, 18, "NONE") || has_id(comm, 18, "twos")) {
            // Big-endian PCM, the default already set.
        } else if (has_id(comm, 18, "sowt")) {
            // Byte-swapped PCM as written by little-endian recorders; defined
            // for 16-bit sample points only.
            if (bits < 9 || bits > 16) {
                return runtime::Err(unsupported("aiff.comm.sample_size", "9..16_for_sowt",
                                                std::to_string(bits)));
            }
            big_endian = false;
        } else if (has_id(comm, 18, "fl32") || has_id(comm, 18, "FL32")) {
            is_float = true;
        } else {
            return runtime::Err(unsupported("aiff.comm.compression",
                                            "none|twos|sowt|fl32", fourcc(comm, 18)));
        }
    }

    SourceFormat format = SourceFormat::float32;
    if (is_float) {
        if (bits != 32)
            return runtime::Err(unsupported("aiff.comm.sample_size", "32", std::to_string(bits)));
    } else {
        // AIFF's sampleSize is the valid bit count and sample points occupy
        // ceil(bits / 8) bytes, left-justified with zero padding — so decoding
        // at the container depth is exact, the same rule the WAV extensible
        // path applies to 20-in-24 files.
        if (bits < 9 || bits > 32) {
            return runtime::Err(unsupported("aiff.comm.sample_size", "9..32",
                                            std::to_string(bits)));
        }
        switch ((bits + 7) / 8) {
            case 2: format = SourceFormat::pcm16; break;
            case 3: format = SourceFormat::pcm24; break;
            default: format = SourceFormat::pcm32; break;
        }
    }

    if (channels == 0 || channels > kMaxCanonicalChannels) {
        return runtime::Err(unsupported("aiff.comm.channels",
                                        "1.." + std::to_string(kMaxCanonicalChannels),
                                        std::to_string(channels)));
    }
    if (*rate < kMinCanonicalSampleRate || *rate > kMaxCanonicalSampleRate) {
        return runtime::Err(unsupported("aiff.comm.sample_rate",
                                        std::to_string(kMinCanonicalSampleRate) + ".." +
                                            std::to_string(kMaxCanonicalSampleRate),
                                        std::to_string(*rate)));
    }

    if (ssnd_chunk.size < kSsndPrologBytes) {
        return runtime::Err(unsupported("aiff.ssnd", "at_least_8_bytes",
                                        std::to_string(ssnd_chunk.size) + "_bytes"));
    }
    const auto ssnd = bytes.subspan(ssnd_chunk.offset, ssnd_chunk.size);
    const auto data_offset = static_cast<std::uint64_t>(read_u32_be(ssnd, 0));
    const auto block_size = read_u32_be(ssnd, 4);
    if (block_size != 0) {
        // A non-zero blockSize means the samples are block-aligned and need
        // deblocking — a rewrite of the audio this decoder refuses to guess at.
        return runtime::Err(unsupported("aiff.ssnd.block_size", "0",
                                        std::to_string(block_size)));
    }

    // COMM's frame count is authoritative, and the sound data must fit it
    // exactly. A short chunk is a truncated file; slack bytes would be audio
    // nobody declared. Either way the samples played would not be the samples
    // hashed, so both are refusals.
    const auto payload = static_cast<std::uint64_t>(ssnd_chunk.size - kSsndPrologBytes);
    const auto frame_bytes = static_cast<std::uint64_t>(bytes_per_sample(format)) * channels;
    const auto need = frames * frame_bytes;
    const auto available = data_offset <= payload ? payload - data_offset : 0;
    if (available != need) {
        return runtime::Err(unsupported("aiff.ssnd.bytes", std::to_string(need) + "_bytes",
                                        std::to_string(available) + "_bytes"));
    }

    return decode_frames(
        ssnd.subspan(kSsndPrologBytes + static_cast<std::size_t>(data_offset),
                     static_cast<std::size_t>(need)),
        format, big_endian, channels, static_cast<std::uint32_t>(*rate), frames);
}

struct FlacCloser {
    drflac* flac = nullptr;
    ~FlacCloser() {
        if (flac)
            drflac_close(flac);
    }
};

runtime::Result<CanonicalPcm, CapsuleError> decode_flac(std::span<const std::uint8_t> bytes) {
    drflac* flac = drflac_open_memory(bytes.data(), bytes.size(), nullptr);
    if (flac == nullptr)
        return runtime::Err(unsupported("flac.stream", "decodable_flac", "undecodable"));
    const FlacCloser closer{flac};

    const auto channels = static_cast<std::uint32_t>(flac->channels);
    if (channels == 0 || channels > kMaxCanonicalChannels) {
        return runtime::Err(unsupported("flac.channels",
                                        "1.." + std::to_string(kMaxCanonicalChannels),
                                        std::to_string(channels)));
    }
    if (flac->sampleRate < kMinCanonicalSampleRate ||
        flac->sampleRate > kMaxCanonicalSampleRate) {
        return runtime::Err(unsupported("flac.sample_rate",
                                        std::to_string(kMinCanonicalSampleRate) + ".." +
                                            std::to_string(kMaxCanonicalSampleRate),
                                        std::to_string(flac->sampleRate)));
    }
    const auto frames = static_cast<std::uint64_t>(flac->totalPCMFrameCount);
    if (frames == 0) {
        // STREAMINFO's total-samples field reads zero when the length is
        // unknown (a streamed encode). The member budget must be provable
        // before allocation, so an unbounded stream is a refusal.
        return runtime::Err(unsupported("flac.total_pcm_frame_count", "declared_in_streaminfo",
                                        "unknown"));
    }
    // Overflow-safe form of the decode_frames budget check, applied here too
    // because the s32 staging buffer below is the same size as the output.
    if (frames > kCapsuleLimitsV1.max_member_expanded_bytes /
                     (static_cast<std::uint64_t>(channels) * sizeof(float))) {
        return runtime::Err(make_error(CapsuleStatus::archive_budget_exceeded,
                                       "canonical_pcm.bytes",
                                       std::to_string(kCapsuleLimitsV1.max_member_expanded_bytes),
                                       std::to_string(frames) + "_frames"));
    }

    const auto samples = static_cast<std::size_t>(frames * channels);
    std::vector<drflac_int32> raw(samples);
    const auto read = drflac_read_pcm_frames_s32(flac, frames, raw.data());
    if (read != frames) {
        // Fewer frames than STREAMINFO promised is a truncated or corrupt
        // stream; padding the difference would hash audio that was never there.
        return runtime::Err(unsupported("flac.frames", std::to_string(frames) + "_frames",
                                        std::to_string(read) + "_frames"));
    }

    CanonicalPcm pcm;
    pcm.channels = channels;
    pcm.sample_rate = flac->sampleRate;
    pcm.frame_count = frames;
    pcm.samples.resize(samples);
    // dr_flac's frame decode is pure integer arithmetic (its SIMD variants
    // produce identical integers), and `s32` output is left-justified to 32
    // bits for every FLAC bit depth. One power-of-two scale therefore covers
    // all depths and lands on exactly the same values as the PCM16/PCM24
    // paths for sources of those depths: the conversion is exact up to 24
    // significant bits and single-rounded (to nearest even) beyond, the same
    // as the WAV pcm32 path. This determinism is what admits FLAC while MP3,
    // OGG, and AAC stay refused.
    for (std::size_t i = 0; i < samples; ++i)
        pcm.samples[i] = static_cast<float>(static_cast<double>(raw[i]) * kPcm32Scale);

    auto valid = validate_canonical(pcm);
    if (valid.is_err())
        return runtime::Err(std::move(valid).error());
    return runtime::Ok(std::move(pcm));
}

}  // namespace

std::vector<std::uint8_t> to_canonical_bytes(const CanonicalPcm& pcm) {
    // The fixed 28-byte header documented in canonical_pcm.hpp, then the
    // interleaved little-endian binary32 samples. Every field is assembled
    // byte by byte from its integer or IEEE bit pattern rather than memcpy'd,
    // so the result is identical on a big-endian host even though none is
    // currently supported. The header travels inside the member so its plain
    // SHA-256 is the audio's identity and a reader needs nothing else.
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kCanonicalPcmHeaderBytes + pcm.samples.size() * sizeof(float));
    for (const auto ch : kCanonicalPcmMagic)
        bytes.push_back(static_cast<std::uint8_t>(ch));
    append_le(bytes, kCanonicalPcmDecoderVersion);
    append_le(bytes, pcm.channels);
    append_le(bytes, pcm.sample_rate);
    append_le(bytes, pcm.frame_count);
    for (const auto sample : pcm.samples)
        append_le(bytes, std::bit_cast<std::uint32_t>(sample));
    return bytes;
}

runtime::Result<CanonicalPcm, CapsuleError> from_canonical_bytes(
    std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kCanonicalPcmHeaderBytes) {
        return runtime::Err(unsupported("canonical_pcm",
                                        "at_least_" +
                                            std::to_string(kCanonicalPcmHeaderBytes) + "_bytes",
                                        std::to_string(bytes.size()) + "_bytes"));
    }
    for (std::size_t i = 0; i < sizeof(kCanonicalPcmMagic); ++i) {
        if (bytes[i] != static_cast<std::uint8_t>(kCanonicalPcmMagic[i])) {
            return runtime::Err(unsupported("canonical_pcm.magic", "pulp.pcm",
                                            hex_bytes(bytes.first(sizeof(kCanonicalPcmMagic)))));
        }
    }
    // The version is inside the member, so this guard binds to the bytes
    // themselves: bytes produced by a future decoder cannot be silently
    // re-read as this version's rendition, no matter what metadata travelled
    // beside them.
    const auto decoder_version = read_u32(bytes, 8);
    if (decoder_version != kCanonicalPcmDecoderVersion) {
        return runtime::Err(unsupported("canonical_pcm.decoder_version",
                                        std::to_string(kCanonicalPcmDecoderVersion),
                                        std::to_string(decoder_version)));
    }
    const auto channels = read_u32(bytes, 12);
    const auto sample_rate = read_u32(bytes, 16);
    const auto frame_count = read_u64(bytes, 20);
    if (channels == 0 || channels > kMaxCanonicalChannels) {
        return runtime::Err(unsupported("canonical_pcm.channels",
                                        "1.." + std::to_string(kMaxCanonicalChannels),
                                        std::to_string(channels)));
    }
    if (frame_count >
        std::numeric_limits<std::uint64_t>::max() / (channels * sizeof(float))) {
        return runtime::Err(unsupported("canonical_pcm.frame_count", "representable_byte_count",
                                        std::to_string(frame_count)));
    }
    // Exactly to the end: trailing bytes would be content the header never
    // declared, hashed into the identity but never played.
    const auto expected = frame_count * channels * sizeof(float);
    const auto payload = bytes.size() - kCanonicalPcmHeaderBytes;
    if (expected != payload) {
        return runtime::Err(unsupported("canonical_pcm.bytes",
                                        std::to_string(expected) + "_bytes",
                                        std::to_string(payload) + "_bytes"));
    }

    CanonicalPcm pcm;
    pcm.channels = channels;
    pcm.sample_rate = sample_rate;
    pcm.frame_count = frame_count;
    pcm.samples.resize(static_cast<std::size_t>(frame_count) * channels);
    for (std::size_t i = 0; i < pcm.samples.size(); ++i) {
        pcm.samples[i] = std::bit_cast<float>(
            read_u32(bytes, kCanonicalPcmHeaderBytes + i * sizeof(float)));
    }

    auto valid = validate_canonical(pcm);
    if (valid.is_err())
        return runtime::Err(std::move(valid).error());
    return runtime::Ok(std::move(pcm));
}

runtime::Result<void, CapsuleError> validate_canonical(const CanonicalPcm& pcm) {
    if (pcm.channels == 0 || pcm.channels > kMaxCanonicalChannels) {
        return runtime::Err(unsupported("canonical_pcm.channels",
                                        "1.." + std::to_string(kMaxCanonicalChannels),
                                        std::to_string(pcm.channels)));
    }
    if (pcm.sample_rate < kMinCanonicalSampleRate || pcm.sample_rate > kMaxCanonicalSampleRate) {
        return runtime::Err(unsupported("canonical_pcm.sample_rate",
                                        std::to_string(kMinCanonicalSampleRate) + ".." +
                                            std::to_string(kMaxCanonicalSampleRate),
                                        std::to_string(pcm.sample_rate)));
    }
    // The declared frame count is what the member header publishes and what
    // the digest covers, so it has to agree with the buffer exactly — a buffer
    // one frame short would play a rendition nobody hashed.
    if (pcm.frame_count > std::numeric_limits<std::uint64_t>::max() / pcm.channels ||
        pcm.frame_count * pcm.channels != static_cast<std::uint64_t>(pcm.samples.size())) {
        return runtime::Err(unsupported("canonical_pcm.frame_count",
                                        std::to_string(pcm.samples.size() / pcm.channels) +
                                            "_frames",
                                        std::to_string(pcm.frame_count) + "_frames"));
    }
    for (std::size_t i = 0; i < pcm.samples.size(); ++i) {
        const auto sample = pcm.samples[i];
        if (std::isfinite(sample))
            continue;
        // A NaN admitted here hands machine B silence or a blown output the
        // author never heard, and it would hash to a stable identity while
        // doing so.
        const char* what = std::isnan(sample) ? "nan" : (sample > 0.0f ? "inf" : "-inf");
        return runtime::Err(
            unsupported("canonical_pcm.samples[" + std::to_string(i) + "]", "finite", what));
    }
    return {};
}

std::string canonical_pcm_digest(const CanonicalPcm& pcm) {
    // The identity of an audio member is the SHA-256 of exactly the bytes that
    // travel — header and samples, one buffer, no trailer. This is what lets
    // the substrate's uniform "row digest = digest of member bytes" rule hold
    // with no audio exception: the exporter's generic member hashing and the
    // importer's extraction check compute the same value without knowing this
    // member is audio. The rate, channel count, frame count, and decoder
    // version still separate identities, because they sit inside the hashed
    // header. The caller validates first; the digest is defined over the
    // fields as declared.
    //
    // Bare lowercase hex, no `sha256:` prefix: the prefix, where a field wants
    // one, is the manifest's spelling and not part of the digest.
    const auto bytes = to_canonical_bytes(pcm);
    return runtime::sha256_hex(bytes.data(), bytes.size());
}

runtime::Result<CanonicalPcm, CapsuleError> decode_to_canonical(
    std::span<const std::uint8_t> source) {
    // Dispatch on the container's own magic, never on a filename. Ogg-wrapped
    // FLAC deliberately lands in the refusal path: its leading `OggS` names
    // the container the user actually has.
    if (source.size() >= kFormHeaderBytes && has_id(source, 0, "RIFF") &&
        has_id(source, 8, "WAVE")) {
        return decode_wav(source);
    }
    if (source.size() >= kFormHeaderBytes && has_id(source, 0, "FORM"))
        return decode_aiff(source);
    if (has_id(source, 0, "fLaC"))
        return decode_flac(source);
    return runtime::Err(unsupported("audio.container", "riff_wave|aiff|flac",
                                    describe_container(source)));
}

}  // namespace pulp::authoring_capsule
