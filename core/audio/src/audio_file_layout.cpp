// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

#include <pulp/audio/audio_file_layout.hpp>

#include <pulp/audio/wav_metadata.hpp>

#include "aiff_parser.hpp"
#include "audio_file_layout_detail.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <istream>
#include <limits>
#include <streambuf>
#include <string_view>

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pulp::audio {

// ── Byte sources ────────────────────────────────────────────────────────────

std::size_t SpanByteSource::read_at(std::uint64_t offset, std::span<std::uint8_t> out) noexcept {
    if (offset >= bytes_.size())
        return 0u;
    const auto available = static_cast<std::size_t>(bytes_.size() - offset);
    const auto count = std::min(out.size(), available);
    std::memcpy(out.data(), bytes_.data() + offset, count);
    return count;
}

FileByteSource::FileByteSource(const std::filesystem::path& path) {
#if defined(_WIN32)
    handle_ = _wopen(path.c_str(), _O_RDONLY | _O_BINARY);
    if (handle_ >= 0) {
        struct _stat64 st{};
        if (_fstat64(handle_, &st) == 0)
            size_ = static_cast<std::uint64_t>(st.st_size);
    }
#else
    handle_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (handle_ >= 0) {
        struct stat st{};
        if (::fstat(handle_, &st) == 0)
            size_ = static_cast<std::uint64_t>(st.st_size);
    }
#endif
}

FileByteSource::~FileByteSource() {
    if (handle_ >= 0) {
#if defined(_WIN32)
        _close(handle_);
#else
        ::close(handle_);
#endif
    }
}

std::size_t FileByteSource::read_at(std::uint64_t offset, std::span<std::uint8_t> out) noexcept {
    if (handle_ < 0 || offset >= size_)
        return 0u;
    std::size_t copied = 0u;
    while (copied < out.size()) {
#if defined(_WIN32)
        if (_lseeki64(handle_, static_cast<long long>(offset + copied), SEEK_SET) < 0)
            break;
        const auto got = _read(handle_, out.data() + copied,
                               static_cast<unsigned>(std::min<std::size_t>(out.size() - copied, 1u << 30)));
#else
        const auto got = ::pread(handle_, out.data() + copied, out.size() - copied,
                                 static_cast<off_t>(offset + copied));
#endif
        if (got <= 0)
            break;
        copied += static_cast<std::size_t>(got);
    }
    return copied;
}

std::size_t CountingByteSource::read_at(std::uint64_t offset, std::span<std::uint8_t> out) noexcept {
    const auto got = inner_.read_at(offset, out);
    bytes_read_ += got;
    ++read_calls_;
    high_water_ = std::max(high_water_, offset + got);
    return got;
}

// ── Names ───────────────────────────────────────────────────────────────────

const char* audio_container_name(AudioContainer container) noexcept {
    switch (container) {
        case AudioContainer::wav: return "wav";
        case AudioContainer::aiff: return "aiff";
        case AudioContainer::aifc: return "aifc";
        case AudioContainer::flac: return "flac";
        case AudioContainer::ogg: return "ogg";
        case AudioContainer::mp3: return "mp3";
        case AudioContainer::mp4: return "mp4";
        case AudioContainer::raw: return "raw";
        case AudioContainer::unknown: break;
    }
    return "unknown";
}

const char* layout_region_kind_name(LayoutRegionKind kind) noexcept {
    switch (kind) {
        case LayoutRegionKind::envelope: return "envelope";
        case LayoutRegionKind::fmt: return "fmt";
        case LayoutRegionKind::data: return "data";
        case LayoutRegionKind::metadata: return "metadata";
        case LayoutRegionKind::padding: return "padding";
        case LayoutRegionKind::unknown: break;
    }
    return "unknown";
}

const char* wav_profile_name(WavProfile profile) noexcept {
    switch (profile) {
        case WavProfile::pcm44: return "pcm44";
        case WavProfile::bwf: return "bwf";
        case WavProfile::floating: return "float";
        case WavProfile::extensible: return "extensible";
        case WavProfile::rf64: return "rf64";
        case WavProfile::w64: return "w64";
    }
    return "pcm44";
}

const char* facts_confidence_name(FactsConfidence confidence) noexcept {
    return confidence == FactsConfidence::parsed ? "parsed" : "container_only";
}

const char* layout_warning_kind_name(LayoutWarningKind kind) noexcept {
    switch (kind) {
        case LayoutWarningKind::envelope_size_mismatch: return "envelope_size_mismatch";
        case LayoutWarningKind::missing_pad_byte: return "missing_pad_byte";
        case LayoutWarningKind::duplicate_fmt: return "duplicate_fmt";
        case LayoutWarningKind::fmt_after_data: return "fmt_after_data";
        case LayoutWarningKind::truncated_data: return "truncated_data";
        case LayoutWarningKind::sentinel_size_without_ds64: return "sentinel_size_without_ds64";
        case LayoutWarningKind::chunk_overlap: return "chunk_overlap";
        case LayoutWarningKind::chunk_past_eof: return "chunk_past_eof";
        case LayoutWarningKind::extensible_mask_channel_mismatch:
            return "extensible_mask_channel_mismatch";
        case LayoutWarningKind::valid_bits_above_container: return "valid_bits_above_container";
    }
    return "unknown";
}

const char* raw_pcm_format_name(RawPcmSpec::Format format) noexcept {
    switch (format) {
        case RawPcmSpec::Format::s8: return "s8";
        case RawPcmSpec::Format::u8: return "u8";
        case RawPcmSpec::Format::s16: return "s16";
        case RawPcmSpec::Format::s24: return "s24";
        case RawPcmSpec::Format::s32: return "s32";
        case RawPcmSpec::Format::f32: return "f32";
        case RawPcmSpec::Format::f64: return "f64";
    }
    return "s16";
}

std::optional<RawPcmSpec::Format> parse_raw_pcm_format(std::string_view name) noexcept {
    using F = RawPcmSpec::Format;
    constexpr std::array<std::pair<std::string_view, F>, 7> kNames{{
        {"s8", F::s8}, {"u8", F::u8}, {"s16", F::s16}, {"s24", F::s24},
        {"s32", F::s32}, {"f32", F::f32}, {"f64", F::f64},
    }};
    for (const auto& [text, value] : kNames) {
        if (text == name)
            return value;
    }
    return std::nullopt;
}

std::uint32_t raw_pcm_bytes_per_sample(RawPcmSpec::Format format) noexcept {
    switch (format) {
        case RawPcmSpec::Format::s8:
        case RawPcmSpec::Format::u8: return 1u;
        case RawPcmSpec::Format::s16: return 2u;
        case RawPcmSpec::Format::s24: return 3u;
        case RawPcmSpec::Format::s32:
        case RawPcmSpec::Format::f32: return 4u;
        case RawPcmSpec::Format::f64: return 8u;
    }
    return 2u;
}

namespace layout_detail {

// ── Reader ──────────────────────────────────────────────────────────────────

Reader::Reader(LayoutByteSource& source) noexcept : source_(source), size_(source.size()) {}

std::vector<std::uint8_t> Reader::read(std::uint64_t offset, std::uint64_t length) {
    std::vector<std::uint8_t> out;
    if (offset >= size_ || length == 0u)
        return out;
    const auto available = size_ - offset;
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(length, available));
    out.resize(count);
    const auto got = source_.read_at(offset, out);
    bytes_read_ += got;
    out.resize(got);
    return out;
}

bool Reader::read_exact(std::uint64_t offset, std::span<std::uint8_t> out) {
    if (offset > size_ || out.size() > size_ - offset)
        return false;
    const auto got = source_.read_at(offset, out);
    bytes_read_ += got;
    return got == out.size();
}

std::uint16_t read_u16le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8u));
}
std::uint32_t read_u32le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8u) |
           (static_cast<std::uint32_t>(p[2]) << 16u) | (static_cast<std::uint32_t>(p[3]) << 24u);
}
std::uint64_t read_u64le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint64_t>(read_u32le(p)) |
           (static_cast<std::uint64_t>(read_u32le(p + 4)) << 32u);
}
std::uint16_t read_u16be(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((p[0] << 8u) | p[1]);
}
std::uint32_t read_u24be(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 16u) | (static_cast<std::uint32_t>(p[1]) << 8u) |
           static_cast<std::uint32_t>(p[2]);
}
std::uint32_t read_u32be(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24u) | (static_cast<std::uint32_t>(p[1]) << 16u) |
           (static_cast<std::uint32_t>(p[2]) << 8u) | static_cast<std::uint32_t>(p[3]);
}
std::uint64_t read_u64be(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint64_t>(read_u32be(p)) << 32u) | read_u32be(p + 4);
}

