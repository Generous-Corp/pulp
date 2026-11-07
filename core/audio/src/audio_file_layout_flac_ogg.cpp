// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

// Native FLAC metadata-block walk and Ogg page walk. FLAC facts are parsed
// from STREAMINFO, which the format defines; Ogg facts are the codec's own
// identification header on the first page and carry container-only
// confidence because nothing here verifies them against the stream.

#include "audio_file_layout_detail.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace pulp::audio::layout_detail {
namespace {

const char* flac_block_name(std::uint8_t type) {
    switch (type) {
        case 0: return "STREAMINFO";
        case 1: return "PADDING";
        case 2: return "APPLICATION";
        case 3: return "SEEKTABLE";
        case 4: return "VORBIS_COMMENT";
        case 5: return "CUESHEET";
        case 6: return "PICTURE";
        default: return "RESERVED";
    }
}

void parse_vorbis_comments(Builder& b, LayoutRegion& region, const std::string& region_id,
                           const std::uint8_t* p, std::size_t size) {
    if (size < 8u)
        return;
    const auto vendor_length = read_u32le(p);
    std::size_t at = 4u;
    if (vendor_length > size - at)
        return;
    const std::string vendor(reinterpret_cast<const char*>(p + at), vendor_length);
    at += vendor_length;
    if (at + 4u > size)
        return;
    const auto count = read_u32le(p + at);
    at += 4u;
    region.fields.push_back({"vendor_length", static_cast<std::uint64_t>(vendor_length)});
    region.fields.push_back({"comment_count", static_cast<std::uint64_t>(count)});
    b.text(region_id, "vendor", vendor);
    for (std::uint32_t i = 0; i < count && at + 4u <= size; ++i) {
        const auto length = read_u32le(p + at);
        at += 4u;
        if (length > size - at)
            break;
        const std::string entry(reinterpret_cast<const char*>(p + at), length);
        at += length;
        const auto eq = entry.find('=');
        if (eq == std::string::npos)
            continue;
        b.text(region_id, entry.substr(0, eq), entry.substr(eq + 1u));
    }
}

} // namespace

