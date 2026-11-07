// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

// RIFF/WAVE, RF64/BW64, and Sony Wave64 layout walk.
//
// The walk is tolerant where `inspect_wav` is strict: it records every
// region it can locate and turns each specification violation into a
// warning, then derives the decoder's verdict from the same rules
// `inspect_wav` applies, so the two agree on accept/reject while the report
// still describes a file the decoder refuses.

#include "audio_file_layout_detail.hpp"

#include <pulp/audio/wav_metadata.hpp>

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
    std::uint64_t header_size = 12u;       // bytes before the first chunk
    std::uint64_t chunk_header_size = 8u;  // id + size
    std::uint64_t alignment = 2u;          // chunk start alignment
    std::uint64_t declared_end = 0u;       // envelope end the header claims
    bool declared_sentinel = false;        // RIFF size field is 0xFFFFFFFF
};

struct Ds64 {
    std::uint64_t riff_size = 0u;
    std::uint64_t data_size = 0u;
    std::uint64_t sample_count = 0u;
    std::unordered_map<std::string, std::uint64_t> table;
};

struct FormatFields {
    std::vector<std::uint8_t> payload;
    std::uint64_t offset = 0u;
    std::uint16_t format_tag = 0u;
    std::uint16_t channels = 0u;
    std::uint32_t sample_rate = 0u;
    std::uint16_t bits_per_sample = 0u;
    std::uint16_t block_align = 0u;
    std::uint16_t valid_bits = 0u;
    std::uint32_t channel_mask = 0u;
    bool extensible = false;
    bool has_channel_mask = false;
};

struct Walk {
    std::optional<FormatFields> fmt;
    std::uint64_t fmt_count = 0u;
    bool data_seen = false;
    std::uint64_t data_offset = 0u;
    std::uint64_t data_size = 0u;        // size the decoder would use
    std::uint64_t data_actual = 0u;
    std::optional<std::uint64_t> fact_frames;
    bool has_bext = false;
    bool structural_fault = false;      // the strict decoder stops here
    std::string first_fault;
};

LayoutRegionKind classify(std::string_view id) {
    static constexpr std::array<std::string_view, 24> kMetadata{
        "bext", "iXML", "axml", "acid", "LIST", "cue ", "smpl", "inst", "plst", "labl",
        "note", "ltxt", "PEAK", "ID3 ", "id3 ", "umid", "levl", "link", "chna", "minf",
        "elm1", "CART", "_PMX", "DISP"};
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
        case 0x0001: return "pcm";
        case 0x0002: return "adpcm";
        case 0x0003: return "float";
        case 0x0006: return "alaw";
        case 0x0007: return "mulaw";
        case 0x0011: return "adpcm";
        default: return "unknown";
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
        env.header_size = 40u;
        env.chunk_header_size = 24u;
        env.alignment = 8u;
        // Wave64 sizes include their own headers, so the riff size is the
        // whole file.
        env.declared_end = read_u64le(head.data() + 16u);
        auto& region = b.add_region(0u, 0u, env.declared_end, "riff", LayoutRegionKind::envelope);
        region.fields.push_back({"form", std::string("wave")});
        region.fields.push_back({"riff_size", env.declared_end});
        return env;
    }
    const auto id = fourcc(head.data());
    env.rf64 = id == "RF64" || id == "BW64";
    const auto size = read_u32le(head.data() + 4u);
    env.declared_sentinel = size == kSizeSentinel;
    env.declared_end = static_cast<std::uint64_t>(size) + 8u;
    auto& region = b.add_region(0u, 8u, size, id, LayoutRegionKind::envelope);
    region.fields.push_back({"form", fourcc(head.data() + 8u)});
    region.fields.push_back({"riff_size", static_cast<std::uint64_t>(size)});
    return env;
}

// Chunk header at `offset`: id and payload size. W64 chunks carry GUID ids
// and sizes that include the 24-byte header; both are normalised here.
struct ChunkHeader {
    std::string id;
    std::uint64_t declared = 0u;
    bool sentinel = false;
    bool printable = true;
};

