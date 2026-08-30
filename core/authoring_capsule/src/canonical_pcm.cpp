#include <pulp/authoring_capsule/canonical_pcm.hpp>

#include <pulp/authoring_capsule/limits.hpp>
#include <pulp/runtime/crypto.hpp>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

// The decode path is a self-contained RIFF/WAVE reader rather than a call into
// `pulp::audio::decode_wav`. That surface returns only frames, channels, and a
// rate: it exposes no format tag, so an IMA-ADPCM, A-law, or mu-law member
// would come back as plausible float samples instead of a refusal, and no
// `found` string could name what was actually seen. The capsule contract
// requires the opposite — a named refusal over a silent approximation — and the
// integer-to-float constants below are part of `kCanonicalPcmDecoderVersion`,
// so this module owns them rather than inheriting whatever a vendored decoder
// does today.

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

constexpr std::size_t kRiffHeaderBytes = 12;
constexpr std::size_t kChunkHeaderBytes = 8;
constexpr std::size_t kMinFmtChunkBytes = 16;
constexpr std::size_t kExtensibleFmtChunkBytes = 40;

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
        if (bytes.size() < kRiffHeaderBytes)
            return "riff:truncated";
        return "riff:" + fourcc(bytes, 8);
    }
    if (has_id(bytes, 0, "RF64"))
        return "rf64";
    if (has_id(bytes, 0, "BW64"))
        return "bw64";
    if (has_id(bytes, 0, "FORM"))
        return "aiff";
    if (has_id(bytes, 0, "fLaC"))
        return "flac";
    if (has_id(bytes, 0, "OggS"))
        return "ogg";
    if (has_id(bytes, 0, "caff"))
        return "caf";
    if (bytes.size() >= 8 && has_id(bytes, 4, "ftyp"))
        return "iso-bmff";
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

struct RiffChunks {
    ChunkSpan format;
    ChunkSpan data;
};

/// Walk the RIFF chunk list, taking the first `fmt ` and the first `data`.
/// Every chunk must lie wholly inside the buffer: a declared size that runs
/// past the end is a truncated file, and guessing at the missing tail would be
/// the approximation this decoder refuses to make.
runtime::Result<RiffChunks, CapsuleError> find_chunks(std::span<const std::uint8_t> bytes) {
    // A RIFF size field that disagrees with the file length is common (streamed
    // writers, appended metadata). Clamping to the real length keeps those
    // readable while still bounding every chunk by bytes actually present.
    const auto declared = static_cast<std::uint64_t>(read_u32(bytes, 4)) + 8;
    const auto end = static_cast<std::size_t>(
        declared < bytes.size() ? declared : static_cast<std::uint64_t>(bytes.size()));
    if (end < kRiffHeaderBytes) {
        return runtime::Err(unsupported("wav.riff.size", "at_least_12",
                                        std::to_string(declared)));
    }

    RiffChunks chunks;
    std::size_t offset = kRiffHeaderBytes;
    while (end - offset >= kChunkHeaderBytes) {
        const auto size = static_cast<std::size_t>(read_u32(bytes, offset + 4));
        const auto payload = offset + kChunkHeaderBytes;
        if (size > end - payload) {
            return runtime::Err(unsupported("wav.chunk", "chunk_within_file",
                                            fourcc(bytes, offset) + ":size=" +
                                                std::to_string(size)));
        }
        if (!chunks.format.found && has_id(bytes, offset, "fmt ")) {
            chunks.format = {payload, size, true};
        } else if (!chunks.data.found && has_id(bytes, offset, "data")) {
            chunks.data = {payload, size, true};
        }
        const auto padding = size & 1u;
        const auto advance = kChunkHeaderBytes + size + padding;
        if (advance > end - offset)
            break;  // A pad byte elided at end of file ends the walk cleanly.
        offset += advance;
    }

    if (!chunks.format.found)
        return runtime::Err(unsupported("wav.fmt", "present", "absent"));
    if (!chunks.data.found)
        return runtime::Err(unsupported("wav.data", "present", "absent"));
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

float decode_sample(std::span<const std::uint8_t> data, std::size_t offset,
                    SourceFormat format) noexcept {
    switch (format) {
        case SourceFormat::pcm16: {
            const auto raw = read_u16(data, offset);
            auto value = static_cast<std::int32_t>(raw);
            if (value & 0x8000)
                value -= 0x10000;
            return static_cast<float>(value) * kPcm16Scale;
        }
        case SourceFormat::pcm24: {
            auto value = static_cast<std::int32_t>(static_cast<std::uint32_t>(data[offset]) |
                                                   (static_cast<std::uint32_t>(data[offset + 1])
                                                    << 8) |
                                                   (static_cast<std::uint32_t>(data[offset + 2])
                                                    << 16));
            if (value & 0x800000)
                value -= 0x1000000;
            return static_cast<float>(value) * kPcm24Scale;
        }
        case SourceFormat::pcm32: {
            const auto value = static_cast<std::int32_t>(read_u32(data, offset));
            return static_cast<float>(static_cast<double>(value) * kPcm32Scale);
        }
        case SourceFormat::float32: return std::bit_cast<float>(read_u32(data, offset));
    }
    return 0.0f;
}

/// Append `value` little-endian, widest-field-last, so the trailer's layout is
/// fixed on every host regardless of the host's own byte order.
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

}  // namespace