void inspect_flac_layout(Builder& b) {
    auto& report = b.report;
    const auto file_size = b.reader.size();
    report.facts_confidence = FactsConfidence::parsed;
    b.add_region(0u, 0u, 4u, "fLaC", LayoutRegionKind::envelope);

    std::uint64_t offset = 4u;
    bool last = false;
    bool streaminfo_seen = false;
    std::uint64_t total_samples = 0u;
    std::uint32_t sample_rate = 0u;
    std::uint32_t channels = 0u;
    std::uint32_t bits = 0u;
    while (!last && offset < file_size && !report.regions_truncated) {
        std::array<std::uint8_t, 4> header{};
        if (!b.reader.read_exact(offset, header)) {
            auto& stub = b.add_region(offset, 0u, file_size - offset, "", LayoutRegionKind::unknown);
            stub.note = "shorter than a metadata block header";
            break;
        }
        last = (header[0] & 0x80u) != 0u;
        const auto type = static_cast<std::uint8_t>(header[0] & 0x7fu);
        const auto length = static_cast<std::uint64_t>(read_u24be(header.data() + 1u));
        const auto payload_offset = offset + 4u;
        const std::string id = flac_block_name(type);
        LayoutRegionKind kind = LayoutRegionKind::metadata;
        if (type == 0u) kind = LayoutRegionKind::fmt;
        else if (type == 1u) kind = LayoutRegionKind::padding;
        else if (type >= 7u) kind = LayoutRegionKind::unknown;
        auto& region = b.add_region(offset, 4u, length, id, kind);
        region.fields.push_back({"block_type", static_cast<std::uint64_t>(type)});
        region.fields.push_back({"is_last", last});

        const bool past_eof = payload_offset > file_size || length > file_size - payload_offset;
        if (past_eof) {
            b.warn(LayoutWarningKind::chunk_past_eof, offset,
                   id + " declares " + std::to_string(length) + " bytes, " +
                       std::to_string(payload_offset + length - file_size) + " past end of file");
        }

        if (type == 0u) {
            if (streaminfo_seen)
                b.warn(LayoutWarningKind::duplicate_fmt, offset, "second STREAMINFO block");
            auto p = b.reader.read(payload_offset, std::min<std::uint64_t>(length, 34u));
            if (p.size() == 34u) {
                const auto min_block = read_u16be(p.data());
                const auto max_block = read_u16be(p.data() + 2u);
                const auto min_frame = read_u24be(p.data() + 4u);
                const auto max_frame = read_u24be(p.data() + 7u);
                const auto packed = read_u64be(p.data() + 10u);
                const auto rate = static_cast<std::uint32_t>(packed >> 44u);
                const auto ch = static_cast<std::uint32_t>((packed >> 41u) & 0x7u) + 1u;
                const auto bps = static_cast<std::uint32_t>((packed >> 36u) & 0x1fu) + 1u;
                const auto samples = packed & ((1ull << 36u) - 1u);
                region.fields.push_back({"min_block_size", static_cast<std::uint64_t>(min_block)});
                region.fields.push_back({"max_block_size", static_cast<std::uint64_t>(max_block)});
                region.fields.push_back({"min_frame_size", static_cast<std::uint64_t>(min_frame)});
                region.fields.push_back({"max_frame_size", static_cast<std::uint64_t>(max_frame)});
                region.fields.push_back({"sample_rate", static_cast<std::uint64_t>(rate)});
                region.fields.push_back({"channels", static_cast<std::uint64_t>(ch)});
                region.fields.push_back({"bits_per_sample", static_cast<std::uint64_t>(bps)});
                region.fields.push_back({"total_samples", samples});
                region.fields.push_back({"variable_block_size", min_block != max_block});
                region.fields.push_back({"md5", hex_bytes(p.data() + 18u, 16u)});
                if (!streaminfo_seen) {
                    sample_rate = rate;
                    channels = ch;
                    bits = bps;
                    total_samples = samples;
                }
            } else {
                region.note = "STREAMINFO shorter than 34 bytes";
            }
            streaminfo_seen = true;
        } else if (type == 3u) {
            region.fields.push_back({"seek_points", length / 18u});
            b.present(id, offset, length);
        } else if (type == 4u) {
            b.present(id, offset, length);
            auto p = b.reader.read(payload_offset, std::min<std::uint64_t>(length, kMaxPayloadRead));
            parse_vorbis_comments(b, region, id, p.data(), p.size());
        } else if (type == 2u) {
            auto p = b.reader.read(payload_offset, std::min<std::uint64_t>(length, 4u));
            if (p.size() == 4u)
                region.fields.push_back({"application_id", printable_id(fourcc(p.data())) ? fourcc(p.data()) : hex_bytes(p.data(), 4u)});
            b.present(id, offset, length);
        } else if (type == 6u) {
            auto p = b.reader.read(payload_offset, std::min<std::uint64_t>(length, 512u));
            if (p.size() >= 8u) {
                region.fields.push_back({"picture_type", static_cast<std::uint64_t>(read_u32be(p.data()))});
                const auto mime_length = read_u32be(p.data() + 4u);
                std::size_t at = 8u;
                if (mime_length <= p.size() - at) {
                    region.fields.push_back({"mime_type", std::string(reinterpret_cast<const char*>(p.data() + at), mime_length)});
                    at += mime_length;
                    if (at + 4u <= p.size()) {
                        const auto desc_length = read_u32be(p.data() + at);
                        at += 4u;
                        if (desc_length <= p.size() - at && at + desc_length + 20u <= p.size()) {
                            at += desc_length;
                            region.fields.push_back({"width", static_cast<std::uint64_t>(read_u32be(p.data() + at))});
                            region.fields.push_back({"height", static_cast<std::uint64_t>(read_u32be(p.data() + at + 4u))});
                            region.fields.push_back({"data_length", static_cast<std::uint64_t>(read_u32be(p.data() + at + 16u))});
                        }
                    }
                }
            }
            b.present(id, offset, length);
        } else if (type == 5u) {
            b.present(id, offset, length);
        }

        if (past_eof)
            break;
        offset = payload_offset + length;
    }

    // First frame header, when the metadata ended inside the file.
    if (last && offset < file_size) {
        auto& frames = b.add_region(offset, 0u, file_size - offset, "frames", LayoutRegionKind::data);
        std::array<std::uint8_t, 4> header{};
        if (b.reader.read_exact(offset, header)) {
            const auto sync = static_cast<std::uint32_t>((header[0] << 6u) | (header[1] >> 2u));
            if (sync == 0x3FFEu) {
                frames.fields.push_back({"first_frame_blocking_strategy",
                                         std::string((header[1] & 0x1u) != 0u ? "variable" : "fixed")});
                frames.fields.push_back({"first_frame_block_size_code", static_cast<std::uint64_t>(header[2] >> 4u)});
                frames.fields.push_back({"first_frame_sample_rate_code", static_cast<std::uint64_t>(header[2] & 0xfu)});
            } else {
                frames.note = "bytes after the last metadata block do not start with a frame sync";
            }
        }
    }

    if (streaminfo_seen) {
        LayoutStreamFacts stream;
        stream.sample_rate = sample_rate;
        stream.channels = channels;
        stream.bits_per_sample = bits;
        stream.valid_bits = bits;
        stream.frames = total_samples;
        stream.duration_seconds = sample_rate != 0u ? static_cast<double>(total_samples) / sample_rate : 0.0;
        stream.encoding = "flac";
        report.stream = stream;
        b.set_decode_limits(total_samples, channels);
    } else {
        report.error = "no STREAMINFO block";
    }
}

