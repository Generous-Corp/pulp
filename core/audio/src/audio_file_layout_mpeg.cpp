// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

// MPEG audio (MP3) tag-and-first-frame walk and ISO base media (MP4/M4A)
// box walk. Both report container identity and the first frame's or the
// sample description's claims with container-only confidence: a bounded
// read cannot verify a lossy stream's length or priming against its audio.

#include "audio_file_layout_detail.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <string>

namespace pulp::audio::layout_detail {
namespace {

// ── MP3 ─────────────────────────────────────────────────────────────────────

constexpr std::uint64_t kSyncSearchWindow = 64u * 1024u;

struct FrameHeader {
    std::uint32_t version_index = 0u;   // 0 = 2.5, 2 = 2, 3 = 1
    std::uint32_t layer = 0u;           // 1, 2, 3
    std::uint32_t bitrate_kbps = 0u;
    std::uint32_t sample_rate = 0u;
    std::uint32_t channels = 0u;
    std::uint32_t channel_mode = 0u;
    std::uint32_t frame_length = 0u;
    std::uint32_t samples_per_frame = 0u;
    bool protection = false;
    bool padding = false;
};

bool frame_header_valid(const std::uint8_t* h) noexcept {
    if (h[0] != 0xffu || (h[1] & 0xe0u) != 0xe0u)
        return false;
    const auto version = (h[1] >> 3u) & 0x3u;
    const auto layer = (h[1] >> 1u) & 0x3u;
    const auto bitrate = h[2] >> 4u;
    const auto rate = (h[2] >> 2u) & 0x3u;
    return version != 1u && layer != 0u && bitrate != 0u && bitrate != 15u && rate != 3u;
}

std::optional<FrameHeader> parse_frame_header(const std::uint8_t* h) {
    if (!frame_header_valid(h))
        return std::nullopt;
    static constexpr std::uint16_t kBitrates[2][3][16] = {
        // MPEG1: layer1, layer2, layer3
        {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
         {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
         {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}},
        // MPEG2/2.5: layer1, layer2, layer3
        {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}}};
    static constexpr std::uint32_t kRates[4][3] = {
        {11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};
    FrameHeader f;
    f.version_index = (h[1] >> 3u) & 0x3u;
    const auto layer_bits = (h[1] >> 1u) & 0x3u;
    f.layer = 4u - layer_bits;
    f.protection = (h[1] & 0x1u) == 0u;
    const auto bitrate_index = h[2] >> 4u;
    const auto rate_index = (h[2] >> 2u) & 0x3u;
    f.padding = ((h[2] >> 1u) & 0x1u) != 0u;
    f.channel_mode = h[3] >> 6u;
    f.channels = f.channel_mode == 3u ? 1u : 2u;
    const bool mpeg1 = f.version_index == 3u;
    f.bitrate_kbps = kBitrates[mpeg1 ? 0 : 1][f.layer - 1u][bitrate_index];
    f.sample_rate = kRates[f.version_index][rate_index];
    if (f.layer == 1u) {
        f.samples_per_frame = 384u;
        f.frame_length = (12u * f.bitrate_kbps * 1000u / f.sample_rate + (f.padding ? 1u : 0u)) * 4u;
    } else {
        f.samples_per_frame = (f.layer == 3u && !mpeg1) ? 576u : 1152u;
        f.frame_length = f.samples_per_frame / 8u * f.bitrate_kbps * 1000u / f.sample_rate + (f.padding ? 1u : 0u);
    }
    return f;
}

const char* version_name(std::uint32_t index) {
    switch (index) {
        case 0: return "2.5";
        case 2: return "2";
        case 3: return "1";
        default: return "reserved";
    }
}

const char* channel_mode_name(std::uint32_t mode) {
    switch (mode) {
        case 0: return "stereo";
        case 1: return "joint_stereo";
        case 2: return "dual_channel";
        default: return "mono";
    }
}

std::uint32_t syncsafe(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0] & 0x7fu) << 21u) | (static_cast<std::uint32_t>(p[1] & 0x7fu) << 14u) |
           (static_cast<std::uint32_t>(p[2] & 0x7fu) << 7u) | static_cast<std::uint32_t>(p[3] & 0x7fu);
}

