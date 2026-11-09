#include <pulp/audio/wave_layout.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace pulp::audio {
namespace {

constexpr std::uint32_t kSizeSentinel = 0xFFFFFFFFu;
constexpr std::uint64_t kChunkHeader = 8u;
constexpr std::uint64_t kDs64Fixed = 28u;
// Chunk headers visited before the walk gives up. Every real file stays far
// below this; it bounds the walk over a file of zero-sized chunks.
constexpr std::size_t kMaxChunks = 4096u;
// ds64 table entries kept for resolving sentinel-sized chunks other than data.
constexpr std::size_t kMaxDs64Entries = 16u;

constexpr std::uint16_t kTagAdpcm = 0x0002u;
constexpr std::uint16_t kTagImaAdpcm = 0x0011u;
constexpr std::uint16_t kTagExtensible = 0xFFFEu;

std::uint16_t u16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8u));
}

std::uint32_t u32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8u) |
           (static_cast<std::uint32_t>(p[2]) << 16u) | (static_cast<std::uint32_t>(p[3]) << 24u);
}

std::uint64_t u64(const std::uint8_t* p) noexcept {
    return static_cast<std::uint64_t>(u32(p)) | (static_cast<std::uint64_t>(u32(p + 4u)) << 32u);
}

bool is_id(const std::uint8_t* p, const char (&id)[5]) noexcept {
    return std::memcmp(p, id, 4u) == 0;
}

// The span cursor keeps the base pointer in `context` and the length in
// `size`, so it needs no storage of its own.
std::size_t read_span(const WaveByteCursor& cursor, std::uint64_t offset, std::uint8_t* out,
                      std::size_t count) noexcept {
    if (offset >= cursor.size)
        return 0u;
    const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(count, cursor.size - offset));
    std::memcpy(out, static_cast<const std::uint8_t*>(cursor.context) + offset, n);
    return n;
}

// Fields of a fmt payload the decoder interprets. Two fmt chunks agree when
// these agree; the byte rate and any bytes past the extension are ignored.
struct InterpretedFormat {
    std::uint16_t tag = 0u;
    std::uint16_t channels = 0u;
    std::uint32_t sample_rate = 0u;
    std::uint16_t block_align = 0u;
    std::uint16_t bits = 0u;
    bool extended = false;
    std::uint16_t valid_bits = 0u;
    std::uint32_t channel_mask = 0u;
    std::array<std::uint8_t, 16> sub_format{};

    bool operator==(const InterpretedFormat&) const = default;
};

InterpretedFormat interpret(std::span<const std::uint8_t> payload) noexcept {
    InterpretedFormat f;
    if (payload.size() < 16u)
        return f;
    f.tag = u16(payload.data());
    f.channels = u16(payload.data() + 2u);
    f.sample_rate = u32(payload.data() + 4u);
    f.block_align = u16(payload.data() + 12u);
    f.bits = u16(payload.data() + 14u);
    if (f.tag == kTagExtensible && payload.size() >= 40u) {
        f.extended = true;
        f.valid_bits = u16(payload.data() + 18u);
        f.channel_mask = u32(payload.data() + 20u);
        std::memcpy(f.sub_format.data(), payload.data() + 24u, 16u);
    }
    return f;
}

bool is_adpcm(std::uint16_t translated) noexcept {
    return translated == kTagAdpcm || translated == kTagImaAdpcm;
}

// Per-block header bytes an ADPCM block carries before its nibbles.
std::uint64_t adpcm_block_header(std::uint16_t translated, std::uint32_t channels) noexcept {
    return static_cast<std::uint64_t>(translated == kTagAdpcm ? 6u : 4u) * channels;
}

// Frames `blocks` whole ADPCM blocks decode to, by the decoder's own count
// (two samples per byte after each block's header; IMA blocks also carry one
// predictor sample per channel). Saturates instead of wrapping.
std::uint64_t adpcm_frames(std::uint16_t translated, std::uint32_t channels,
                           std::uint64_t block_align, std::uint64_t blocks) noexcept {
    const auto header = adpcm_block_header(translated, channels);
    const auto nibble_bytes = block_align - header; // block_align > header, checked by caller
    constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
    if (blocks != 0u && nibble_bytes * 2u > kMax / blocks)
        return kMax;
    auto samples = blocks * nibble_bytes * 2u;
    if (translated == kTagImaAdpcm) {
        if (blocks > kMax / channels || samples > kMax - blocks * channels)
            return kMax;
        samples += blocks * channels;
    }
    return samples / channels;
}