std::string fourcc(const std::uint8_t* p) {
    return std::string(reinterpret_cast<const char*>(p), 4u);
}

bool printable_id(std::string_view id) noexcept {
    return std::all_of(id.begin(), id.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u >= 0x20u && u < 0x7fu;
    });
}

std::string hex_bytes(const std::uint8_t* p, std::size_t count) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(count * 2u);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(kHex[p[i] >> 4u]);
        out.push_back(kHex[p[i] & 0xfu]);
    }
    return out;
}

std::string trimmed_text(const std::uint8_t* p, std::size_t count) {
    std::string out(reinterpret_cast<const char*>(p), count);
    while (!out.empty() && (out.back() == '\0' || out.back() == ' '))
        out.pop_back();
    return out;
}

std::string utf16_to_utf8(const std::uint8_t* p, std::size_t count, bool big_endian) {
    std::string out;
    std::size_t i = 0u;
    if (count >= 2u) {
        if (p[0] == 0xff && p[1] == 0xfe) { big_endian = false; i = 2u; }
        else if (p[0] == 0xfe && p[1] == 0xff) { big_endian = true; i = 2u; }
    }
    auto unit = [&](std::size_t at) -> std::uint32_t {
        return big_endian ? read_u16be(p + at) : read_u16le(p + at);
    };
    for (; i + 1u < count; i += 2u) {
        std::uint32_t code = unit(i);
        if (code == 0u)
            break;
        if (code >= 0xd800u && code <= 0xdbffu && i + 3u < count) {
            const std::uint32_t low = unit(i + 2u);
            if (low >= 0xdc00u && low <= 0xdfffu) {
                code = 0x10000u + ((code - 0xd800u) << 10u) + (low - 0xdc00u);
                i += 2u;
            }
        }
        if (code < 0x80u) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800u) {
            out.push_back(static_cast<char>(0xc0u | (code >> 6u)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3fu)));
        } else if (code < 0x10000u) {
            out.push_back(static_cast<char>(0xe0u | (code >> 12u)));
            out.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3fu)));
        } else {
            out.push_back(static_cast<char>(0xf0u | (code >> 18u)));
            out.push_back(static_cast<char>(0x80u | ((code >> 12u) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3fu)));
        }
    }
    return out;
}