std::string id3_text(const std::uint8_t* p, std::size_t size) {
    if (size == 0u)
        return {};
    const auto encoding = p[0];
    const auto* body = p + 1u;
    const auto body_size = size - 1u;
    switch (encoding) {
        case 1: return utf16_to_utf8(body, body_size, false);
        case 2: return utf16_to_utf8(body, body_size, true);
        default: return trimmed_text(body, body_size);
    }
}

void parse_id3v2_frames(Builder& b, LayoutRegion& region, const std::uint8_t* p, std::size_t size,
                        std::uint8_t major) {
    std::uint64_t frames = 0u;
    std::size_t at = 0u;
    const std::size_t header_size = major >= 3u ? 10u : 6u;
    while (at + header_size <= size) {
        if (p[at] == 0u)
            break;
        std::string id;
        std::uint32_t frame_size = 0u;
        if (major >= 3u) {
            id = fourcc(p + at);
            frame_size = major == 4u ? syncsafe(p + at + 4u) : read_u32be(p + at + 4u);
        } else {
            id = std::string(reinterpret_cast<const char*>(p + at), 3u);
            frame_size = read_u24be(p + at + 3u);
        }
        const auto body = at + header_size;
        if (frame_size > size - body)
            break;
        ++frames;
        if (!id.empty() && id[0] == 'T')
            b.text("ID3v2", id, id3_text(p + body, frame_size));
        at = body + frame_size;
    }
    region.fields.push_back({"frames", frames});
}

} // namespace

