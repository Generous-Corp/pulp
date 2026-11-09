// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

// FORM/AIFF and FORM/AIFC layout walk. The chunk table comes from a
// tolerant walk; the stream facts and the decoder's verdict come from the
// reader's own `parse_aiff_layout`, served through a streambuf over the
// bounded byte source so the two cannot drift apart.

#include "audio_file_layout_detail.hpp"

#include "aiff_parser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <istream>
#include <limits>
#include <streambuf>

namespace pulp::audio::layout_detail {
namespace {

/// Seekable read-only streambuf over a LayoutByteSource. Each underflow
/// pulls one small block so the reader's own bounded access pattern is
/// preserved end to end.
class SourceStreambuf final : public std::streambuf {
  public:
    explicit SourceStreambuf(Reader& reader) noexcept : reader_(reader) {}

  protected:
    int_type underflow() override {
        if (position_ >= reader_.size())
            return traits_type::eof();
        auto block = reader_.read(position_, buffer_.size());
        if (block.empty())
            return traits_type::eof();
        std::memcpy(buffer_.data(), block.data(), block.size());
        block_start_ = position_;
        setg(buffer_.data(), buffer_.data(), buffer_.data() + block.size());
        return traits_type::to_int_type(buffer_[0]);
    }

    pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode) override {
        std::int64_t base = 0;
        if (dir == std::ios_base::cur)
            base = static_cast<std::int64_t>(current());
        else if (dir == std::ios_base::end)
            base = static_cast<std::int64_t>(reader_.size());
        const auto target = base + static_cast<std::int64_t>(off);
        if (target < 0)
            return pos_type(off_type(-1));
        return seek_to(static_cast<std::uint64_t>(target));
    }

    pos_type seekpos(pos_type pos, std::ios_base::openmode) override {
        return seek_to(static_cast<std::uint64_t>(off_type(pos)));
    }

  private:
    std::uint64_t current() const noexcept {
        return gptr() != nullptr ? block_start_ + static_cast<std::uint64_t>(gptr() - eback())
                                 : position_;
    }

    pos_type seek_to(std::uint64_t target) {
        position_ = target;
        setg(nullptr, nullptr, nullptr);
        return pos_type(static_cast<off_type>(target));
    }

    Reader& reader_;
    // The reader asks for at most 22 bytes per visit; a 64-byte block keeps
    // each seek-and-read within one request without over-fetching.
    std::array<char, 64> buffer_{};
    std::uint64_t block_start_ = 0u;
    std::uint64_t position_ = 0u;
};

double extended_to_double(const std::uint8_t* p) noexcept {
    const auto exponent = static_cast<int>(((p[0] & 0x7fu) << 8u) | p[1]) - 16383;
    const auto mantissa = read_u64be(p + 2u);
    if (mantissa == 0u)
        return 0.0;
    const double value = std::ldexp(static_cast<double>(mantissa), exponent - 63);
    return (p[0] & 0x80u) != 0u ? -value : value;
}

// Encoding vocabulary for an AIFF-C compression type. 'twos' and 'sowt' are
// plain PCM in either byte order, and the reader decodes both; 'in24'/'in32'
// are PCM spellings it does not.
std::string aifc_encoding(const std::string& compression_type) {
    if (compression_type.empty() || compression_type == "NONE" || compression_type == "twos" ||
        compression_type == "sowt" || compression_type == "in24" || compression_type == "in32")
        return "pcm";
    if (compression_type == "fl32" || compression_type == "fl64" || compression_type == "FL32" ||
        compression_type == "FL64")
        return "float";
    if (compression_type == "ulaw" || compression_type == "ULAW")
        return "mulaw";
    if (compression_type == "alaw" || compression_type == "ALAW")
        return "alaw";
    std::string lowered = compression_type;
    for (auto& c : lowered)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!lowered.empty() && lowered.back() == ' ')
        lowered.pop_back();
    return lowered;
}