void warn(WaveLayout& layout, LayoutWarningKind kind, std::uint64_t offset, std::uint64_t declared,
          std::uint64_t actual) {
    layout.warnings.push_back(WaveWarning{kind, offset, declared, actual});
}

void put_id(CanonicalWaveHeader& h, const char (&id)[5]) noexcept {
    std::memcpy(h.bytes.data() + h.size, id, 4u);
    h.size += 4u;
}

void put_u32(CanonicalWaveHeader& h, std::uint32_t value) noexcept {
    for (unsigned shift = 0; shift < 32u; shift += 8u)
        h.bytes[h.size++] = static_cast<std::uint8_t>(value >> shift);
}

void put_u64(CanonicalWaveHeader& h, std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64u; shift += 8u)
        h.bytes[h.size++] = static_cast<std::uint8_t>(value >> shift);
}

void set_u32(CanonicalWaveHeader& h, std::size_t at, std::uint32_t value) noexcept {
    for (unsigned shift = 0; shift < 32u; shift += 8u)
        h.bytes[at + shift / 8u] = static_cast<std::uint8_t>(value >> shift);
}

void set_u64(CanonicalWaveHeader& h, std::size_t at, std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64u; shift += 8u)
        h.bytes[at + shift / 8u] = static_cast<std::uint8_t>(value >> shift);
}

} // namespace

WaveByteCursor WaveByteCursor::over(std::span<const std::uint8_t> bytes) noexcept {
    WaveByteCursor cursor;
    cursor.context = const_cast<std::uint8_t*>(bytes.data());
    cursor.size = bytes.size();
    cursor.read_at = read_span;
    return cursor;
}

std::size_t WaveByteCursor::read(std::uint64_t offset, std::uint8_t* out,
                                 std::size_t count) const noexcept {
    if (count == 0u || read_at == nullptr)
        return 0u;
    return read_at(*this, offset, out, count);
}

bool WaveLayout::has_warning(LayoutWarningKind kind) const noexcept {
    return std::any_of(warnings.begin(), warnings.end(),
                       [kind](const WaveWarning& w) { return w.kind == kind; });
}

const char* wave_refusal_reason(WaveRefusal refusal) noexcept {
    switch (refusal) {
    case WaveRefusal::none:
        return "";
    case WaveRefusal::not_wave:
        return "no RIFF, RF64 or BW64 WAVE header";
    case WaveRefusal::no_fmt:
        return "no fmt chunk";
    case WaveRefusal::fmt_too_short:
        return "fmt payload shorter than the 16-byte PCM header";
    case WaveRefusal::conflicting_fmt:
        return "a second fmt chunk contradicts the first";
    case WaveRefusal::conflicting_data_size:
        return "the data chunk's size field and its ds64 entry disagree";
    case WaveRefusal::no_data:
        return "no data chunk";
    case WaveRefusal::unsupported_format:
        return "the decoder has no reader for this fmt chunk";
    case WaveRefusal::zero_channels:
        return "zero channels";
    case WaveRefusal::zero_sample_rate:
        return "zero sample rate";
    case WaveRefusal::block_align_too_small:
        return "block align is smaller than the frame the fmt chunk describes";
    case WaveRefusal::no_whole_frame:
        return "the data chunk declares bytes but holds no whole frame";
    }
    return "";
}