// ── Ogg ─────────────────────────────────────────────────────────────────────

namespace {

constexpr std::size_t kMaxOggPages = 512u;

struct OggPage {
    std::uint64_t offset = 0u;
    std::uint64_t size = 0u;        // header + lacing + body
    std::uint64_t body_offset = 0u;
    std::uint64_t body_size = 0u;
    std::uint8_t header_type = 0u;
    std::uint64_t granule = 0u;
    std::uint32_t serial = 0u;
    std::uint32_t sequence = 0u;
    std::uint8_t segments = 0u;
};

bool read_page(Reader& reader, std::uint64_t offset, OggPage& page) {
    std::array<std::uint8_t, 27> header{};
    if (!reader.read_exact(offset, header) || std::memcmp(header.data(), "OggS", 4u) != 0)
        return false;
    page.offset = offset;
    page.header_type = header[5];
    page.granule = read_u64le(header.data() + 6u);
    page.serial = read_u32le(header.data() + 14u);
    page.sequence = read_u32le(header.data() + 18u);
    page.segments = header[26];
    const auto lacing = reader.read(offset + 27u, page.segments);
    if (lacing.size() != page.segments)
        return false;
    page.body_size = 0u;
    for (const auto lace : lacing)
        page.body_size += lace;
    page.body_offset = offset + 27u + page.segments;
    page.size = 27u + page.segments + page.body_size;
    return true;
}

struct OggStream {
    std::uint32_t serial = 0u;
    std::string codec;
    std::uint32_t channels = 0u;
    std::uint32_t sample_rate = 0u;
    std::uint32_t pre_skip = 0u;
    std::uint64_t last_granule = 0u;
    std::uint64_t pages = 0u;
    std::size_t data_region = SIZE_MAX;   // index into report.regions, SIZE_MAX when none
    bool eos = false;
    bool comments_pending = false;
};

void identify(Builder& b, LayoutRegion& region, OggStream& stream, const std::uint8_t* p, std::size_t size) {
    if (size >= 30u && std::memcmp(p, "\x01vorbis", 7u) == 0) {
        stream.codec = "vorbis";
        stream.channels = p[11];
        stream.sample_rate = read_u32le(p + 12u);
        region.fields.push_back({"vorbis_version", static_cast<std::uint64_t>(read_u32le(p + 7u))});
        region.fields.push_back({"bitrate_nominal", static_cast<std::int64_t>(static_cast<std::int32_t>(read_u32le(p + 20u)))});
    } else if (size >= 19u && std::memcmp(p, "OpusHead", 8u) == 0) {
        stream.codec = "opus";
        stream.channels = p[9];
        stream.pre_skip = read_u16le(p + 10u);
        stream.sample_rate = 48000u;
        region.fields.push_back({"opus_version", static_cast<std::uint64_t>(p[8])});
        region.fields.push_back({"pre_skip", static_cast<std::uint64_t>(stream.pre_skip)});
        region.fields.push_back({"input_sample_rate", static_cast<std::uint64_t>(read_u32le(p + 12u))});
        region.fields.push_back({"output_gain", static_cast<std::int64_t>(static_cast<std::int16_t>(read_u16le(p + 16u)))});
        region.fields.push_back({"channel_mapping_family", static_cast<std::uint64_t>(p[18])});
    } else if (size >= 51u && std::memcmp(p, "\x7f""FLAC", 5u) == 0 && std::memcmp(p + 9u, "fLaC", 4u) == 0) {
        stream.codec = "flac";
        const auto packed = read_u64be(p + 17u + 10u);
        stream.sample_rate = static_cast<std::uint32_t>(packed >> 44u);
        stream.channels = static_cast<std::uint32_t>((packed >> 41u) & 0x7u) + 1u;
        region.fields.push_back({"bits_per_sample", static_cast<std::uint64_t>(((packed >> 36u) & 0x1fu) + 1u)});
        region.fields.push_back({"total_samples", packed & ((1ull << 36u) - 1u)});
        region.fields.push_back({"header_packets", static_cast<std::uint64_t>(read_u16be(p + 7u))});
    } else if (size >= 8u && std::memcmp(p, "Speex   ", 8u) == 0) {
        stream.codec = "speex";
        if (size >= 52u) {
            stream.sample_rate = read_u32le(p + 36u);
            stream.channels = read_u32le(p + 48u);
        }
    } else {
        stream.codec = "unknown";
        region.fields.push_back({"first_packet_prefix", hex_bytes(p, std::min<std::size_t>(size, 8u))});
    }
    region.fields.push_back({"codec", stream.codec});
    if (stream.channels != 0u)
        region.fields.push_back({"channels", static_cast<std::uint64_t>(stream.channels)});
    if (stream.sample_rate != 0u)
        region.fields.push_back({"sample_rate", static_cast<std::uint64_t>(stream.sample_rate)});
    (void)b;
}

} // namespace