LayoutRegionKind classify(std::string_view id) {
    static constexpr std::array<std::string_view, 14> kMetadata{
        "NAME", "AUTH", "(c) ", "ANNO", "COMT", "MARK", "INST",
        "MIDI", "AESD", "APPL", "ID3 ", "CHAN", "basc", "trns"};
    if (id == "COMM" || id == "FVER")
        return LayoutRegionKind::fmt;
    if (id == "SSND")
        return LayoutRegionKind::data;
    if (id == "FLLR")
        return LayoutRegionKind::padding;
    if (std::find(kMetadata.begin(), kMetadata.end(), id) != kMetadata.end())
        return LayoutRegionKind::metadata;
    return LayoutRegionKind::unknown;
}

struct Walk {
    bool comm_seen = false;
    bool ssnd_seen = false;
    std::string compression_type;
};

void parse_comm(Builder& b, LayoutRegion& region, Walk& walk, bool aifc, const IffChunk& chunk) {
    if (walk.comm_seen)
        b.warn(LayoutWarningKind::duplicate_fmt, chunk.offset,
               "second COMM chunk; the reader uses the first");
    if (walk.ssnd_seen && !walk.comm_seen)
        b.warn(LayoutWarningKind::fmt_after_data, chunk.offset,
               "COMM chunk follows the SSND chunk");
    walk.comm_seen = true;
    auto payload = b.reader.read_capped(chunk.payload_offset, chunk.declared, 256u);
    if (payload.size() < 18u) {
        region.note = "COMM payload shorter than 18 bytes";
        return;
    }
    region.fields.push_back({"channels", static_cast<std::uint64_t>(read_u16be(payload.data()))});
    region.fields.push_back(
        {"frames", static_cast<std::uint64_t>(read_u32be(payload.data() + 2u))});
    region.fields.push_back(
        {"bits_per_sample", static_cast<std::uint64_t>(read_u16be(payload.data() + 6u))});
    const double rate = extended_to_double(payload.data() + 8u);
    if (std::isfinite(rate))
        region.fields.push_back({"sample_rate", rate});
    else
        region.note = "COMM sample rate is not a finite number";
    if (aifc && payload.size() >= 22u) {
        if (walk.compression_type.empty())
            walk.compression_type = fourcc(payload.data() + 18u);
        region.fields.push_back({"compression_type", fourcc(payload.data() + 18u)});
        if (payload.size() > 22u) {
            const auto name_length = std::min<std::size_t>(payload[22], payload.size() - 23u);
            // The pstring is free text from the file, not a spec'd token.
            b.text("COMM", "compression_name", trimmed_text(payload.data() + 23u, name_length));
        }
    }
}

} // namespace