WaveLayout walk_wave_layout(const WaveByteCursor& cursor) {
    WaveLayout layout;
    layout.file_size = cursor.size;
    const auto size = cursor.size;

    std::array<std::uint8_t, 12> head{};
    if (size < head.size() || cursor.read(0u, head.data(), head.size()) != head.size() ||
        !is_id(head.data() + 8u, "WAVE")) {
        layout.refusal = WaveRefusal::not_wave;
        return layout;
    }
    if (is_id(head.data(), "RIFF")) {
        layout.envelope = WaveEnvelope::riff;
    } else if (is_id(head.data(), "RF64")) {
        layout.envelope = WaveEnvelope::rf64;
    } else if (is_id(head.data(), "BW64")) {
        layout.envelope = WaveEnvelope::bw64;
    } else {
        layout.refusal = WaveRefusal::not_wave;
        return layout;
    }
    const auto riff32 = u32(head.data() + 4u);
    std::uint64_t offset = head.size();

    // RF64 and BW64 resolve their sentinel sizes through a ds64 chunk that
    // must come first.
    struct TableEntry {
        std::array<std::uint8_t, 4> id;
        std::uint64_t size;
    };
    std::array<TableEntry, kMaxDs64Entries> table{};
    std::size_t table_size = 0u;
    std::uint64_t ds64_riff_size = 0u;
    if (layout.envelope != WaveEnvelope::riff) {
        std::array<std::uint8_t, kChunkHeader + kDs64Fixed> ds64{};
        if (cursor.read(offset, ds64.data(), ds64.size()) == ds64.size() &&
            is_id(ds64.data(), "ds64") && u32(ds64.data() + 4u) >= kDs64Fixed) {
            const auto declared = u32(ds64.data() + 4u);
            layout.has_ds64 = true;
            ds64_riff_size = u64(ds64.data() + 8u);
            layout.ds64_data_size = u64(ds64.data() + 16u);
            layout.ds64_sample_count = u64(ds64.data() + 24u);
            const auto entries = u32(ds64.data() + 32u);
            const auto table_at = offset + kChunkHeader + kDs64Fixed;
            const auto table_room = (static_cast<std::uint64_t>(declared) - kDs64Fixed) / 12u;
            const auto wanted = std::min<std::uint64_t>({entries, table_room, kMaxDs64Entries});
            for (std::uint64_t i = 0; i < wanted; ++i) {
                std::array<std::uint8_t, 12> entry{};
                if (cursor.read(table_at + i * 12u, entry.data(), entry.size()) != entry.size())
                    break;
                std::memcpy(table[table_size].id.data(), entry.data(), 4u);
                table[table_size].size = u64(entry.data() + 4u);
                ++table_size;
            }
            offset += kChunkHeader + declared + (declared & 1u);
        } else {
            warn(layout, LayoutWarningKind::sentinel_size_without_ds64, 0u, riff32, 0u);
        }
    }

    layout.riff_size = (riff32 == kSizeSentinel && layout.has_ds64) ? ds64_riff_size : riff32;
    const auto declared_end = layout.riff_size + 8u; // wraps only when end_overflows
    const bool end_overflows = layout.riff_size > std::numeric_limits<std::uint64_t>::max() - 8u;
    if (end_overflows || declared_end != size)
        warn(layout, LayoutWarningKind::envelope_size_mismatch, 0u,
             end_overflows ? std::numeric_limits<std::uint64_t>::max() : declared_end, size);
    const auto envelope_end = end_overflows ? size : std::min(declared_end, size);

    InterpretedFormat first_format;
    std::size_t chunks = 0u;
    while (offset <= size && size - offset >= kChunkHeader) {
        // Past the declared envelope only while fmt or data is still missing:
        // a RIFF size too small is recovered, trailing bytes are not chunks.
        if (offset >= envelope_end && layout.has_fmt && layout.has_data)
            break;
        if (++chunks > kMaxChunks)
            break;
        std::array<std::uint8_t, kChunkHeader> header{};
        if (cursor.read(offset, header.data(), header.size()) != header.size())
            break;
        const auto size32 = u32(header.data() + 4u);
        const auto payload = offset + kChunkHeader;
        std::uint64_t declared = size32;
        const bool data = is_id(header.data(), "data");

        if (size32 == kSizeSentinel) {
            bool resolved = false;
            if (layout.has_ds64 && data) {
                declared = layout.ds64_data_size;
                resolved = true;
            } else if (layout.has_ds64) {
                for (std::size_t i = 0; i < table_size; ++i) {
                    if (std::memcmp(table[i].id.data(), header.data(), 4u) == 0) {
                        declared = table[i].size;
                        resolved = true;
                        break;
                    }
                }
            }
            if (!resolved)
                warn(layout, LayoutWarningKind::sentinel_size_without_ds64, offset, size32, 0u);
        } else if (data && layout.has_ds64 && layout.ds64_data_size != 0u &&
                   layout.ds64_data_size != declared && !layout.has_data) {
            // RF64 under 4 GiB may carry real 32-bit sizes; a ds64 entry
            // naming a different size contradicts it.
            warn(layout, LayoutWarningKind::chunk_overlap, offset, layout.ds64_data_size, declared);
            layout.refusal = WaveRefusal::conflicting_data_size;
        }

        const auto room = size - payload;
        const auto present = std::min(declared, room);
        const bool past_eof = declared > room;

        if (is_id(header.data(), "fmt ")) {
            std::array<std::uint8_t, WaveFormatChunk::kMaxPayload> bytes{};
            const auto wanted = static_cast<std::size_t>(
                std::min<std::uint64_t>(present, WaveFormatChunk::kMaxPayload));
            const auto got = cursor.read(payload, bytes.data(), wanted);
            const auto fields = interpret({bytes.data(), got});
            if (!layout.has_fmt) {
                layout.has_fmt = true;
                layout.fmt.offset = offset;
                layout.fmt.declared = declared;
                layout.fmt.payload = bytes;
                layout.fmt.payload_size = static_cast<std::uint32_t>(got);
                first_format = fields;
                if (layout.has_data)
                    warn(layout, LayoutWarningKind::fmt_after_data, offset, 0u, 0u);
                if (got < 16u && layout.refusal == WaveRefusal::none)
                    layout.refusal = WaveRefusal::fmt_too_short;
            } else {
                warn(layout, LayoutWarningKind::duplicate_fmt, offset, declared, got);
                if ((got < 16u || !(fields == first_format)) && layout.refusal == WaveRefusal::none)
                    layout.refusal = WaveRefusal::conflicting_fmt;
            }
        } else if (data) {
            if (!layout.has_data) {
                layout.has_data = true;
                layout.data_header_offset = offset;
                layout.data_offset = payload;
                layout.data_declared = declared;
                layout.data_present = present;
                if (past_eof)
                    warn(layout, LayoutWarningKind::truncated_data, offset, declared, present);
            }
        } else if (is_id(header.data(), "fact")) {
            std::array<std::uint8_t, 4> count{};
            if (!layout.fact_sample_count && declared >= 4u &&
                cursor.read(payload, count.data(), count.size()) == count.size())
                layout.fact_sample_count = u32(count.data());
        }

        if (past_eof) {
            if (!data)
                warn(layout, LayoutWarningKind::chunk_past_eof, offset, declared, present);
            break;
        }
        const auto end = payload + declared;
        if ((declared & 1u) != 0u && end == size) {
            warn(layout, LayoutWarningKind::missing_pad_byte, offset, declared, present);
            break;
        }
        offset = end + (declared & 1u);
    }

    if (layout.refusal == WaveRefusal::none) {
        if (!layout.has_fmt)
            layout.refusal = WaveRefusal::no_fmt;
        else if (!layout.has_data)
            layout.refusal = WaveRefusal::no_data;
    }
    return layout;
}