// ── Report building ─────────────────────────────────────────────────────────

LayoutRegion& Builder::add_region(std::uint64_t offset, std::uint64_t header_size, std::uint64_t declared,
                                  std::string id, LayoutRegionKind kind) {
    if (report.regions.size() >= kMaxRegions) {
        report.regions_truncated = true;
        return overflow_;
    }
    LayoutRegion region;
    region.offset = offset;
    region.declared_size = declared;
    const auto payload_offset = offset + header_size;
    const auto available = payload_offset <= reader.size() ? reader.size() - payload_offset : 0u;
    region.actual_size = std::min(declared, available);
    region.id = std::move(id);
    region.kind = kind;
    report.regions.push_back(std::move(region));
    return report.regions.back();
}

void Builder::warn(LayoutWarningKind kind, std::uint64_t offset, std::string detail) {
    report.warnings.push_back(LayoutWarning{kind, offset, std::move(detail)});
}

void Builder::present(const std::string& id, std::uint64_t offset, std::uint64_t size) {
    report.metadata_present.push_back(LayoutMetadataPresence{id, offset, size});
}

void Builder::text(const std::string& region_id, std::string key, std::string value) {
    if (!options.include_metadata_text || value.empty())
        return;
    report.metadata_text.push_back(LayoutMetadataText{region_id, std::move(key), std::move(value)});
}