void inspect_ogg_layout(Builder& b) {
    auto& report = b.report;
    const auto file_size = b.reader.size();
    report.facts_confidence = FactsConfidence::container_only;

    std::vector<OggStream> streams;
    std::uint64_t offset = 0u;
    std::size_t pages = 0u;
    bool walk_complete = false;
    while (offset < file_size && pages < kMaxOggPages && !report.regions_truncated) {
        OggPage page;
        if (!read_page(b.reader, offset, page)) {
            auto& stub = b.add_region(offset, 0u, file_size - offset, "", LayoutRegionKind::unknown);
            stub.note = pages == 0u ? "first page header is malformed" : "bytes after the last whole page do not start a page";
            break;
        }
        ++pages;
        const bool bos = (page.header_type & 0x02u) != 0u;
        const bool eos = (page.header_type & 0x04u) != 0u;
        const bool past_eof = page.offset + page.size > file_size;
        if (past_eof) {
            b.warn(LayoutWarningKind::chunk_past_eof, page.offset,
                   "page " + std::to_string(page.sequence) + " declares " + std::to_string(page.size) +
                       " bytes, " + std::to_string(page.offset + page.size - file_size) + " past end of file");
        }

        OggStream* stream = nullptr;
        for (auto& candidate : streams) {
            if (candidate.serial == page.serial) {
                stream = &candidate;
                break;
            }
        }
        if (bos || stream == nullptr) {
            streams.push_back(OggStream{});
            stream = &streams.back();
            stream->serial = page.serial;
            stream->comments_pending = true;
            auto& region = b.add_region(page.offset, 0u, page.size, "OggS", LayoutRegionKind::fmt);
            region.fields.push_back({"serial", static_cast<std::uint64_t>(page.serial)});
            region.fields.push_back({"page_sequence", static_cast<std::uint64_t>(page.sequence)});
            region.fields.push_back({"bos", bos});
            region.fields.push_back({"segments", static_cast<std::uint64_t>(page.segments)});
            auto body = b.reader.read(page.body_offset, std::min<std::uint64_t>(page.body_size, 512u));
            identify(b, region, *stream, body.data(), body.size());
            region.note = bos ? "identification header" : "page with no preceding BOS page for its serial";
        } else if (stream->comments_pending) {
            stream->comments_pending = false;
            auto& region = b.add_region(page.offset, 0u, page.size, "OggS", LayoutRegionKind::metadata);
            region.fields.push_back({"serial", static_cast<std::uint64_t>(page.serial)});
            region.fields.push_back({"page_sequence", static_cast<std::uint64_t>(page.sequence)});
            region.note = "comment header";
            b.present(stream->codec == "opus" ? "OpusTags" : "VorbisComment", page.offset, page.size);
            auto body = b.reader.read(page.body_offset, std::min<std::uint64_t>(page.body_size, kMaxPayloadRead));
            std::size_t skip = 0u;
            if (stream->codec == "vorbis" && body.size() >= 7u) skip = 7u;
            else if (stream->codec == "opus" && body.size() >= 8u) skip = 8u;
            if (skip != 0u || stream->codec == "flac")
                parse_vorbis_comments(b, region, "OggS", body.data() + skip, body.size() - skip);
        } else {
            if (stream->data_region == SIZE_MAX) {
                const auto index = report.regions.size();
                auto& region = b.add_region(page.offset, 0u, 0u, "OggS", LayoutRegionKind::data);
                region.note = "audio pages for serial " + std::to_string(page.serial);
                region.fields.push_back({"serial", static_cast<std::uint64_t>(page.serial)});
                if (!report.regions_truncated)
                    stream->data_region = index;
            }
            ++stream->pages;
            if (stream->data_region != SIZE_MAX) {
                auto& region = report.regions[stream->data_region];
                region.declared_size = (page.offset + page.size) - region.offset;
                region.actual_size = std::min(region.declared_size, file_size - region.offset);
            }
        }
        stream->last_granule = page.granule;
        stream->eos = stream->eos || eos;
        if (past_eof)
            break;
        offset = page.offset + page.size;
        if (offset >= file_size)
            walk_complete = true;
    }

    // Close out each data region's page count and granule facts.
    for (const auto& stream : streams) {
        if (stream.data_region == SIZE_MAX)
            continue;
        auto& region = report.regions[stream.data_region];
        region.fields.push_back({"pages", stream.pages});
        region.fields.push_back({"last_granule", stream.last_granule});
        region.fields.push_back({"eos", stream.eos});
    }

    report.container_detail = "OggS";
    if (streams.empty()) {
        report.error = "no Ogg page could be read";
        return;
    }
    auto& first = streams.front();
    LayoutStreamFacts facts;
    facts.sample_rate = first.sample_rate;
    facts.channels = first.channels;
    facts.encoding = first.codec;
    if (walk_complete && first.eos && streams.size() == 1u) {
        facts.frames = first.codec == "opus" && first.last_granule >= first.pre_skip
            ? first.last_granule - first.pre_skip : first.last_granule;
        facts.duration_seconds = facts.sample_rate != 0u
            ? static_cast<double>(facts.frames) / facts.sample_rate : 0.0;
    }
    report.stream = facts;
    report.regions.front().fields.push_back({"logical_streams", static_cast<std::uint64_t>(streams.size())});
    report.regions.front().fields.push_back({"chained", streams.size() > 1u});
    report.regions.front().fields.push_back({"pages_walked", static_cast<std::uint64_t>(pages)});
    report.regions.front().fields.push_back({"page_walk_complete", walk_complete});
}

} // namespace pulp::audio::layout_detail
