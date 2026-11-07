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
// plain PCM in either byte order; the in-memory reader still admits only NONE.
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
    const auto declared_end = form_size + 8u;
    auto& envelope = b.add_region(0u, 8u, form_size, "FORM", LayoutRegionKind::envelope);
    envelope.fields.push_back({"form_type", fourcc(head.data() + 8u)});
    envelope.fields.push_back({"form_size", form_size});
    if (declared_end != file_size) {
        b.warn(LayoutWarningKind::envelope_size_mismatch, 0u,
               "FORM declares " + std::to_string(declared_end) + " bytes, file holds " +
                   std::to_string(file_size));
    }
    const auto walk_end = declared_end >= 12u ? std::min(declared_end, file_size) : file_size;

    bool comm_seen = false;
    bool ssnd_seen = false;
    std::string compression_type;
    std::uint64_t offset = 12u;
    while (offset < walk_end && !report.regions_truncated) {
        if (walk_end - offset < 8u) {
            auto& stub = b.add_region(offset, 0u, walk_end - offset, "", LayoutRegionKind::unknown);
            stub.note = "shorter than a chunk header";
            break;
        }
        std::array<std::uint8_t, 8> raw{};
        if (!b.reader.read_exact(offset, raw))
            break;
        const auto id = fourcc(raw.data());
        const bool printable = raw_id_printable(raw.data(), 4u);
        const auto declared = static_cast<std::uint64_t>(read_u32be(raw.data() + 4u));
        const auto payload_offset = offset + 8u;
        auto& region = b.add_region(offset, 8u, declared, id,
                                    printable ? classify(id) : LayoutRegionKind::unknown);
        if (!printable)
            region.note =
                "chunk id is not printable ASCII; the walk may be out of step with the file";

        const bool past_eof = payload_offset > file_size || declared > file_size - payload_offset;
        const auto chunk_end = past_eof ? file_size : payload_offset + declared;
        if (past_eof) {
            const auto missing = payload_offset + declared - file_size;
            b.warn(id == "SSND" ? LayoutWarningKind::truncated_data
                                : LayoutWarningKind::chunk_past_eof,
                   offset,
                   id + " declares " + std::to_string(declared) + " bytes, " +
                       std::to_string(missing) + " past end of file");
        }
        std::uint64_t pad = declared & 1u;
        if (pad != 0u && !past_eof && chunk_end >= file_size) {
            b.warn(LayoutWarningKind::missing_pad_byte, offset,
                   id + " has odd size " + std::to_string(declared) +
                       " and ends the file without a pad byte");
            pad = 0u;
        }

        if (id == "COMM") {
            if (comm_seen)
                b.warn(LayoutWarningKind::duplicate_fmt, offset,
                       "second COMM chunk; the reader uses the first");
            if (ssnd_seen && !comm_seen)
                b.warn(LayoutWarningKind::fmt_after_data, offset,
                       "COMM chunk follows the SSND chunk");
            comm_seen = true;
            auto payload = b.reader.read(payload_offset, std::min<std::uint64_t>(declared, 256u));
            if (payload.size() >= 18u) {
                region.fields.push_back(
                    {"channels", static_cast<std::uint64_t>(read_u16be(payload.data()))});
                region.fields.push_back(
                    {"frames", static_cast<std::uint64_t>(read_u32be(payload.data() + 2u))});
                region.fields.push_back({"bits_per_sample", static_cast<std::uint64_t>(
                                                                read_u16be(payload.data() + 6u))});
                const double rate = extended_to_double(payload.data() + 8u);
                if (std::isfinite(rate))
                    region.fields.push_back({"sample_rate", rate});
                else
                    region.note = "COMM sample rate is not a finite number";
                if (aifc && payload.size() >= 22u) {
                    if (compression_type.empty())
                        compression_type = fourcc(payload.data() + 18u);
                    region.fields.push_back({"compression_type", fourcc(payload.data() + 18u)});
                    if (payload.size() > 22u) {
                        const auto name_length =
                            std::min<std::size_t>(payload[22], payload.size() - 23u);
                        // The pstring is free text from the file, not a spec'd token.
                        b.text("COMM", "compression_name",
                               trimmed_text(payload.data() + 23u, name_length));
                    }
                }
            } else {
                region.note = "COMM payload shorter than 18 bytes";
            }
        } else if (id == "SSND") {
            if (!ssnd_seen) {
                ssnd_seen = true;
            } else {
                region.note = "second SSND chunk; the reader uses the first";
            }
            auto payload = b.reader.read(payload_offset, std::min<std::uint64_t>(declared, 8u));
            if (payload.size() == 8u) {
                region.fields.push_back(
                    {"offset", static_cast<std::uint64_t>(read_u32be(payload.data()))});
                region.fields.push_back(
                    {"block_size", static_cast<std::uint64_t>(read_u32be(payload.data() + 4u))});
            }
        } else if (id == "FVER") {
            auto payload = b.reader.read(payload_offset, std::min<std::uint64_t>(declared, 4u));
            if (payload.size() == 4u)
                region.fields.push_back(
                    {"timestamp", static_cast<std::uint64_t>(read_u32be(payload.data()))});
        } else if (region.kind == LayoutRegionKind::metadata) {
            b.present(id, offset, declared);
            if (id == "NAME" || id == "AUTH" || id == "(c) " || id == "ANNO") {
                if (b.options.include_metadata_text) {
                    auto payload = b.reader.read(
                        payload_offset, std::min<std::uint64_t>(declared, kMaxPayloadRead));
                    b.text(id, "text", trimmed_text(payload.data(), payload.size()));
                }
            } else if (id == "MARK" || id == "COMT") {
                auto payload = b.reader.read(payload_offset, std::min<std::uint64_t>(declared, 2u));
                if (payload.size() == 2u)
                    region.fields.push_back(
                        {"count", static_cast<std::uint64_t>(read_u16be(payload.data()))});
            } else if (id == "ID3 ") {
                region.note = "ID3v2 tag embedded in AIFF";
            }
        }

        if (past_eof)
            break;
        offset = chunk_end + pad;
    }
    if (declared_end > 12u && declared_end < file_size) {
        auto& trailing =
            b.add_region(declared_end, 0u, file_size - declared_end, "", LayoutRegionKind::unknown);
        trailing.note = "bytes after the declared envelope";
    }

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
        stream_facts.encoding = aifc_encoding(compression_type);
        stream_facts.bytes_per_frame = pcm ? pcm->bytes_per_frame : 0u;
        report.stream = stream_facts;
        b.set_decode_limits(layout->info.num_frames, layout->info.num_channels);
    }
    LayoutDecodeVerdict verdict;
    if (pcm) {
        verdict.decodable = true;
    } else if (!layout) {
        verdict.reason = "AIFF reader rejects the chunk layout";
    } else if (!ssnd_seen) {
        verdict.reason = "no SSND chunk";
    } else if (layout->info.num_frames == 0u) {
        verdict.reason = "zero frames";
    } else if (aifc && compression_type != "NONE") {
        // The reader admits only the NONE compression type; 'twos' and 'sowt'
        // name plain PCM too but are refused as written.
        verdict.reason = "AIFF-C compression type '" + compression_type + "' is not NONE";
    } else {
        verdict.reason = "unsupported PCM width or SSND shorter than the declared frames";
    }
    report.decode = verdict;
}

} // namespace pulp::audio::layout_detail