void Builder::set_decode_limits(std::uint64_t frames, std::uint32_t channels) {
    LayoutDecodeLimitsFact fact;
    fact.limits = options.decode_limits;
    std::uint64_t bytes = 0u;
    const bool fits = channels != 0u &&
                      frames <= std::numeric_limits<std::uint64_t>::max() / channels &&
                      frames * channels <= std::numeric_limits<std::uint64_t>::max() / sizeof(float);
    if (fits)
        bytes = frames * channels * sizeof(float);
    fact.output_bytes = bytes;
    if (frames > fact.limits.max_frames)
        fact.exceeded.emplace_back("max_frames");
    if (channels > fact.limits.max_channels)
        fact.exceeded.emplace_back("max_channels");
    if (!fits || bytes > fact.limits.max_output_bytes)
        fact.exceeded.emplace_back("max_output_bytes");
    fact.exceeds = !fact.exceeded.empty();
    report.decode_limits = fact;
}

// ── Container detection ─────────────────────────────────────────────────────

namespace {

constexpr std::array<std::uint8_t, 16> kW64RiffGuid{0x72, 0x69, 0x66, 0x66, 0x2E, 0x91, 0xCF, 0x11,
                                                    0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00};

bool starts_with(std::span<const std::uint8_t> bytes, std::string_view magic, std::size_t at = 0u) {
    if (bytes.size() < at + magic.size())
        return false;
    return std::memcmp(bytes.data() + at, magic.data(), magic.size()) == 0;
}

bool mp3_frame_header_valid(const std::uint8_t* h) noexcept {
    if (h[0] != 0xffu || (h[1] & 0xe0u) != 0xe0u)
        return false;
    const auto version = (h[1] >> 3u) & 0x3u;   // 1 is reserved
    const auto layer = (h[1] >> 1u) & 0x3u;     // 0 is reserved
    const auto bitrate = h[2] >> 4u;            // 0 free, 15 bad
    const auto rate = (h[2] >> 2u) & 0x3u;      // 3 reserved
    return version != 1u && layer != 0u && bitrate != 0u && bitrate != 15u && rate != 3u;
}

AudioContainer sniff(Reader& reader, std::string& detail) {
    const auto head = reader.read(0u, 16u);
    if (head.size() >= 12u) {
        if ((starts_with(head, "RIFF") || starts_with(head, "RF64") || starts_with(head, "BW64")) &&
            starts_with(head, "WAVE", 8u)) {
            detail = fourcc(head.data());
            return AudioContainer::wav;
        }
        if (starts_with(head, "FORM")) {
            if (starts_with(head, "AIFF", 8u)) { detail = "FORM"; return AudioContainer::aiff; }
            if (starts_with(head, "AIFC", 8u)) { detail = "FORM"; return AudioContainer::aifc; }
        }
        if (starts_with(head, "ftyp", 4u)) {
            detail = "ftyp:" + fourcc(head.data() + 8u);
            return AudioContainer::mp4;
        }
    }
    if (head.size() >= 16u && std::memcmp(head.data(), kW64RiffGuid.data(), 16u) == 0) {
        detail = "W64";
        return AudioContainer::wav;
    }
    if (starts_with(head, "fLaC")) { detail = "fLaC"; return AudioContainer::flac; }
    if (starts_with(head, "OggS")) { detail = "OggS"; return AudioContainer::ogg; }
    if (starts_with(head, "ID3")) { detail = "ID3"; return AudioContainer::mp3; }
    if (starts_with(head, "TAG")) { detail = "ID3v1"; return AudioContainer::mp3; }
    if (head.size() >= 4u && mp3_frame_header_valid(head.data())) {
        detail = "frame";
        return AudioContainer::mp3;
    }
    if (starts_with(head, "APETAGEX")) { detail = "APE"; return AudioContainer::mp3; }
    return AudioContainer::unknown;
}

} // namespace

// ── Raw PCM ─────────────────────────────────────────────────────────────────