std::vector<std::uint8_t> to_canonical_bytes(const CanonicalPcm& pcm) {
    // Interleaved little-endian binary32, no header: the channel count, rate,
    // and frame count live in the manifest, where the closure covers them.
    // The bytes are assembled from the IEEE bit pattern rather than memcpy'd
    // from the float array, so the result is identical on a big-endian host
    // even though none is currently supported.
    std::vector<std::uint8_t> bytes;
    bytes.reserve(pcm.samples.size() * sizeof(float));
    for (const auto sample : pcm.samples)
        append_le(bytes, std::bit_cast<std::uint32_t>(sample));
    return bytes;
}

runtime::Result<CanonicalPcm, CapsuleError>
from_canonical_bytes(std::span<const std::uint8_t> bytes, const CanonicalPcmMedia& media) {
    // `CanonicalPcm` carries no decoder version of its own, so bytes produced
    // by a future decoder would be silently re-digested as version 1 — a
    // different rendition wearing this version's identity. Refuse instead.
    if (media.decoder_version != kCanonicalPcmDecoderVersion) {
        return runtime::Err(unsupported("canonical_pcm.decoder_version",
                                        std::to_string(kCanonicalPcmDecoderVersion),
                                        std::to_string(media.decoder_version)));
    }
    if (media.channels == 0 || media.channels > kMaxCanonicalChannels) {
        return runtime::Err(unsupported("canonical_pcm.channels",
                                        "1.." + std::to_string(kMaxCanonicalChannels),
                                        std::to_string(media.channels)));
    }
    if (media.frame_count >
        std::numeric_limits<std::uint64_t>::max() / (media.channels * sizeof(float))) {
        return runtime::Err(unsupported("canonical_pcm.frame_count", "representable_byte_count",
                                        std::to_string(media.frame_count)));
    }

    const auto expected = media.frame_count * media.channels * sizeof(float);
    if (expected != bytes.size()) {
        return runtime::Err(unsupported("canonical_pcm.bytes", std::to_string(expected) + "_bytes",
                                        std::to_string(bytes.size()) + "_bytes"));
    }

    CanonicalPcm pcm;
    pcm.channels = media.channels;
    pcm.sample_rate = media.sample_rate;
    pcm.frame_count = media.frame_count;
    pcm.samples.resize(static_cast<std::size_t>(media.frame_count) * media.channels);
    for (std::size_t i = 0; i < pcm.samples.size(); ++i)
        pcm.samples[i] = std::bit_cast<float>(read_u32(bytes, i * sizeof(float)));

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
    // The declared frame count is what the manifest publishes and what the
    // digest covers, so it has to agree with the buffer exactly — a buffer one
    // frame short would play a rendition nobody hashed.
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
    // Digest input is the canonical sample bytes followed by a fixed 20-byte
    // little-endian media trailer. Every audio member's identity depends on
    // this layout, so it is frozen:
    //
    //   [0 .. n)      interleaved little-endian binary32 samples (n = 4 * |samples|)
    //   [n+0  .. n+4)   uint32 channels
    //   [n+4  .. n+8)   uint32 sample_rate
    //   [n+8  .. n+16)  uint64 frame_count
    //   [n+16 .. n+20)  uint32 decoder_version
    //
    // The trailer is fixed width and appended last, so the split between
    // samples and metadata is unambiguous without a length prefix. Including
    // the rate is the point: two renditions of the same samples declared at
    // 44100 and 48000 are different audio and must not share an identity.
    // Including the decoder version means a future change to the integer
    // scaling above re-identifies everything it touched, rather than silently
    // reusing an identity minted by different arithmetic.
    //
    // The caller validates first; the digest is defined over the buffer that
    // is present together with the fields as declared.
    auto bytes = to_canonical_bytes(pcm);
    bytes.reserve(bytes.size() + 20);
    append_le(bytes, pcm.channels);
    append_le(bytes, pcm.sample_rate);
    append_le(bytes, pcm.frame_count);
    append_le(bytes, kCanonicalPcmDecoderVersion);
    // Bare lowercase hex, no `sha256:` prefix: the prefix, where a field wants
    // one, is the manifest's spelling and not part of the digest.
    return runtime::sha256_hex(bytes.data(), bytes.size());
}

