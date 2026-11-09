// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

// RIFF/WAVE, RF64/BW64, and Sony Wave64 layout walk.
//
// This walk is presentation: it records every region it can locate and the
// fields the specification defines for each. The verdict, the stream facts
// and the warnings that bear on decoding come from the decoder's own probe
// (walk_wave_layout, interpret_wav_format, decide_wave), so the report and
// inspect_wav cannot disagree.

#include "audio_file_layout_detail.hpp"
#include "wav_frame_reader.hpp"

#include <pulp/audio/wav_metadata.hpp>
#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <optional>
#include <unordered_map>

namespace pulp::audio::layout_detail {
namespace {

constexpr std::uint32_t kSizeSentinel = 0xFFFFFFFFu;
constexpr std::array<std::uint8_t, 12> kW64ChunkSuffix{0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1,
                                                       0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A};

struct Envelope {
    bool w64 = false;
    bool rf64 = false;
    bool declared_sentinel = false; // RIFF size field is 0xFFFFFFFF
    IffEnvelope iff;
};

struct Ds64 {
    std::uint64_t riff_size = 0u;
    std::uint64_t data_size = 0u;
    std::uint64_t sample_count = 0u;
    std::unordered_map<std::string, std::uint64_t> table;
};

struct FormatFields {
    std::vector<std::uint8_t> payload;
    std::uint16_t format_tag = 0u;
    std::uint16_t channels = 0u;
    std::uint32_t sample_rate = 0u;
    std::uint16_t bits_per_sample = 0u;
    std::uint16_t block_align = 0u;
    std::uint16_t valid_bits = 0u;
    std::uint32_t channel_mask = 0u;
    bool extensible = false;
};

struct Walk {
    std::optional<FormatFields> fmt;
    std::uint64_t fmt_count = 0u;
    bool data_seen = false;
    std::uint64_t data_size = 0u; // first data chunk's declared payload, for Wave64 facts
    bool has_bext = false;
};

LayoutRegionKind classify(std::string_view id) {
    static constexpr std::array<std::string_view, 24> kMetadata{
        "bext", "iXML", "axml", "acid", "LIST", "cue ", "smpl", "inst",
        "plst", "labl", "note", "ltxt", "PEAK", "ID3 ", "id3 ", "umid",
        "levl", "link", "chna", "minf", "elm1", "CART", "_PMX", "DISP"};
    if (id == "fmt " || id == "fact")
        return LayoutRegionKind::fmt;
    if (id == "data")
        return LayoutRegionKind::data;
    if (id == "ds64")
        return LayoutRegionKind::envelope;
    if (id == "JUNK" || id == "junk" || id == "PAD " || id == "FLLR" || id == "pad ")
        return LayoutRegionKind::padding;
    if (std::find(kMetadata.begin(), kMetadata.end(), id) != kMetadata.end())
        return LayoutRegionKind::metadata;
    return LayoutRegionKind::unknown;
}

const char* encoding_name(std::uint16_t translated_tag) {
    switch (translated_tag) {
    case 0x0001:
        return "pcm";
    case 0x0002:
        return "adpcm";
    case 0x0003:
        return "float";
    case 0x0006:
        return "alaw";
    case 0x0007:
        return "mulaw";
    case 0x0011:
        return "adpcm";
    default:
        return "unknown";
    }
}

std::optional<Envelope> read_envelope(Builder& b) {
    Envelope env;
    const auto head = b.reader.read(0u, 40u);
    if (head.size() < 12u)
        return std::nullopt;
    if (b.report.container_detail == "W64") {
        if (head.size() < 40u)
            return std::nullopt;
        env.w64 = true;
        env.iff.header_size = 40u;
        env.iff.chunk_header_size = 24u;
        env.iff.alignment = 8u;
        env.iff.pad_bytes = false;
        // Wave64 sizes include their own headers, so the riff size is the
        // whole file.
        env.iff.declared_end = read_u64le(head.data() + 16u);
        auto& region =
            b.add_region(0u, 0u, env.iff.declared_end, "riff", LayoutRegionKind::envelope);
        region.fields.push_back({"form", std::string("wave")});
        region.fields.push_back({"riff_size", env.iff.declared_end});
        return env;
    }
    const auto id = fourcc(head.data());
    env.rf64 = id == "RF64" || id == "BW64";
    const auto size = read_u32le(head.data() + 4u);
    env.declared_sentinel = size == kSizeSentinel;
    env.iff.declared_end = static_cast<std::uint64_t>(size) + 8u;
    auto& region = b.add_region(0u, 8u, size, id, LayoutRegionKind::envelope);
    region.fields.push_back({"form", fourcc(head.data() + 8u)});
    region.fields.push_back({"riff_size", static_cast<std::uint64_t>(size)});
    return env;
}

// Chunk header at `offset`: id and payload size. W64 chunks carry GUID ids
// and sizes that include the 24-byte header; both are normalised here.
std::optional<IffChunk> read_chunk_header(Builder& b, const Envelope& env, std::uint64_t offset) {
    std::array<std::uint8_t, 24> raw{};
    if (!b.reader.read_exact(offset, std::span(raw.data(), env.iff.chunk_header_size)))
        return std::nullopt;
    IffChunk header;
    if (env.w64) {
        if (std::memcmp(raw.data() + 4u, kW64ChunkSuffix.data(), kW64ChunkSuffix.size()) == 0) {
            header.id = fourcc(raw.data());
            header.printable = raw_id_printable(raw.data(), 4u);
        } else {
            header.id = runtime::hex_encode(raw.data(), 16u);
        }
        const auto total = read_u64le(raw.data() + 16u);
        header.declared = total >= 24u ? total - 24u : 0u;
        if (total < 24u)
            header.printable = false;
    } else {
        header.id = fourcc(raw.data());
        header.printable = raw_id_printable(raw.data(), 4u);
        const auto size = read_u32le(raw.data() + 4u);
        header.declared = size;
        header.sentinel = size == kSizeSentinel;
    }
    return header;
}

void parse_fmt(Builder& b, LayoutRegion& region, Walk& walk, std::uint64_t payload_offset,
               std::uint64_t declared) {
    ++walk.fmt_count;
    auto payload = b.reader.read_capped(payload_offset, declared, 1024u);
    FormatFields fields;
    fields.payload = payload;
    if (payload.size() < 16u) {
        region.note = "fmt payload shorter than the 16-byte PCM header";
        if (walk.fmt_count == 1u)
            walk.fmt = std::move(fields);
        return;
    }
    fields.format_tag = read_u16le(payload.data());
    fields.channels = read_u16le(payload.data() + 2u);
    fields.sample_rate = read_u32le(payload.data() + 4u);
    const auto byte_rate = read_u32le(payload.data() + 8u);
    fields.block_align = read_u16le(payload.data() + 12u);
    fields.bits_per_sample = read_u16le(payload.data() + 14u);
    region.fields.push_back({"format_tag", static_cast<std::uint64_t>(fields.format_tag)});
    region.fields.push_back({"channels", static_cast<std::uint64_t>(fields.channels)});
    region.fields.push_back({"sample_rate", static_cast<std::uint64_t>(fields.sample_rate)});
    region.fields.push_back({"byte_rate", static_cast<std::uint64_t>(byte_rate)});
    region.fields.push_back({"block_align", static_cast<std::uint64_t>(fields.block_align)});
    region.fields.push_back(
        {"bits_per_sample", static_cast<std::uint64_t>(fields.bits_per_sample)});
    if (payload.size() >= 18u) {
        region.fields.push_back(
            {"cb_size", static_cast<std::uint64_t>(read_u16le(payload.data() + 16u))});
    }
    fields.valid_bits = fields.bits_per_sample;
    if (fields.format_tag == 0xFFFEu && payload.size() >= 40u) {
        fields.extensible = true;
        fields.valid_bits = read_u16le(payload.data() + 18u);
        fields.channel_mask = read_u32le(payload.data() + 20u);
        region.fields.push_back({"valid_bits", static_cast<std::uint64_t>(fields.valid_bits)});
        region.fields.push_back({"channel_mask", static_cast<std::uint64_t>(fields.channel_mask)});
        region.fields.push_back(
            {"sub_format_tag", static_cast<std::uint64_t>(read_u16le(payload.data() + 24u))});
        region.fields.push_back(
            {"sub_format_guid", runtime::hex_encode(payload.data() + 24u, 16u)});
        // A second fmt is either identical to the first or refused, and
        // duplicate_fmt covers it, so only the first raises field warnings.
        if (walk.fmt_count == 1u && fields.valid_bits > fields.bits_per_sample) {
            b.warn(LayoutWarningKind::valid_bits_above_container, region.offset,
                   "valid bits " + std::to_string(fields.valid_bits) + " exceed container bits " +
                       std::to_string(fields.bits_per_sample));
        }
        const auto mask_channels = static_cast<std::uint32_t>(std::popcount(fields.channel_mask));
        if (walk.fmt_count == 1u && fields.channel_mask != 0u && mask_channels != fields.channels) {
            b.warn(LayoutWarningKind::extensible_mask_channel_mismatch, region.offset,
                   "channel mask names " + std::to_string(mask_channels) + " speakers for " +
                       std::to_string(fields.channels) + " channels");
        }
    } else if (fields.format_tag == 0xFFFEu) {
        region.note = "extensible format tag without the 40-byte extension";
        fields.extensible = true;
    }
    if (walk.fmt_count == 1u)
        walk.fmt = std::move(fields);
}

void parse_ds64(Builder& b, LayoutRegion& region, Ds64& ds64, std::uint64_t payload_offset,
                std::uint64_t declared) {
    auto payload = b.reader.read_capped(payload_offset, declared, 4096u);
    if (payload.size() < 28u) {
        region.note = "ds64 too short: payload is under the 28-byte minimum";
        return;
    }
    ds64.riff_size = read_u64le(payload.data());
    ds64.data_size = read_u64le(payload.data() + 8u);
    ds64.sample_count = read_u64le(payload.data() + 16u);
    const auto table_length = read_u32le(payload.data() + 24u);
    region.fields.push_back({"riff_size", ds64.riff_size});
    region.fields.push_back({"data_size", ds64.data_size});
    region.fields.push_back({"sample_count", ds64.sample_count});
    region.fields.push_back({"table_length", static_cast<std::uint64_t>(table_length)});
    for (std::uint32_t i = 0; i < table_length; ++i) {
        const std::size_t at = 28u + static_cast<std::size_t>(i) * 12u;
        if (at + 12u > payload.size())
            break;
        const auto id = fourcc(payload.data() + at);
        const auto size = read_u64le(payload.data() + at + 4u);
        ds64.table[id] = size;
        region.fields.push_back({"table." + id, size});
    }
}

void parse_bext(Builder& b, LayoutRegion& region, std::uint64_t payload_offset,
                std::uint64_t declared) {
    auto payload = b.reader.read_capped(payload_offset, declared, kMaxPayloadRead);
    if (payload.size() < 602u) {
        region.note = "bext payload shorter than the 602-byte fixed header";
        return;
    }
    const auto version = read_u16le(payload.data() + 346u);
    region.fields.push_back({"version", static_cast<std::uint64_t>(version)});
    region.fields.push_back({"time_reference", read_u64le(payload.data() + 338u)});
    if (version >= 2u) {
        auto s16 = [&](std::size_t at) {
            return static_cast<std::int64_t>(
                static_cast<std::int16_t>(read_u16le(payload.data() + at)));
        };
        region.fields.push_back({"loudness_value", s16(412u)});
        region.fields.push_back({"loudness_range", s16(414u)});
        region.fields.push_back({"max_true_peak_level", s16(416u)});
        region.fields.push_back({"max_momentary_loudness", s16(418u)});
        region.fields.push_back({"max_short_term_loudness", s16(420u)});
    }
    if (const auto bext = decode_bext_chunk(payload.data(), payload.size())) {
        b.text("bext", "description", bext->description);
        b.text("bext", "originator", bext->originator);
        b.text("bext", "originator_reference", bext->originator_reference);
        b.text("bext", "origination_date", bext->origination_date);
        b.text("bext", "origination_time", bext->origination_time);
        b.text("bext", "coding_history", bext->coding_history);
    }
}

void parse_list(Builder& b, LayoutRegion& region, std::uint64_t payload_offset,
                std::uint64_t declared) {
    auto payload = b.reader.read_capped(payload_offset, declared, kMaxPayloadRead);
    if (payload.size() < 4u)
        return;
    const auto list_type = fourcc(payload.data());
    region.fields.push_back({"list_type", list_type});
    std::uint64_t entries = 0u;
    std::size_t at = 4u;
    while (at + 8u <= payload.size()) {
        const auto id = fourcc(payload.data() + at);
        const auto size = read_u32le(payload.data() + at + 4u);
        const auto value_at = at + 8u;
        if (size > payload.size() - value_at)
            break;
        ++entries;
        if (list_type == "INFO")
            b.text("LIST", id, trimmed_text(payload.data() + value_at, size));
        at = value_at + size + (size & 1u);
    }
    region.fields.push_back({"entries", entries});
    if (declared > payload.size())
        region.note = "payload read capped; entry count covers the first " +
                      std::to_string(payload.size()) + " bytes";
}

void parse_acid(Builder& b, LayoutRegion& region, std::uint64_t payload_offset,
                std::uint64_t declared) {
    auto payload = b.reader.read_capped(payload_offset, declared, 64u);
    if (const auto acid = decode_acid_chunk(payload.data(), payload.size())) {
        region.fields.push_back({"flags", static_cast<std::uint64_t>(acid->flags)});
        region.fields.push_back({"root_note", static_cast<std::uint64_t>(acid->root_note)});
        region.fields.push_back({"num_beats", static_cast<std::uint64_t>(acid->num_beats)});
        region.fields.push_back(
            {"meter_numerator", static_cast<std::uint64_t>(acid->meter_numerator)});
        region.fields.push_back(
            {"meter_denominator", static_cast<std::uint64_t>(acid->meter_denominator)});
        region.fields.push_back({"tempo_bpm", static_cast<double>(acid->tempo_bpm)});
    }
}

void parse_fact(Builder& b, LayoutRegion& region, const Envelope& env, const IffChunk& chunk) {
    const std::uint64_t count_bytes = env.w64 ? 8u : 4u;
    auto payload = b.reader.read_capped(chunk.payload_offset, chunk.declared, count_bytes);
    if (payload.size() == count_bytes) {
        const std::uint64_t count =
            env.w64 ? read_u64le(payload.data()) : read_u32le(payload.data());
        region.fields.push_back({"sample_length", count});
    } else {
        region.note = "fact payload shorter than its sample count";
    }
}

// A cursor over the report's bounded reader, so the probe's reads are
// counted in bytes_read like the walk's own.
WaveByteCursor cursor_over(Reader& reader) noexcept {
    WaveByteCursor cursor;
    cursor.context = &reader;
    cursor.size = reader.size();
    cursor.read_at = [](const WaveByteCursor& self, std::uint64_t offset, std::uint8_t* out,
                        std::size_t count) noexcept -> std::size_t {
        const auto bytes = static_cast<Reader*>(self.context)->read(offset, count);
        std::copy(bytes.begin(), bytes.end(), out);
        return bytes.size();
    };
    return cursor;
}

// The warnings whose truth is the decoder's: the probe's walk is the one the
// decoder reads by, so its list replaces whatever the presentation walk saw
// of these kinds.
bool decoder_owned(LayoutWarningKind kind) noexcept {
    switch (kind) {
    case LayoutWarningKind::envelope_size_mismatch:
    case LayoutWarningKind::missing_pad_byte:
    case LayoutWarningKind::duplicate_fmt:
    case LayoutWarningKind::fmt_after_data:
    case LayoutWarningKind::truncated_data:
    case LayoutWarningKind::sentinel_size_without_ds64:
    case LayoutWarningKind::chunk_overlap:
    case LayoutWarningKind::chunk_past_eof:
        return true;
    case LayoutWarningKind::extensible_mask_channel_mismatch:
    case LayoutWarningKind::valid_bits_above_container:
        return false;
    }
    return false;
}

std::string chunk_id_at(Builder& b, std::uint64_t offset) {
    const auto raw = b.reader.read(offset, 4u);
    return raw.size() == 4u ? fourcc(raw.data()) : std::string("chunk");
}

std::string describe(Builder& b, const WaveWarning& w, const detail::WavProbe& probe) {
    const auto n = [](std::uint64_t v) { return std::to_string(v); };
    switch (w.kind) {
    case LayoutWarningKind::envelope_size_mismatch:
        return "envelope declares " + n(w.declared) + " bytes, file holds " + n(w.actual);
    case LayoutWarningKind::missing_pad_byte:
        return chunk_id_at(b, w.offset) + " has odd size " + n(w.declared) +
               " and ends the file without a pad byte";
    case LayoutWarningKind::duplicate_fmt:
        return probe.layout.refusal == WaveRefusal::conflicting_fmt
                   ? "second fmt chunk contradicts the first"
                   : "second fmt chunk repeats the first";
    case LayoutWarningKind::fmt_after_data:
        return "fmt chunk follows the data chunk";
    case LayoutWarningKind::truncated_data:
    case LayoutWarningKind::chunk_past_eof:
        return chunk_id_at(b, w.offset) + " declares " + n(w.declared) + " bytes, " +
               n(w.declared - w.actual) + " past end of file";
    case LayoutWarningKind::sentinel_size_without_ds64:
        return w.offset == 0u
                   ? std::string("RF64 envelope without a leading ds64 chunk")
                   : chunk_id_at(b, w.offset) + " chunk size is 0xFFFFFFFF with no ds64 entry";
    case LayoutWarningKind::chunk_overlap:
        return "ds64 data size " + n(w.declared) + " disagrees with the data chunk size " +
               n(w.actual);
    case LayoutWarningKind::extensible_mask_channel_mismatch:
    case LayoutWarningKind::valid_bits_above_container:
        break;
    }
    return {};
}

void finish(Builder& b, const Envelope& env, Walk& walk) {
    auto& report = b.report;
    report.facts_confidence = FactsConfidence::parsed;

    // Profile: the envelope wins, then the metadata family, then the fmt tag.
    if (env.w64) {
        report.profile = WavProfile::w64;
    } else if (env.rf64) {
        report.profile = WavProfile::rf64;
    } else if (walk.has_bext) {
        report.profile = WavProfile::bwf;
    } else if (walk.fmt) {
        if (walk.fmt->extensible)
            report.profile = WavProfile::extensible;
        else if (walk.fmt->format_tag == 0x0003u)
            report.profile = WavProfile::floating;
        else if (walk.fmt->format_tag == 0x0001u)
            report.profile = WavProfile::pcm44;
    }

    if (env.w64) {
        // No Pulp reader decodes Wave64, but its fmt chunk is the same
        // structure, so the stream it describes is still reported.
        if (walk.fmt && walk.data_seen) {
            if (const auto facts = interpret_wav_format(walk.fmt->payload, walk.data_size)) {
                LayoutStreamFacts stream;
                stream.sample_rate = facts->sample_rate;
                stream.channels = facts->channels;
                stream.bits_per_sample = facts->bits_per_sample;
                stream.valid_bits = facts->bits_per_sample;
                stream.frames = facts->total_frames;
                stream.declared_frames = facts->declared_frames;
                stream.duration_seconds =
                    facts->sample_rate != 0u
                        ? static_cast<double>(facts->total_frames) / facts->sample_rate
                        : 0.0;
                stream.encoding = encoding_name(facts->translated_format_tag);
                stream.bytes_per_frame = facts->bytes_per_frame;
                report.stream = stream;
            }
        }
        LayoutDecodeVerdict verdict;
        verdict.reason = "Sony Wave64 envelope; no Pulp reader decodes it";
        report.decode = verdict;
        return;
    }

    const auto probe = detail::probe_wav(cursor_over(b.reader));
    auto& warnings = report.warnings;
    warnings.erase(std::remove_if(warnings.begin(), warnings.end(),
                                  [](const LayoutWarning& w) { return decoder_owned(w.kind); }),
                   warnings.end());
    for (const auto& w : probe.layout.warnings)
        b.warn(w.kind, w.offset, describe(b, w, probe));

    const auto& decision = probe.decision;
    if (probe.facts) {
        const auto& facts = *probe.facts;
        LayoutStreamFacts stream;
        stream.sample_rate = facts.sample_rate;
        stream.channels = facts.channels;
        stream.bits_per_sample = facts.bits_per_sample;
        stream.valid_bits = walk.fmt && walk.fmt->extensible && walk.fmt->valid_bits != 0u
                                ? walk.fmt->valid_bits
                                : facts.bits_per_sample;
        stream.frames = decision.recoverable_frames;
        stream.declared_frames = decision.declared_frames;
        stream.duration_seconds =
            facts.sample_rate != 0u
                ? static_cast<double>(decision.recoverable_frames) / facts.sample_rate
                : 0.0;
        stream.encoding = encoding_name(facts.translated_format_tag);
        stream.bytes_per_frame = facts.bytes_per_frame;
        report.stream = stream;
        // The ceilings bind what a decode allocates: the frames present.
        b.set_decode_limits(decision.recoverable_frames, facts.channels);
        // Every byte present but a partial last frame (or block) is still
        // truncation, which the walk alone cannot see.
        if (decision.truncated && !probe.layout.has_warning(LayoutWarningKind::truncated_data)) {
            const auto unit = facts.bytes_per_frame != 0u
                                  ? std::to_string(facts.bytes_per_frame) + "-byte frames"
                                  : std::string("blocks");
            b.warn(LayoutWarningKind::truncated_data, probe.layout.data_header_offset,
                   "data payload of " + std::to_string(probe.layout.data_declared) +
                       " bytes is not a whole number of " + unit);
        }
    }

    LayoutDecodeVerdict verdict;
    verdict.decodable = decision.accepted();
    verdict.truncated = decision.accepted() && decision.truncated;
    verdict.reason = wave_refusal_reason(decision.refusal);
    report.decode = verdict;
}

} // namespace

void inspect_wav_layout(Builder& b) {
    auto& report = b.report;
    const auto file_size = b.reader.size();
    const auto env_opt = read_envelope(b);
    if (!env_opt) {
        report.error = "header shorter than a WAVE envelope";
        return;
    }
    Envelope env = *env_opt;
    Walk walk;
    Ds64 ds64;
    bool have_ds64 = false;

    // RF64 resolves its envelope through ds64, which must be the first chunk.
    std::uint64_t offset = env.iff.header_size;
    if (env.rf64) {
        if (const auto first = read_chunk_header(b, env, offset); first && first->id == "ds64") {
            auto& region = b.add_region(offset, env.iff.chunk_header_size, first->declared,
                                        first->id, LayoutRegionKind::envelope);
            parse_ds64(b, region, ds64, offset + env.iff.chunk_header_size, first->declared);
            have_ds64 = region.fields.size() >= 4u;
            if (have_ds64 && env.declared_sentinel)
                env.iff.declared_end = ds64.riff_size + 8u;
            offset += env.iff.chunk_header_size + first->declared + (first->declared & 1u);
        }
    }

    // Walk to the smaller of the declared envelope and the file, except when
    // the declared size is unusable (zero or shorter than the header), where
    // the file size is the only bound left.
    env.iff.walk_end = env.iff.declared_end >= env.iff.header_size
                           ? std::min(env.iff.declared_end, file_size)
                           : file_size;

    IffHooks hooks;
    hooks.read_header = [&](std::uint64_t at) { return read_chunk_header(b, env, at); };
    hooks.classify = classify;
    // Region sizes only; the warnings these conditions raise are the
    // probe's (see finish).
    hooks.resolve = [&](IffChunk& chunk) {
        if (!chunk.sentinel)
            return;
        if (have_ds64 && chunk.id == "data") {
            chunk.declared = ds64.data_size;
        } else if (have_ds64 && ds64.table.count(chunk.id) != 0u) {
            chunk.declared = ds64.table.at(chunk.id);
        } else {
            chunk.declared =
                chunk.payload_offset <= file_size ? file_size - chunk.payload_offset : 0u;
            chunk.stop_after = true;
        }
    };
    hooks.on_chunk = [&](const IffChunk& chunk, LayoutRegion& region) {
        const auto& id = chunk.id;
        if (id == "fmt ") {
            parse_fmt(b, region, walk, chunk.payload_offset, chunk.declared);
        } else if (id == "data") {
            if (!walk.data_seen) {
                walk.data_seen = true;
                walk.data_size = chunk.declared;
            } else {
                region.note = "second data chunk; the decoder uses the first";
            }
        } else if (id == "fact") {
            parse_fact(b, region, env, chunk);
        } else if (id == "ds64") {
            region.note = "ds64 after the first chunk position; the decoder ignores it here";
        } else if (id == "bext") {
            walk.has_bext = true;
            parse_bext(b, region, chunk.payload_offset, chunk.declared);
        } else if (id == "LIST") {
            parse_list(b, region, chunk.payload_offset, chunk.declared);
        } else if (id == "acid") {
            parse_acid(b, region, chunk.payload_offset, chunk.declared);
        } else if (id == "iXML" || id == "axml" || id == "_PMX") {
            b.read_text(id, "xml", chunk.payload_offset, chunk.declared);
        }
    };
    walk_iff_chunks(b, env.iff, offset, hooks);

    finish(b, env, walk);
}

} // namespace pulp::audio::layout_detail