std::optional<ChunkHeader> read_chunk_header(Builder& b, const Envelope& env, std::uint64_t offset) {
    std::array<std::uint8_t, 24> raw{};
    if (!b.reader.read_exact(offset, std::span(raw.data(), env.chunk_header_size)))
        return std::nullopt;
    ChunkHeader header;
    if (env.w64) {
        if (std::memcmp(raw.data() + 4u, kW64ChunkSuffix.data(), kW64ChunkSuffix.size()) == 0) {
            header.id = fourcc(raw.data());
        } else {
            header.id = hex_bytes(raw.data(), 16u);
        }
        const auto total = read_u64le(raw.data() + 16u);
        header.declared = total >= 24u ? total - 24u : 0u;
        if (total < 24u)
            header.printable = false;
    } else {
        header.id = fourcc(raw.data());
        const auto size = read_u32le(raw.data() + 4u);
        header.declared = size;
        header.sentinel = size == kSizeSentinel;
    }
    header.printable = header.printable && printable_id(header.id);
    return header;
}

void parse_fmt(Builder& b, LayoutRegion& region, Walk& walk, std::uint64_t payload_offset,
               std::uint64_t declared) {
    ++walk.fmt_count;
    if (walk.fmt_count > 1u) {
        b.warn(LayoutWarningKind::duplicate_fmt, region.offset,
               "second fmt chunk; the decoder uses the first");
    }
    if (walk.data_seen && walk.fmt_count == 1u) {
        b.warn(LayoutWarningKind::fmt_after_data, region.offset,
               "fmt chunk follows the data chunk");
    }
    auto payload = b.reader.read(payload_offset, std::min<std::uint64_t>(declared, 1024u));
    if (payload.size() < 16u) {
        region.note = "fmt payload shorter than the 16-byte PCM header";
        if (walk.fmt_count == 1u) {
            FormatFields fields;
            fields.payload = payload;
            fields.offset = region.offset;
            walk.fmt = std::move(fields);
        }
        return;
    }
    FormatFields fields;
    fields.payload = payload;
    fields.offset = region.offset;
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
    region.fields.push_back({"bits_per_sample", static_cast<std::uint64_t>(fields.bits_per_sample)});
    if (payload.size() >= 18u) {
        region.fields.push_back({"cb_size", static_cast<std::uint64_t>(read_u16le(payload.data() + 16u))});
    }
    fields.valid_bits = fields.bits_per_sample;
    if (fields.format_tag == 0xFFFEu && payload.size() >= 40u) {
        fields.extensible = true;
        fields.valid_bits = read_u16le(payload.data() + 18u);
        fields.channel_mask = read_u32le(payload.data() + 20u);
        fields.has_channel_mask = true;
        region.fields.push_back({"valid_bits", static_cast<std::uint64_t>(fields.valid_bits)});
        region.fields.push_back({"channel_mask", static_cast<std::uint64_t>(fields.channel_mask)});
        region.fields.push_back({"sub_format_tag", static_cast<std::uint64_t>(read_u16le(payload.data() + 24u))});
        region.fields.push_back({"sub_format_guid", hex_bytes(payload.data() + 24u, 16u)});
        if (fields.valid_bits > fields.bits_per_sample) {
            b.warn(LayoutWarningKind::valid_bits_above_container, region.offset,
                   "valid bits " + std::to_string(fields.valid_bits) + " exceed container bits " +
                       std::to_string(fields.bits_per_sample));
        }
        const auto mask_channels = static_cast<std::uint32_t>(std::popcount(fields.channel_mask));
        if (fields.channel_mask != 0u && mask_channels != fields.channels) {
            b.warn(LayoutWarningKind::extensible_mask_channel_mismatch, region.offset,
                   "channel mask names " + std::to_string(mask_channels) +
                       " speakers for " + std::to_string(fields.channels) + " channels");
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
    auto payload = b.reader.read(payload_offset, std::min<std::uint64_t>(declared, 4096u));
    if (payload.size() < 28u) {
        region.note = "ds64 payload shorter than 28 bytes";
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

void parse_bext(Builder& b, LayoutRegion& region, std::uint64_t payload_offset, std::uint64_t declared) {
    auto payload = b.reader.read(payload_offset, std::min<std::uint64_t>(declared, kMaxPayloadRead));
    if (payload.size() < 602u) {
        region.note = "bext payload shorter than the 602-byte fixed header";
        return;
    }
    const auto version = read_u16le(payload.data() + 346u);
    region.fields.push_back({"version", static_cast<std::uint64_t>(version)});
    region.fields.push_back({"time_reference", read_u64le(payload.data() + 338u)});
    if (version >= 2u) {
        auto s16 = [&](std::size_t at) {
            return static_cast<std::int64_t>(static_cast<std::int16_t>(read_u16le(payload.data() + at)));
        };
        region.fields.push_back({"loudness_value", s16(412u)});
        region.fields.push_back({"loudness_range", s16(414u)});
        region.fields.push_back({"max_true_peak_level", s16(416u)});
        region.fields.push_back({"max_momentary_loudness", s16(418u)});
        region.fields.push_back({"max_short_term_loudness", s16(420u)});
    }
    // Builder::text is the one place that decides whether free text is kept.
    if (const auto bext = decode_bext_chunk(payload.data(), payload.size())) {
        b.text("bext", "description", bext->description);
        b.text("bext", "originator", bext->originator);
        b.text("bext", "originator_reference", bext->originator_reference);
        b.text("bext", "origination_date", bext->origination_date);
        b.text("bext", "origination_time", bext->origination_time);
        b.text("bext", "coding_history", bext->coding_history);
    }
}

void parse_list(Builder& b, LayoutRegion& region, std::uint64_t payload_offset, std::uint64_t declared) {
    auto payload = b.reader.read(payload_offset, std::min<std::uint64_t>(declared, kMaxPayloadRead));
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

void parse_acid(Builder& b, LayoutRegion& region, std::uint64_t payload_offset, std::uint64_t declared) {
    auto payload = b.reader.read(payload_offset, std::min<std::uint64_t>(declared, 64u));
    if (const auto acid = decode_acid_chunk(payload.data(), payload.size())) {
        region.fields.push_back({"flags", static_cast<std::uint64_t>(acid->flags)});
        region.fields.push_back({"root_note", static_cast<std::uint64_t>(acid->root_note)});
        region.fields.push_back({"num_beats", static_cast<std::uint64_t>(acid->num_beats)});
        region.fields.push_back({"meter_numerator", static_cast<std::uint64_t>(acid->meter_numerator)});
        region.fields.push_back({"meter_denominator", static_cast<std::uint64_t>(acid->meter_denominator)});
        region.fields.push_back({"tempo_bpm", static_cast<double>(acid->tempo_bpm)});
    }
}

void parse_xml_text(Builder& b, const std::string& id, std::uint64_t payload_offset, std::uint64_t declared) {
    if (!b.options.include_metadata_text)
        return;
    auto payload = b.reader.read(payload_offset, std::min<std::uint64_t>(declared, kMaxPayloadRead));
    auto text = trimmed_text(payload.data(), payload.size());
    if (declared > payload.size())
        text += "\n[truncated at " + std::to_string(payload.size()) + " bytes]";
    b.text(id, "xml", std::move(text));
}

void finish(Builder& b, const Envelope& env, Walk& walk, std::uint64_t walk_end) {
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

    std::optional<WavFormatFacts> facts;
    if (walk.fmt && walk.data_seen) {
        facts = interpret_wav_format(walk.fmt->payload, walk.data_size, walk.fact_frames);
    }
    bool whole_frames = true;
    if (facts) {
        LayoutStreamFacts stream;
        stream.sample_rate = facts->sample_rate;
        stream.channels = facts->channels;
        stream.bits_per_sample = facts->bits_per_sample;
        stream.valid_bits = walk.fmt->extensible ? walk.fmt->valid_bits : facts->bits_per_sample;
        stream.frames = facts->total_frames;
        stream.duration_seconds = facts->sample_rate != 0u
            ? static_cast<double>(facts->total_frames) / facts->sample_rate : 0.0;
        stream.encoding = encoding_name(facts->translated_format_tag);
        stream.bytes_per_frame = facts->bytes_per_frame;
        report.stream = stream;
        b.set_decode_limits(facts->total_frames, facts->channels);
        const bool adpcm = facts->translated_format_tag == 0x0002u ||
                           facts->translated_format_tag == 0x0011u;
        if (!adpcm && facts->bytes_per_frame != 0u &&
            walk.data_size % facts->bytes_per_frame != 0u) {
            whole_frames = false;
            b.warn(LayoutWarningKind::truncated_data, walk.data_offset,
                   "data payload of " + std::to_string(walk.data_size) + " bytes is not a whole number of " +
                       std::to_string(facts->bytes_per_frame) + "-byte frames");
        }
    }

    // The decoder's verdict, in the order inspect_wav checks.
    LayoutDecodeVerdict verdict;
    if (env.w64 || env.rf64) {
        verdict.reason = "in-memory decoder accepts RIFF envelopes only";
    } else if (env.declared_end != b.reader.size()) {
        verdict.reason = "RIFF size " + std::to_string(env.declared_end - 8u) + " does not match file size " +
                         std::to_string(b.reader.size());
    } else if (walk.structural_fault) {
        verdict.reason = walk.first_fault;
    } else if (!walk.fmt) {
        verdict.reason = "no fmt chunk";
    } else if (!walk.data_seen) {
        verdict.reason = "no data chunk";
    } else if (!facts) {
        verdict.reason = "decoder rejects the fmt chunk";
    } else if (facts->channels == 0u) {
        verdict.reason = "zero channels";
    } else if (facts->sample_rate == 0u) {
        verdict.reason = "zero sample rate";
    } else if (facts->total_frames == 0u) {
        verdict.reason = "zero frames";
    } else if (!whole_frames) {
        verdict.reason = "data payload is not a whole number of frames";
    } else {
        verdict.decodable = true;
    }
    report.decode = verdict;
    (void)walk_end;
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
    std::uint64_t offset = env.header_size;
    if (env.rf64) {
        if (const auto first = read_chunk_header(b, env, offset); first && first->id == "ds64") {
            auto& region = b.add_region(offset, env.chunk_header_size, first->declared, first->id, LayoutRegionKind::envelope);
            parse_ds64(b, region, ds64, offset + env.chunk_header_size, first->declared);
            have_ds64 = region.fields.size() >= 4u;
            if (have_ds64 && env.declared_sentinel)
                env.declared_end = ds64.riff_size + 8u;
            offset += env.chunk_header_size + first->declared + (first->declared & 1u);
        }
        if (!have_ds64) {
            b.warn(LayoutWarningKind::sentinel_size_without_ds64, 0u,
                   "RF64 envelope without a leading ds64 chunk");
        }
    }

    if (env.declared_end != file_size) {
        b.warn(LayoutWarningKind::envelope_size_mismatch, 0u,
               "envelope declares " + std::to_string(env.declared_end) + " bytes, file holds " +
                   std::to_string(file_size));
    }
    // Walk to the smaller of the declared envelope and the file, except when
    // the declared size is unusable (zero or shorter than the header), where
    // the file size is the only bound left.
    const auto walk_end = env.declared_end >= env.header_size
        ? std::min(env.declared_end, file_size) : file_size;
    while (offset < walk_end && !report.regions_truncated) {
        if (walk_end - offset < env.chunk_header_size) {
            auto& stub = b.add_region(offset, 0u, walk_end - offset, "", LayoutRegionKind::unknown);
            stub.note = "shorter than a chunk header";
            if (!walk.structural_fault) {
                walk.structural_fault = true;
                walk.first_fault = "trailing bytes shorter than a chunk header";
            }
            break;
        }
        const auto header = read_chunk_header(b, env, offset);
        if (!header)
            break;
        const auto payload_offset = offset + env.chunk_header_size;
        std::uint64_t declared = header->declared;
        bool stop_after = false;

        if (header->sentinel) {
            if (have_ds64 && header->id == "data") {
                declared = ds64.data_size;
            } else if (have_ds64 && ds64.table.count(header->id) != 0u) {
                declared = ds64.table.at(header->id);
            } else {
                b.warn(LayoutWarningKind::sentinel_size_without_ds64, offset,
                       header->id + " chunk size is 0xFFFFFFFF with no ds64 entry");
                declared = payload_offset <= file_size ? file_size - payload_offset : 0u;
                stop_after = true;
                if (!walk.structural_fault) {
                    walk.structural_fault = true;
                    walk.first_fault = header->id + " chunk size is 0xFFFFFFFF with no ds64 to resolve it";
                }
            }
        } else if (env.rf64 && have_ds64 && header->id == "data" && ds64.data_size != declared) {
            // The decoder trusts ds64 for the data size, so the payload it
            // reads extends past the 32-bit extent the walk uses.
            if (ds64.data_size > declared) {
                b.warn(LayoutWarningKind::chunk_overlap, offset,
                       "ds64 data size " + std::to_string(ds64.data_size) + " extends " +
                           std::to_string(ds64.data_size - declared) + " bytes past the data chunk's own size " +
                           std::to_string(declared) + ", over the chunks that follow");
            } else {
                b.warn(LayoutWarningKind::chunk_overlap, offset,
                       "ds64 data size " + std::to_string(ds64.data_size) + " disagrees with the data chunk size " +
                           std::to_string(declared));
            }
        }

        const auto kind = header->printable ? classify(header->id) : LayoutRegionKind::unknown;
        auto& region = b.add_region(offset, env.chunk_header_size, declared, header->id, kind);
        if (!header->printable)
            region.note = "chunk id is not printable ASCII; the walk may be out of step with the file";

        const bool past_eof = payload_offset > file_size || declared > file_size - payload_offset;
        const auto chunk_end = past_eof ? file_size : payload_offset + declared;
        if (past_eof) {
            const auto missing = payload_offset + declared - file_size;
            if (header->id == "data") {
                b.warn(LayoutWarningKind::truncated_data, offset,
                       "data declares " + std::to_string(declared) + " bytes, " + std::to_string(missing) +
                           " past end of file");
            } else {
                b.warn(LayoutWarningKind::chunk_past_eof, offset,
                       header->id + " declares " + std::to_string(declared) + " bytes, " +
                           std::to_string(missing) + " past end of file");
            }
            if (!walk.structural_fault) {
                walk.structural_fault = true;
                walk.first_fault = header->id + " chunk extends past end of file";
            }
        }

        std::uint64_t pad = 0u;
        if (!env.w64 && (declared & 1u) != 0u) {
            pad = 1u;
            if (!past_eof && chunk_end >= file_size) {
                b.warn(LayoutWarningKind::missing_pad_byte, offset,
                       header->id + " has odd size " + std::to_string(declared) + " and ends the file without a pad byte");
                if (!walk.structural_fault) {
                    walk.structural_fault = true;
                    walk.first_fault = header->id + " chunk is odd-sized with no pad byte";
                }
                pad = 0u;
            }
        } else if (env.w64) {
            const auto total = env.chunk_header_size + declared;
            pad = (env.alignment - (total % env.alignment)) % env.alignment;
        }

        if (header->id == "fmt ") {
            parse_fmt(b, region, walk, payload_offset, declared);
        } else if (header->id == "data") {
            if (!walk.data_seen) {
                walk.data_seen = true;
                walk.data_offset = offset;
                walk.data_size = (env.rf64 && have_ds64) ? ds64.data_size : declared;
                walk.data_actual = region.actual_size;
            } else {
                region.note = "second data chunk; the decoder uses the first";
            }
        } else if (header->id == "fact") {
            auto payload = b.reader.read(payload_offset, std::min<std::uint64_t>(declared, 4u));
            if (payload.size() == 4u) {
                const auto count = read_u32le(payload.data());
                region.fields.push_back({"sample_length", static_cast<std::uint64_t>(count)});
                if (!walk.fact_frames)
                    walk.fact_frames = count;
            }
        } else if (header->id == "ds64") {
            region.note = "ds64 after the first chunk position; the decoder ignores it here";
        } else if (header->id == "bext") {
            walk.has_bext = true;
            b.present(header->id, offset, declared);
            parse_bext(b, region, payload_offset, declared);
        } else if (header->id == "LIST") {
            b.present(header->id, offset, declared);
            parse_list(b, region, payload_offset, declared);
        } else if (header->id == "acid") {
            b.present(header->id, offset, declared);
            parse_acid(b, region, payload_offset, declared);
        } else if (header->id == "iXML" || header->id == "axml" || header->id == "_PMX") {
            b.present(header->id, offset, declared);
            parse_xml_text(b, header->id, payload_offset, declared);
        } else if (kind == LayoutRegionKind::metadata) {
            b.present(header->id, offset, declared);
        }

        if (stop_after || past_eof)
            break;
        offset = chunk_end + pad;
    }
    if (env.declared_end > env.header_size && env.declared_end < file_size) {
        auto& trailing = b.add_region(env.declared_end, 0u, file_size - env.declared_end, "",
                                      LayoutRegionKind::unknown);
        trailing.note = "bytes after the declared envelope";
    }

    finish(b, env, walk, walk_end);
}

} // namespace pulp::audio::layout_detail