WaveDecision decide_wave(const WaveLayout& layout,
                         const std::optional<WavFormatFacts>& facts) noexcept {
    WaveDecision decision;
    if (layout.refusal != WaveRefusal::none) {
        decision.refusal = layout.refusal;
        return decision;
    }
    if (!facts) {
        decision.refusal = WaveRefusal::unsupported_format;
        return decision;
    }
    if (facts->channels == 0u) {
        decision.refusal = WaveRefusal::zero_channels;
        return decision;
    }
    if (facts->sample_rate == 0u) {
        decision.refusal = WaveRefusal::zero_sample_rate;
        return decision;
    }

    const auto fmt = interpret(layout.fmt.bytes());
    const auto translated = facts->translated_format_tag;
    const std::uint64_t block_align = fmt.block_align;
    if (is_adpcm(translated)) {
        if (block_align <= adpcm_block_header(translated, facts->channels)) {
            decision.refusal = WaveRefusal::block_align_too_small;
            return decision;
        }
        const auto whole_blocks = layout.data_present / block_align;
        const auto declared_blocks =
            layout.data_declared / block_align + (layout.data_declared % block_align != 0u);
        decision.recoverable_frames =
            adpcm_frames(translated, facts->channels, block_align, whole_blocks);
        decision.recoverable_data_bytes = whole_blocks * block_align;
        decision.declared_frames =
            adpcm_frames(translated, facts->channels, block_align, declared_blocks);
    } else {
        const std::uint64_t sample_bytes = (static_cast<std::uint64_t>(fmt.bits) + 7u) / 8u;
        const auto frame = facts->bytes_per_frame;
        if (block_align < sample_bytes * facts->channels || frame == 0u) {
            decision.refusal = WaveRefusal::block_align_too_small;
            return decision;
        }
        decision.recoverable_frames = layout.data_present / frame;
        decision.recoverable_data_bytes = decision.recoverable_frames * frame;
        decision.declared_frames =
            layout.data_declared / frame + (layout.data_declared % frame != 0u);
    }
    decision.truncated = decision.recoverable_frames < decision.declared_frames;
    // Zero frames is a valid empty file only when the data chunk declares
    // zero bytes; bytes with no whole frame among them are refused.
    if (decision.recoverable_frames == 0u && layout.data_declared != 0u) {
        decision.refusal = WaveRefusal::no_whole_frame;
        decision.recoverable_data_bytes = 0u;
    }
    return decision;
}