void inspect_raw(Builder& b, const RawPcmSpec& spec) {
    auto& report = b.report;
    report.container = AudioContainer::raw;
    report.container_detail = raw_pcm_format_name(spec.format);
    report.facts_confidence = FactsConfidence::parsed;
    if (spec.channels == 0u) { report.error = "raw PCM needs at least one channel"; return; }
    if (spec.sample_rate == 0u) { report.error = "raw PCM needs a positive sample rate"; return; }
    if (spec.offset > b.reader.size()) {
        report.error = "raw PCM offset " + std::to_string(spec.offset) +
                       " is past the end of the file (" + std::to_string(b.reader.size()) + " bytes)";
        return;
    }
    const auto bytes_per_sample = raw_pcm_bytes_per_sample(spec.format);
    const std::uint64_t bytes_per_frame = static_cast<std::uint64_t>(bytes_per_sample) * spec.channels;
    const auto payload = b.reader.size() - spec.offset;
    const auto frames = payload / bytes_per_frame;
    const auto tail = payload % bytes_per_frame;

    if (spec.offset > 0u) {
        auto& skipped = b.add_region(0u, 0u, spec.offset, "header", LayoutRegionKind::unknown);
        skipped.note = "bytes before --offset, not interpreted";
    }
    auto& pcm = b.add_region(spec.offset, 0u, payload, "pcm", LayoutRegionKind::data);
    pcm.fields.push_back({"format", std::string(raw_pcm_format_name(spec.format))});
    pcm.fields.push_back({"endian", std::string(spec.endian == RawPcmSpec::Endian::big ? "big" : "little")});
    pcm.fields.push_back({"channels", static_cast<std::uint64_t>(spec.channels)});
    pcm.fields.push_back({"sample_rate", static_cast<std::uint64_t>(spec.sample_rate)});
    pcm.fields.push_back({"bytes_per_frame", bytes_per_frame});
    pcm.fields.push_back({"frames", frames});

    LayoutStreamFacts facts;
    facts.sample_rate = spec.sample_rate;
    facts.channels = spec.channels;
    facts.bits_per_sample = bytes_per_sample * 8u;
    facts.valid_bits = facts.bits_per_sample;
    facts.frames = frames;
    facts.duration_seconds = static_cast<double>(frames) / spec.sample_rate;
    facts.encoding = (spec.format == RawPcmSpec::Format::f32 || spec.format == RawPcmSpec::Format::f64)
                         ? "float" : "pcm";
    facts.bytes_per_frame = static_cast<std::uint32_t>(bytes_per_frame);
    report.stream = facts;
    report.truncated_tail_bytes = tail;
    b.set_decode_limits(frames, spec.channels);
}

} // namespace layout_detail

// ── Entry points ────────────────────────────────────────────────────────────

AudioFileLayoutReport inspect_audio_file_layout(LayoutByteSource& source,
                                                const LayoutInspectOptions& options) {
    layout_detail::Builder b(source, options);
    b.report.file_size = source.size();
    if (options.raw) {
        layout_detail::inspect_raw(b, *options.raw);
    } else {
        std::string detail;
        const auto container = layout_detail::sniff(b.reader, detail);
        b.report.container = container;
        b.report.container_detail = detail;
        switch (container) {
            case AudioContainer::wav: layout_detail::inspect_wav_layout(b); break;
            case AudioContainer::aiff:
            case AudioContainer::aifc: layout_detail::inspect_aiff_layout(b); break;
            case AudioContainer::flac: layout_detail::inspect_flac_layout(b); break;
            case AudioContainer::ogg: layout_detail::inspect_ogg_layout(b); break;
            case AudioContainer::mp3: layout_detail::inspect_mp3_layout(b); break;
            case AudioContainer::mp4: layout_detail::inspect_mp4_layout(b); break;
            case AudioContainer::raw:
            case AudioContainer::unknown:
                b.report.facts_confidence = FactsConfidence::container_only;
                if (source.size() == 0u)
                    b.report.error = "empty input";
                break;
        }
    }
    b.report.bytes_read = b.reader.bytes_read();
    return std::move(b.report);
}

AudioFileLayoutReport inspect_audio_file_layout(std::span<const std::uint8_t> bytes,
                                                const LayoutInspectOptions& options) {
    SpanByteSource source(bytes);
    return inspect_audio_file_layout(source, options);
}

AudioFileLayoutReport inspect_audio_file_layout(const std::filesystem::path& path,
                                                const LayoutInspectOptions& options) {
    FileByteSource source(path);
    if (!source.is_open()) {
        AudioFileLayoutReport report;
        report.error = "cannot open file";
        return report;
    }
    return inspect_audio_file_layout(source, options);
}

} // namespace pulp::audio