void inspect_mp3_layout(Builder& b) {
    auto& report = b.report;
    const auto file_size = b.reader.size();
    report.facts_confidence = FactsConfidence::container_only;
    report.container_detail = "mpeg";

    std::uint64_t audio_start = 0u;
    const auto head = b.reader.read(0u, 10u);
    if (head.size() == 10u && std::memcmp(head.data(), "ID3", 3u) == 0) {
        const auto major = head[3];
        const auto flags = head[5];
        const std::uint64_t tag_size = syncsafe(head.data() + 6u);
        const bool footer = (flags & 0x10u) != 0u;
        const auto total = 10u + tag_size + (footer ? 10u : 0u);
        auto& region = b.add_region(0u, 0u, total, "ID3v2", LayoutRegionKind::metadata);
        region.fields.push_back({"version", std::string("2.") + std::to_string(major) + "." + std::to_string(head[4])});
        region.fields.push_back({"flags", static_cast<std::uint64_t>(flags)});
        region.fields.push_back({"unsynchronisation", (flags & 0x80u) != 0u});
        region.fields.push_back({"extended_header", (flags & 0x40u) != 0u});
        region.fields.push_back({"footer", footer});
        b.present("ID3v2", 0u, total);
        if (total > file_size) {
            b.warn(LayoutWarningKind::chunk_past_eof, 0u,
                   "ID3v2 tag declares " + std::to_string(total) + " bytes, " +
                       std::to_string(total - file_size) + " past end of file");
        } else {
            auto body = b.reader.read(10u, std::min<std::uint64_t>(tag_size, kMaxPayloadRead));
            std::size_t skip = 0u;
            if ((flags & 0x40u) != 0u && body.size() >= 4u)
                skip = major == 4u ? syncsafe(body.data()) : read_u32be(body.data()) + 4u;
            if (skip < body.size())
                parse_id3v2_frames(b, region, body.data() + skip, body.size() - skip, major);
        }
        audio_start = std::min<std::uint64_t>(total, file_size);
    }

    // Tail tags: ID3v1 is the last 128 bytes; an APEv2 footer sits just
    // before it (or at the very end when there is no ID3v1).
    std::uint64_t audio_end = file_size;
    if (file_size >= 128u) {
        auto tail = b.reader.read(file_size - 128u, 128u);
        if (tail.size() == 128u && std::memcmp(tail.data(), "TAG", 3u) == 0) {
            auto& region = b.add_region(file_size - 128u, 0u, 128u, "ID3v1", LayoutRegionKind::metadata);
            region.fields.push_back({"has_track_number", tail[125] == 0u && tail[126] != 0u});
            b.present("ID3v1", file_size - 128u, 128u);
            b.text("ID3v1", "title", trimmed_text(tail.data() + 3u, 30u));
            b.text("ID3v1", "artist", trimmed_text(tail.data() + 33u, 30u));
            b.text("ID3v1", "album", trimmed_text(tail.data() + 63u, 30u));
            audio_end = file_size - 128u;
        }
    }
    if (audio_end >= 32u) {
        auto footer = b.reader.read(audio_end - 32u, 32u);
        if (footer.size() == 32u && std::memcmp(footer.data(), "APETAGEX", 8u) == 0) {
            const auto version = read_u32le(footer.data() + 8u);
            const std::uint64_t tag_size = read_u32le(footer.data() + 12u);
            const auto items = read_u32le(footer.data() + 16u);
            const auto flags = read_u32le(footer.data() + 20u);
            const bool has_header = (flags & 0x80000000u) != 0u;
            const auto total = tag_size + (has_header ? 32u : 0u);
            const auto start = total <= audio_end ? audio_end - total : 0u;
            auto& region = b.add_region(start, 0u, total, "APEv2", LayoutRegionKind::metadata);
            region.fields.push_back({"version", static_cast<std::uint64_t>(version)});
            region.fields.push_back({"items", static_cast<std::uint64_t>(items)});
            region.fields.push_back({"has_header", has_header});
            b.present("APEv2", start, total);
            if (total > audio_end) {
                b.warn(LayoutWarningKind::chunk_overlap, start,
                       "APE tag declares " + std::to_string(total) + " bytes but only " +
                           std::to_string(audio_end) + " precede its footer");
            }
            audio_end = start;
        }
    }

    // First frame: search a bounded window after the leading tag for a sync
    // whose computed length lands on another valid sync (or end of audio).
    std::optional<FrameHeader> first;
    std::uint64_t first_offset = audio_start;
    {
        const auto window = b.reader.read(audio_start, std::min<std::uint64_t>(kSyncSearchWindow, audio_end > audio_start ? audio_end - audio_start : 0u));
        for (std::size_t i = 0u; i + 4u <= window.size(); ++i) {
            const auto candidate = parse_frame_header(window.data() + i);
            if (!candidate || candidate->frame_length == 0u)
                continue;
            const auto next = audio_start + i + candidate->frame_length;
            bool confirmed = next >= audio_end;
            if (!confirmed) {
                std::array<std::uint8_t, 4> next_header{};
                confirmed = b.reader.read_exact(next, next_header) && frame_header_valid(next_header.data());
            }
            if (confirmed) {
                first = candidate;
                first_offset = audio_start + i;
                break;
            }
        }
    }

    if (first_offset > audio_start) {
        auto& gap = b.add_region(audio_start, 0u, first_offset - audio_start, "", LayoutRegionKind::unknown);
        gap.note = "bytes between the leading tag and the first frame sync";
    }
    if (!first) {
        if (audio_end > first_offset) {
            auto& rest = b.add_region(first_offset, 0u, audio_end - first_offset, "", LayoutRegionKind::unknown);
            rest.note = "no MPEG frame sync found in the first " + std::to_string(kSyncSearchWindow) + " bytes";
        }
        report.error = "no MPEG audio frame found";
        return;
    }
    auto& frames = b.add_region(first_offset, 0u, audio_end > first_offset ? audio_end - first_offset : 0u,
                                "frames", LayoutRegionKind::data);
    frames.fields.push_back({"mpeg_version", std::string(version_name(first->version_index))});
    frames.fields.push_back({"layer", static_cast<std::uint64_t>(first->layer)});
    frames.fields.push_back({"first_frame_bitrate_kbps", static_cast<std::uint64_t>(first->bitrate_kbps)});
    frames.fields.push_back({"sample_rate", static_cast<std::uint64_t>(first->sample_rate)});
    frames.fields.push_back({"channel_mode", std::string(channel_mode_name(first->channel_mode))});
    frames.fields.push_back({"first_frame_length", static_cast<std::uint64_t>(first->frame_length)});
    frames.fields.push_back({"samples_per_frame", static_cast<std::uint64_t>(first->samples_per_frame)});
    frames.fields.push_back({"crc_protection", first->protection});

    // Xing/Info or VBRI header inside the first frame.
    std::uint64_t xing_frames = 0u;
    std::uint64_t xing_bytes = 0u;
    {
        const bool mpeg1 = first->version_index == 3u;
        const std::size_t side_info = mpeg1 ? (first->channels == 1u ? 17u : 32u)
                                            : (first->channels == 1u ? 9u : 17u);
        auto frame = b.reader.read(first_offset, std::min<std::uint64_t>(first->frame_length, 2048u));
        const std::size_t xing_at = 4u + side_info;
        if (frame.size() >= xing_at + 8u &&
            (std::memcmp(frame.data() + xing_at, "Xing", 4u) == 0 || std::memcmp(frame.data() + xing_at, "Info", 4u) == 0)) {
            const auto tag = fourcc(frame.data() + xing_at);
            const auto flags = read_u32be(frame.data() + xing_at + 4u);
            std::size_t at = xing_at + 8u;
            frames.fields.push_back({"vbr_header", tag});
            if ((flags & 0x1u) != 0u && at + 4u <= frame.size()) {
                xing_frames = read_u32be(frame.data() + at);
                frames.fields.push_back({"vbr_frames", xing_frames});
                at += 4u;
            }
            if ((flags & 0x2u) != 0u && at + 4u <= frame.size()) {
                xing_bytes = read_u32be(frame.data() + at);
                frames.fields.push_back({"vbr_bytes", xing_bytes});
                at += 4u;
            }
            if ((flags & 0x4u) != 0u)
                at += 100u;
            if ((flags & 0x8u) != 0u && at + 4u <= frame.size()) {
                frames.fields.push_back({"vbr_quality", static_cast<std::uint64_t>(read_u32be(frame.data() + at))});
                at += 4u;
            }
            // LAME extension: encoder string then delay/padding packed in 3 bytes.
            if (at + 24u <= frame.size() && printable_id(std::string(reinterpret_cast<const char*>(frame.data() + at), 4u))) {
                frames.fields.push_back({"encoder", trimmed_text(frame.data() + at, 9u)});
                const auto* dp = frame.data() + at + 21u;
                const auto delay = (static_cast<std::uint32_t>(dp[0]) << 4u) | (dp[1] >> 4u);
                const auto padding = (static_cast<std::uint32_t>(dp[1] & 0xfu) << 8u) | dp[2];
                frames.fields.push_back({"encoder_delay", static_cast<std::uint64_t>(delay)});
                frames.fields.push_back({"encoder_padding", static_cast<std::uint64_t>(padding)});
            }
        } else if (frame.size() >= 4u + 32u + 26u && std::memcmp(frame.data() + 36u, "VBRI", 4u) == 0) {
            frames.fields.push_back({"vbr_header", std::string("VBRI")});
            frames.fields.push_back({"vbri_delay", static_cast<std::uint64_t>(read_u16be(frame.data() + 42u))});
            xing_bytes = read_u32be(frame.data() + 46u);
            xing_frames = read_u32be(frame.data() + 50u);
            frames.fields.push_back({"vbr_bytes", xing_bytes});
            frames.fields.push_back({"vbr_frames", xing_frames});
        }
    }

    LayoutStreamFacts facts;
    facts.sample_rate = first->sample_rate;
    facts.channels = first->channels;
    facts.encoding = first->layer == 3u ? "mp3" : (first->layer == 2u ? "mp2" : "mp1");
    if (xing_frames != 0u) {
        facts.frames = xing_frames * first->samples_per_frame;
        facts.duration_seconds = static_cast<double>(facts.frames) / first->sample_rate;
    }
    report.stream = facts;
}