CanonicalWaveHeader canonical_wave_header(std::span<const std::uint8_t> fmt_payload,
                                          std::uint64_t data_bytes) noexcept {
    CanonicalWaveHeader h;
    const bool extensible = fmt_payload.size() >= 40u && u16(fmt_payload.data()) == kTagExtensible;
    const std::size_t fmt_size = extensible ? 40u : std::min<std::size_t>(fmt_payload.size(), 16u);
    // Bytes after the size field up to the data payload, without ds64.
    const std::uint64_t riff_header = 4u + kChunkHeader + fmt_size + kChunkHeader;
    const auto pad = data_bytes & 1u;
    const bool rf64 =
        data_bytes > static_cast<std::uint64_t>(kSizeSentinel) - 1u - riff_header - pad;

    put_id(h, rf64 ? "RF64" : "RIFF");
    const auto riff_slot = h.size;
    put_u32(h, 0u);
    put_id(h, "WAVE");
    std::size_t ds64_riff_slot = 0u;
    if (rf64) {
        put_id(h, "ds64");
        put_u32(h, static_cast<std::uint32_t>(kDs64Fixed));
        ds64_riff_slot = h.size;
        put_u64(h, 0u);
        put_u64(h, data_bytes);
        put_u64(h, 0u); // no sample count: the decoder derives frames from the data size
        put_u32(h, 0u);
    }
    put_id(h, "fmt ");
    put_u32(h, static_cast<std::uint32_t>(fmt_size));
    std::memcpy(h.bytes.data() + h.size, fmt_payload.data(), fmt_size);
    h.size += fmt_size;
    put_id(h, "data");
    put_u32(h, rf64 ? kSizeSentinel : static_cast<std::uint32_t>(data_bytes));

    const auto riff_size = static_cast<std::uint64_t>(h.size) - 8u + data_bytes + pad;
    if (rf64) {
        set_u32(h, riff_slot, kSizeSentinel);
        set_u64(h, ds64_riff_slot, riff_size);
    } else {
        set_u32(h, riff_slot, static_cast<std::uint32_t>(riff_size));
    }
    return h;
}

CanonicalWaveHeader canonical_wave_header(const WaveLayout& layout,
                                          std::uint64_t data_bytes) noexcept {
    return canonical_wave_header(layout.fmt.bytes(), data_bytes);
}

} // namespace pulp::audio