runtime::Result<CanonicalPcm, CapsuleError> decode_to_canonical(std::span<const std::uint8_t> wav) {
    if (wav.size() < kRiffHeaderBytes || !has_id(wav, 0, "RIFF") || !has_id(wav, 8, "WAVE")) {
        return runtime::Err(unsupported("wav", "riff_wave", describe_container(wav)));
    }

    auto chunks = find_chunks(wav);
    if (chunks.is_err())
        return runtime::Err(std::move(chunks).error());

    auto format = parse_format(wav, chunks->format);
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
        // the audio in the manifest and the host resamples at play time. A rate
        // outside the admissible range is therefore a refusal, not a conversion.
        return runtime::Err(unsupported("wav.fmt.sample_rate",
                                        std::to_string(kMinCanonicalSampleRate) + ".." +
                                            std::to_string(kMaxCanonicalSampleRate),
                                        std::to_string(format->sample_rate)));
    }

    const auto sample_bytes = bytes_per_sample(format->format);
    const auto frame_bytes = sample_bytes * format->channels;
    if (format->block_align != 0 && format->block_align != frame_bytes) {
        return runtime::Err(unsupported("wav.fmt.block_align", std::to_string(frame_bytes),
                                        std::to_string(format->block_align)));
    }
    const auto data_size = chunks->data.size;
    if (data_size % frame_bytes != 0) {
        // A trailing partial frame would have to be dropped or padded. Either
        // is a change to the audio the manifest will hash, so name it instead.
        return runtime::Err(unsupported("wav.data", "multiple_of_" + std::to_string(frame_bytes),
                                        std::to_string(data_size) + "_bytes"));
    }

    const auto frames = static_cast<std::uint64_t>(data_size / frame_bytes);
    const auto samples = frames * format->channels;
    const auto canonical_bytes = samples * sizeof(float);
    // A canonical rendition too large to be one archive member cannot travel in
    // a capsule at all, so refuse before allocating rather than after.
    if (canonical_bytes > kCapsuleLimitsV1.max_member_expanded_bytes) {
        return runtime::Err(make_error(CapsuleStatus::archive_budget_exceeded,
                                       "canonical_pcm.bytes",
                                       std::to_string(kCapsuleLimitsV1.max_member_expanded_bytes),
                                       std::to_string(canonical_bytes)));
    }

    const auto data = wav.subspan(chunks->data.offset, data_size);
    CanonicalPcm pcm;
    pcm.channels = format->channels;
    pcm.sample_rate = format->sample_rate;
    pcm.frame_count = frames;
    pcm.samples.resize(static_cast<std::size_t>(samples));
    for (std::size_t i = 0; i < pcm.samples.size(); ++i)
        pcm.samples[i] = decode_sample(data, i * sample_bytes, format->format);

    // A float32 source can carry NaN or infinity straight through; the
    // canonical form cannot.
    auto valid = validate_canonical(pcm);
    if (valid.is_err())
        return runtime::Err(std::move(valid).error());
    return runtime::Ok(std::move(pcm));
}

}  // namespace pulp::authoring_capsule