// ── MP4 ─────────────────────────────────────────────────────────────────────

namespace {

constexpr std::uint64_t kMaxMoovRead = 1024u * 1024u;
constexpr std::size_t kMaxBoxDepth = 8u;

struct Mp4Facts {
    std::uint32_t timescale = 0u;
    std::uint64_t duration = 0u;
    std::uint32_t channels = 0u;
    std::uint32_t sample_rate = 0u;
    std::string codec;
    std::string handler;
    std::uint64_t stts_samples = 0u;
    std::uint64_t media_timescale = 0u;
    std::uint64_t media_duration = 0u;
    bool co64 = false;
    bool stco = false;
    bool audio_track_done = false;
};

LayoutRegionKind box_kind(std::string_view type) {
    if (type == "ftyp") return LayoutRegionKind::envelope;
    if (type == "moov" || type == "trak" || type == "mdia" || type == "minf" || type == "stbl" ||
        type == "mvhd" || type == "tkhd" || type == "mdhd" || type == "hdlr" || type == "stsd" ||
        type == "stts" || type == "stsz" || type == "stsc" || type == "stco" || type == "co64" ||
        type == "esds" || type == "elst" || type == "edts" || type == "smhd" || type == "dinf")
        return LayoutRegionKind::fmt;
    if (type == "mdat") return LayoutRegionKind::data;
    if (type == "free" || type == "skip" || type == "wide") return LayoutRegionKind::padding;
    if (type == "udta" || type == "meta" || type == "ilst" || type == "chpl" || type == "uuid")
        return LayoutRegionKind::metadata;
    return LayoutRegionKind::unknown;
}

// Reads an MPEG-4 descriptor's expandable length.
std::uint32_t descriptor_length(const std::uint8_t* p, std::size_t size, std::size_t& at) {
    std::uint32_t length = 0u;
    for (std::size_t i = 0; i < 4u && at < size; ++i) {
        const auto byte = p[at++];
        length = (length << 7u) | (byte & 0x7fu);
        if ((byte & 0x80u) == 0u)
            break;
    }
    return length;
}

void parse_esds(LayoutRegion& region, Mp4Facts& facts, const std::uint8_t* p, std::size_t size) {
    std::size_t at = 4u;  // version + flags
    while (at < size) {
        const auto tag = p[at++];
        const auto length = descriptor_length(p, size, at);
        const auto end = std::min(size, at + length);
        if (tag == 0x03u) {
            at += 3u;  // ES_ID + flags; assume no dependsOn/URL/OCR flags
            continue;
        }
        if (tag == 0x04u && at + 13u <= end) {
            const auto oti = p[at];
            region.fields.push_back({"object_type_indication", static_cast<std::uint64_t>(oti)});
            region.fields.push_back({"max_bitrate", static_cast<std::uint64_t>(read_u32be(p + at + 5u))});
            region.fields.push_back({"avg_bitrate", static_cast<std::uint64_t>(read_u32be(p + at + 9u))});
            if (oti == 0x40u) facts.codec = "aac";
            else if (oti == 0x6bu || oti == 0x69u) facts.codec = "mp3";
            at += 13u;
            continue;
        }
        if (tag == 0x05u && at + 2u <= end) {
            const auto aot = p[at] >> 3u;
            const auto freq_index = ((p[at] & 0x7u) << 1u) | (p[at + 1u] >> 7u);
            const auto channel_config = (p[at + 1u] >> 3u) & 0xfu;
            region.fields.push_back({"audio_object_type", static_cast<std::uint64_t>(aot)});
            region.fields.push_back({"sampling_frequency_index", static_cast<std::uint64_t>(freq_index)});
            region.fields.push_back({"channel_configuration", static_cast<std::uint64_t>(channel_config)});
            // The sample entry's channelcount is pinned to 2 by QuickTime-
            // style writers regardless of the stream; the AudioSpecificConfig
            // is the codec's own statement.
            if (channel_config >= 1u && channel_config <= 7u)
                facts.channels = channel_config == 7u ? 8u : channel_config;
            if (aot == 2u) facts.codec = "aac-lc";
            else if (aot == 5u) facts.codec = "he-aac";
            else if (aot == 29u) facts.codec = "he-aac-v2";
            return;
        }
        at = end;
    }
}

void walk_boxes(Builder& b, Mp4Facts& facts, std::span<const std::uint8_t> bytes, std::uint64_t base,
                std::size_t depth, const std::string& parent);

void parse_leaf(Builder& b, Mp4Facts& facts, LayoutRegion& region, const std::string& type,
                std::span<const std::uint8_t> body, std::uint64_t body_offset, std::size_t depth,
                const std::string& parent) {
    const auto* p = body.data();
    const auto size = body.size();
    if (type == "mvhd" && size >= 20u) {
        const auto version = p[0];
        if (version == 1u && size >= 32u) {
            facts.timescale = read_u32be(p + 20u);
            facts.duration = read_u64be(p + 24u);
        } else {
            facts.timescale = read_u32be(p + 12u);
            facts.duration = read_u32be(p + 16u);
        }
        region.fields.push_back({"timescale", static_cast<std::uint64_t>(facts.timescale)});
        region.fields.push_back({"duration", facts.duration});
    } else if (type == "mdhd" && size >= 20u && !facts.audio_track_done) {
        const auto version = p[0];
        if (version == 1u && size >= 32u) {
            facts.media_timescale = read_u32be(p + 20u);
            facts.media_duration = read_u64be(p + 24u);
        } else {
            facts.media_timescale = read_u32be(p + 12u);
            facts.media_duration = read_u32be(p + 16u);
        }
        region.fields.push_back({"timescale", facts.media_timescale});
        region.fields.push_back({"duration", facts.media_duration});
    } else if (type == "hdlr" && size >= 12u) {
        const auto handler = fourcc(p + 8u);
        region.fields.push_back({"handler_type", handler});
        if (!facts.audio_track_done)
            facts.handler = handler;
    } else if (type == "stsd" && size >= 16u) {
        const auto entries = read_u32be(p + 4u);
        region.fields.push_back({"entry_count", static_cast<std::uint64_t>(entries)});
        const auto entry_size = read_u32be(p + 8u);
        const auto entry_type = fourcc(p + 12u);
        region.fields.push_back({"first_entry", entry_type});
        if (facts.handler == "soun" && !facts.audio_track_done && size >= 16u + 28u) {
            const auto* e = p + 16u;  // after the entry's own size+type
            const auto channels = read_u16be(e + 16u);
            const auto sample_size = read_u16be(e + 18u);
            const auto rate = read_u32be(e + 24u) >> 16u;
            region.fields.push_back({"channels", static_cast<std::uint64_t>(channels)});
            region.fields.push_back({"sample_size", static_cast<std::uint64_t>(sample_size)});
            region.fields.push_back({"sample_rate", static_cast<std::uint64_t>(rate)});
            facts.channels = channels;
            facts.sample_rate = rate;
            facts.codec = entry_type;
            // Child boxes of the sample entry (esds, alac, dac3) follow its
            // 8-byte header and 28-byte audio body, so they start 44 bytes
            // into the stsd body.
            constexpr std::size_t kEntryChildren = 8u + 36u;
            if (entry_size > 36u && kEntryChildren < size && depth + 1u < kMaxBoxDepth) {
                const auto children = body.subspan(kEntryChildren, std::min<std::size_t>(entry_size - 36u, size - kEntryChildren));
                walk_boxes(b, facts, children, body_offset + kEntryChildren, depth + 1u, entry_type);
            }
        }
    } else if (type == "esds") {
        if (parent == "mp4a" || facts.handler == "soun")
            parse_esds(region, facts, p, size);
    } else if (type == "stts" && size >= 8u) {
        const auto entries = read_u32be(p + 4u);
        region.fields.push_back({"entry_count", static_cast<std::uint64_t>(entries)});
        std::uint64_t total = 0u;
        for (std::uint32_t i = 0; i < entries; ++i) {
            const std::size_t at = 8u + static_cast<std::size_t>(i) * 8u;
            if (at + 8u > size)
                break;
            total += static_cast<std::uint64_t>(read_u32be(p + at)) * read_u32be(p + at + 4u);
        }
        region.fields.push_back({"total_sample_delta", total});
        if (facts.handler == "soun" && !facts.audio_track_done)
            facts.stts_samples = total;
    } else if (type == "stsz" && size >= 12u) {
        region.fields.push_back({"sample_size", static_cast<std::uint64_t>(read_u32be(p + 4u))});
        region.fields.push_back({"sample_count", static_cast<std::uint64_t>(read_u32be(p + 8u))});
    } else if ((type == "stco" || type == "co64") && size >= 8u) {
        region.fields.push_back({"entry_count", static_cast<std::uint64_t>(read_u32be(p + 4u))});
        if (facts.handler == "soun" && !facts.audio_track_done) {
            if (type == "co64") facts.co64 = true; else facts.stco = true;
        }
    } else if (type == "elst" && size >= 8u) {
        const auto version = p[0];
        const auto entries = read_u32be(p + 4u);
        region.fields.push_back({"entry_count", static_cast<std::uint64_t>(entries)});
        if (entries >= 1u) {
            if (version == 1u && size >= 28u) {
                region.fields.push_back({"first_segment_duration", read_u64be(p + 8u)});
                region.fields.push_back({"first_media_time", static_cast<std::int64_t>(read_u64be(p + 16u))});
            } else if (size >= 20u) {
                region.fields.push_back({"first_segment_duration", static_cast<std::uint64_t>(read_u32be(p + 8u))});
                region.fields.push_back({"first_media_time", static_cast<std::int64_t>(static_cast<std::int32_t>(read_u32be(p + 12u)))});
            }
        }
    } else if (type == "data" && parent.size() == 4u && size >= 8u) {
        const auto data_type = read_u32be(p);
        if (data_type == 1u)
            b.text("ilst", parent, trimmed_text(p + 8u, size - 8u));
    }
}

void walk_boxes(Builder& b, Mp4Facts& facts, std::span<const std::uint8_t> bytes, std::uint64_t base,
                std::size_t depth, const std::string& parent) {
    std::size_t at = 0u;
    while (at + 8u <= bytes.size() && !b.report.regions_truncated) {
        std::uint64_t size = read_u32be(bytes.data() + at);
        const auto type = fourcc(bytes.data() + at + 4u);
        std::size_t header = 8u;
        if (size == 1u) {
            if (at + 16u > bytes.size())
                break;
            size = read_u64be(bytes.data() + at + 8u);
            header = 16u;
        } else if (size == 0u) {
            size = bytes.size() - at;
        }
        if (size < header)
            break;
        const auto kind = printable_id(type) ? box_kind(type) : LayoutRegionKind::unknown;
        auto& region = b.add_region(base + at, header, size - header, type, kind);
        if (type == "meta" && parent != "ilst") {
            header += 4u;  // full box: version + flags before children
        }
        const auto body_end = std::min<std::uint64_t>(bytes.size(), at + size);
        if (at + header > body_end)
            break;
        const auto body = bytes.subspan(at + header, static_cast<std::size_t>(body_end - at - header));
        const bool container = type == "moov" || type == "trak" || type == "mdia" || type == "minf" ||
                               type == "stbl" || type == "udta" || type == "meta" || type == "ilst" ||
                               type == "edts" || type == "dinf" || (parent == "ilst");
        if (type == "udta" || type == "meta" || type == "ilst")
            b.present(type, base + at, size);
        if (container && depth + 1u < kMaxBoxDepth) {
            walk_boxes(b, facts, body, base + at + header, depth + 1u, type);
            if (type == "trak" && facts.handler == "soun")
                facts.audio_track_done = true;
            if (type == "trak" && !facts.audio_track_done)
                facts.handler.clear();
        } else {
            parse_leaf(b, facts, region, type, body, base + at + header, depth, parent);
        }
        at = static_cast<std::size_t>(body_end);
    }
}

} // namespace