void inspect_aiff_layout(Builder& b) {
    auto& report = b.report;
    const auto file_size = b.reader.size();
    const bool aifc = report.container == AudioContainer::aifc;
    const auto head = b.reader.read(0u, 12u);
    if (head.size() < 12u) {
        report.error = "header shorter than a FORM envelope";
        return;
    }
    const auto form_size = static_cast<std::uint64_t>(read_u32be(head.data() + 4u));
    IffEnvelope env;
    env.declared_end = form_size + 8u;
    env.data_id = "SSND";
    auto& envelope = b.add_region(0u, 8u, form_size, "FORM", LayoutRegionKind::envelope);
    envelope.fields.push_back({"form_type", fourcc(head.data() + 8u)});
    envelope.fields.push_back({"form_size", form_size});
    if (env.declared_end != file_size) {
        b.warn(LayoutWarningKind::envelope_size_mismatch, 0u,
               "FORM declares " + std::to_string(env.declared_end) + " bytes, file holds " +
                   std::to_string(file_size));
    }
    env.walk_end = env.declared_end >= 12u ? std::min(env.declared_end, file_size) : file_size;

    Walk walk;
    IffHooks hooks;
    hooks.read_header = [&](std::uint64_t at) -> std::optional<IffChunk> {
        std::array<std::uint8_t, 8> raw{};
        if (!b.reader.read_exact(at, raw))
            return std::nullopt;
        IffChunk chunk;
        chunk.id = fourcc(raw.data());
        chunk.printable = raw_id_printable(raw.data(), 4u);
        chunk.declared = read_u32be(raw.data() + 4u);
        return chunk;
    };
    hooks.classify = classify;
    hooks.on_chunk = [&](const IffChunk& chunk, LayoutRegion& region) {
        const auto& id = chunk.id;
        if (id == "COMM") {
            parse_comm(b, region, walk, aifc, chunk);
        } else if (id == "SSND") {
            if (!walk.ssnd_seen)
                walk.ssnd_seen = true;
            else
                region.note = "second SSND chunk; the reader uses the first";
            auto payload = b.reader.read_capped(chunk.payload_offset, chunk.declared, 8u);
            if (payload.size() == 8u) {
                region.fields.push_back(
                    {"offset", static_cast<std::uint64_t>(read_u32be(payload.data()))});
                region.fields.push_back(
                    {"block_size", static_cast<std::uint64_t>(read_u32be(payload.data() + 4u))});
            }
        } else if (id == "FVER") {
            auto payload = b.reader.read_capped(chunk.payload_offset, chunk.declared, 4u);
            if (payload.size() == 4u)
                region.fields.push_back(
                    {"timestamp", static_cast<std::uint64_t>(read_u32be(payload.data()))});
        } else if (id == "NAME" || id == "AUTH" || id == "(c) " || id == "ANNO") {
            b.read_text(id, "text", chunk.payload_offset, chunk.declared);
        } else if (id == "MARK" || id == "COMT") {
            auto payload = b.reader.read_capped(chunk.payload_offset, chunk.declared, 2u);
            if (payload.size() == 2u)
                region.fields.push_back(
                    {"count", static_cast<std::uint64_t>(read_u16be(payload.data()))});
        } else if (id == "ID3 ") {
            region.note = "ID3v2 tag embedded in AIFF";
        }
    };
    walk_iff_chunks(b, env, 12u, hooks);

    // The reader's own interpretation, through the same bounded source.
    SourceStreambuf streambuf(b.reader);
    std::istream stream(&streambuf);
    const auto layout = detail::parse_aiff_layout(stream, file_size, false);
    const auto pcm = detail::parse_aiff_layout(stream, file_size, true);
    report.facts_confidence = FactsConfidence::parsed;
    if (layout) {
        LayoutStreamFacts stream_facts;
        stream_facts.sample_rate = layout->info.sample_rate;
        stream_facts.channels = layout->info.num_channels;
        stream_facts.bits_per_sample = layout->info.bits_per_sample;
        stream_facts.valid_bits = layout->info.bits_per_sample;
        stream_facts.frames = layout->info.num_frames;
        stream_facts.duration_seconds = layout->info.duration_seconds;
        stream_facts.encoding = aifc_encoding(walk.compression_type);
        stream_facts.bytes_per_frame = pcm ? pcm->bytes_per_frame : 0u;
        report.stream = stream_facts;
        b.set_decode_limits(layout->info.num_frames, layout->info.num_channels);
    }
    LayoutDecodeVerdict verdict;
    if (pcm) {
        verdict.decodable = true;
    } else if (!layout) {
        verdict.reason = "AIFF reader rejects the chunk layout";
    } else if (!walk.ssnd_seen) {
        verdict.reason = "no SSND chunk";
    } else if (aifc && walk.compression_type != "NONE" && walk.compression_type != "twos" &&
               walk.compression_type != "sowt") {
        // The reader decodes the three plain-PCM spellings; every other
        // compression type is a codec it does not carry.
        verdict.reason = "AIFF-C compression type '" + walk.compression_type +
                         "' is not plain PCM (NONE, twos or sowt)";
    } else {
        verdict.reason = "unsupported PCM width or SSND shorter than the declared frames";
    }
    report.decode = verdict;
}

} // namespace pulp::audio::layout_detail