void inspect_mp4_layout(Builder& b) {
    auto& report = b.report;
    const auto file_size = b.reader.size();
    report.facts_confidence = FactsConfidence::container_only;
    Mp4Facts facts;
    std::uint64_t offset = 0u;
    while (offset < file_size && !report.regions_truncated) {
        std::array<std::uint8_t, 16> raw{};
        if (file_size - offset < 8u || !b.reader.read_exact(offset, std::span(raw.data(), 8u))) {
            auto& stub = b.add_region(offset, 0u, file_size - offset, "", LayoutRegionKind::unknown);
            stub.note = "shorter than a box header";
            break;
        }
        std::uint64_t size = read_u32be(raw.data());
        const auto type = fourcc(raw.data() + 4u);
        std::uint64_t header = 8u;
        bool largesize = false;
        if (size == 1u) {
            if (!b.reader.read_exact(offset + 8u, std::span(raw.data() + 8u, 8u)))
                break;
            size = read_u64be(raw.data() + 8u);
            header = 16u;
            largesize = true;
        } else if (size == 0u) {
            size = file_size - offset;
        }
        const bool printable = printable_id(type);
        auto& region = b.add_region(offset, header, size >= header ? size - header : 0u, type,
                                    printable ? box_kind(type) : LayoutRegionKind::unknown);
        if (largesize)
            region.fields.push_back({"largesize", true});
        if (size < header) {
            region.note = "box size smaller than its header";
            break;
        }
        const bool past_eof = size > file_size - offset;
        if (past_eof) {
            b.warn(type == "mdat" ? LayoutWarningKind::truncated_data : LayoutWarningKind::chunk_past_eof, offset,
                   type + " declares " + std::to_string(size) + " bytes, " +
                       std::to_string(offset + size - file_size) + " past end of file");
        }
        if (type == "ftyp") {
            auto body = b.reader.read(offset + header, std::min<std::uint64_t>(size - header, 256u));
            if (body.size() >= 8u) {
                region.fields.push_back({"major_brand", fourcc(body.data())});
                region.fields.push_back({"minor_version", static_cast<std::uint64_t>(read_u32be(body.data() + 4u))});
                std::string brands;
                for (std::size_t at = 8u; at + 4u <= body.size(); at += 4u) {
                    if (!brands.empty()) brands += ',';
                    brands += fourcc(body.data() + at);
                }
                region.fields.push_back({"compatible_brands", brands});
                report.container_detail = "ftyp:" + fourcc(body.data());
            }
        } else if (type == "moov") {
            const auto body_size = size - header;
            auto body = b.reader.read(offset + header, std::min<std::uint64_t>(body_size, kMaxMoovRead));
            if (body_size > body.size())
                region.note = "moov read capped at " + std::to_string(body.size()) + " bytes; boxes past the cap are not listed";
            walk_boxes(b, facts, body, offset + header, 1u, "moov");
        } else if (type == "mdat") {
            region.note = largesize ? "sample data; 64-bit size" : "sample data";
        }
        if (past_eof)
            break;
        offset += size;
    }

    if (facts.handler == "soun" || facts.audio_track_done || facts.sample_rate != 0u) {
        LayoutStreamFacts stream;
        stream.sample_rate = facts.sample_rate != 0u ? facts.sample_rate
                                                     : static_cast<std::uint32_t>(facts.media_timescale);
        stream.channels = facts.channels;
        stream.encoding = facts.codec.empty() ? "unknown" : facts.codec;
        stream.frames = facts.stts_samples;
        if (stream.frames == 0u && facts.media_timescale != 0u && facts.sample_rate == facts.media_timescale)
            stream.frames = facts.media_duration;
        stream.duration_seconds = stream.sample_rate != 0u && stream.frames != 0u
            ? static_cast<double>(stream.frames) / stream.sample_rate
            : (facts.media_timescale != 0u ? static_cast<double>(facts.media_duration) / facts.media_timescale : 0.0);
        report.stream = stream;
    }
    // The chunk-offset table style matters for files past 4 GiB; name it on
    // the sample table that owns it.
    for (auto& region : report.regions) {
        if (region.id == "stbl") {
            region.fields.push_back({"chunk_offsets", std::string(facts.co64 ? "co64" : (facts.stco ? "stco" : "none"))});
            break;
        }
    }
}

} // namespace pulp::audio::layout_detail
