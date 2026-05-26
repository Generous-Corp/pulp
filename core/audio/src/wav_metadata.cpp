// wav_metadata.cpp — implementation of WAV metadata round-trip.
//
// All numeric fields in RIFF chunks are little-endian regardless of
// host byte order, so we route everything through explicit byte-level
// helpers rather than memcpy + cast. Strings inside fixed-size bext
// fields are NUL-terminated when shorter than the slot and truncated
// (no NUL) when exactly slot-sized; reads strip trailing NULs.
//
// Layout reminders:
//   RIFF<u32 size>WAVE { <ckId u32><ckSize u32><payload [+1 pad byte if odd]> ...}
//   RF64 variant is recognised on read (size==0xFFFFFFFF) but we always
//   emit RIFF on write — the metadata round-trip is for files small
//   enough to round-trip in memory.

#include <pulp/audio/wav_metadata.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace pulp::audio {
namespace {

// ── Byte-level helpers (little-endian, host-agnostic) ───────────────────────

inline uint16_t rd_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t rd_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}
inline int16_t rd_i16(const uint8_t* p) {
    return static_cast<int16_t>(rd_u16(p));
}
inline float rd_f32(const uint8_t* p) {
    uint32_t bits = rd_u32(p);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

inline void wr_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
}
inline void wr_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}
inline void wr_i16(std::vector<uint8_t>& v, int16_t x) {
    wr_u16(v, static_cast<uint16_t>(x));
}
inline void wr_f32(std::vector<uint8_t>& v, float x) {
    uint32_t bits;
    std::memcpy(&bits, &x, sizeof(bits));
    wr_u32(v, bits);
}

// Pad/truncate `s` into exactly `slot` bytes (NUL-padded).
inline void append_fixed_string(std::vector<uint8_t>& out, const std::string& s, size_t slot) {
    size_t n = std::min(s.size(), slot);
    out.insert(out.end(), s.begin(), s.begin() + n);
    for (size_t i = n; i < slot; ++i)
        out.push_back(0);
}

// Read fixed-size string slot and trim trailing NULs.
inline std::string read_fixed_string(const uint8_t* p, size_t slot) {
    size_t n = slot;
    while (n > 0 && p[n - 1] == 0) --n;
    return std::string(reinterpret_cast<const char*>(p), n);
}

// FourCC literal helper — compares a 4-byte ID against a string literal.
inline bool fourcc_eq(const uint8_t* p, const char (&s)[5]) {
    return p[0] == static_cast<uint8_t>(s[0])
        && p[1] == static_cast<uint8_t>(s[1])
        && p[2] == static_cast<uint8_t>(s[2])
        && p[3] == static_cast<uint8_t>(s[3]);
}

// Minimal `fmt ` chunk for the silent-WAV builder used in tests.
std::vector<uint8_t> build_minimal_fmt(uint32_t sample_rate,
                                       uint16_t num_channels,
                                       uint16_t bits_per_sample) {
    std::vector<uint8_t> v;
    v.reserve(16);
    wr_u16(v, 1);                                          // PCM
    wr_u16(v, num_channels);
    wr_u32(v, sample_rate);
    const uint32_t block_align = (bits_per_sample / 8u) * num_channels;
    const uint32_t byte_rate = block_align * sample_rate;
    wr_u32(v, byte_rate);
    wr_u16(v, static_cast<uint16_t>(block_align));
    wr_u16(v, bits_per_sample);
    return v;
}

void append_chunk(std::vector<uint8_t>& out,
                  const char (&id)[5],
                  const std::vector<uint8_t>& payload) {
    out.push_back(static_cast<uint8_t>(id[0]));
    out.push_back(static_cast<uint8_t>(id[1]));
    out.push_back(static_cast<uint8_t>(id[2]));
    out.push_back(static_cast<uint8_t>(id[3]));
    wr_u32(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    if (payload.size() & 1u)
        out.push_back(0); // word-align
}

bool read_file_bytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    auto sz = in.tellg();
    if (sz <= 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    in.read(reinterpret_cast<char*>(out.data()), sz);
    return in.good();
}

bool write_file_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

} // namespace

// ── Chunk codecs ────────────────────────────────────────────────────────────

std::vector<uint8_t> encode_bext_chunk(const BwavMetadata& bwav) {
    // Fixed header up to coding_history is 602 bytes (bext v2 layout).
    std::vector<uint8_t> v;
    v.reserve(602 + bwav.coding_history.size());

    append_fixed_string(v, bwav.description, 256);
    append_fixed_string(v, bwav.originator, 32);
    append_fixed_string(v, bwav.originator_reference, 32);
    append_fixed_string(v, bwav.origination_date, 10);
    append_fixed_string(v, bwav.origination_time, 8);

    const uint32_t tr_low = static_cast<uint32_t>(bwav.time_reference & 0xFFFFFFFFu);
    const uint32_t tr_high = static_cast<uint32_t>((bwav.time_reference >> 32) & 0xFFFFFFFFu);
    wr_u32(v, tr_low);
    wr_u32(v, tr_high);
    wr_u16(v, bwav.version);

    v.insert(v.end(), bwav.umid.begin(), bwav.umid.end()); // 64 bytes

    wr_i16(v, bwav.loudness_value);
    wr_i16(v, bwav.loudness_range);
    wr_i16(v, bwav.max_true_peak_level);
    wr_i16(v, bwav.max_momentary_loudness);
    wr_i16(v, bwav.max_short_term_loudness);

    // Reserved: 180 bytes of zeros per EBU spec.
    v.insert(v.end(), 180, uint8_t{0});

    // Variable-length coding history (ASCII, terminated by \r\n typically).
    v.insert(v.end(), bwav.coding_history.begin(), bwav.coding_history.end());

    return v;
}

std::optional<BwavMetadata> decode_bext_chunk(const uint8_t* data, size_t size) {
    // Minimum bext payload is 602 bytes (fixed header before coding_history).
    if (size < 602) return std::nullopt;

    BwavMetadata b;
    b.description = read_fixed_string(data + 0, 256);
    b.originator = read_fixed_string(data + 256, 32);
    b.originator_reference = read_fixed_string(data + 288, 32);
    b.origination_date = read_fixed_string(data + 320, 10);
    b.origination_time = read_fixed_string(data + 330, 8);

    const uint32_t tr_low = rd_u32(data + 338);
    const uint32_t tr_high = rd_u32(data + 342);
    b.time_reference = (static_cast<uint64_t>(tr_high) << 32) | tr_low;
    b.version = rd_u16(data + 346);

    std::memcpy(b.umid.data(), data + 348, 64);

    b.loudness_value = rd_i16(data + 412);
    b.loudness_range = rd_i16(data + 414);
    b.max_true_peak_level = rd_i16(data + 416);
    b.max_momentary_loudness = rd_i16(data + 418);
    b.max_short_term_loudness = rd_i16(data + 420);
    // 180 bytes of reserved padding at offset 422.

    if (size > 602) {
        b.coding_history.assign(reinterpret_cast<const char*>(data + 602), size - 602);
        // Strip trailing NULs / padding.
        while (!b.coding_history.empty() && b.coding_history.back() == '\0')
            b.coding_history.pop_back();
    }
    return b;
}

std::vector<uint8_t> encode_acid_chunk(const AcidMetadata& acid) {
    std::vector<uint8_t> v;
    v.reserve(24);
    wr_u32(v, acid.flags);
    wr_u16(v, acid.root_note);
    wr_u16(v, acid.reserved1);
    wr_f32(v, acid.reserved2);
    wr_u32(v, acid.num_beats);
    wr_u16(v, acid.meter_denominator);
    wr_u16(v, acid.meter_numerator);
    wr_f32(v, acid.tempo_bpm);
    return v;
}

std::optional<AcidMetadata> decode_acid_chunk(const uint8_t* data, size_t size) {
    if (size < 24) return std::nullopt;
    AcidMetadata a;
    a.flags = rd_u32(data + 0);
    a.root_note = rd_u16(data + 4);
    a.reserved1 = rd_u16(data + 6);
    a.reserved2 = rd_f32(data + 8);
    a.num_beats = rd_u32(data + 12);
    a.meter_denominator = rd_u16(data + 16);
    a.meter_numerator = rd_u16(data + 18);
    a.tempo_bpm = rd_f32(data + 20);
    return a;
}

// ── RIFF walker / file API ──────────────────────────────────────────────────

namespace {

// Visit every chunk in a RIFF-WAVE byte stream. `visitor(id, payload, size)`.
template <class F>
bool walk_riff_chunks(const std::vector<uint8_t>& bytes, F&& visitor) {
    if (bytes.size() < 12) return false;
    if (!fourcc_eq(bytes.data(), "RIFF") && !fourcc_eq(bytes.data(), "RF64"))
        return false;
    if (!fourcc_eq(bytes.data() + 8, "WAVE")) return false;

    size_t off = 12;
    while (off + 8 <= bytes.size()) {
        const uint8_t* id = bytes.data() + off;
        uint32_t sz = rd_u32(bytes.data() + off + 4);
        const uint8_t* payload = bytes.data() + off + 8;
        // Guard against malformed sz that runs past EOF.
        if (sz > bytes.size() - (off + 8)) {
            sz = static_cast<uint32_t>(bytes.size() - (off + 8));
        }
        visitor(id, payload, static_cast<size_t>(sz));
        off += 8 + sz + (sz & 1u); // skip pad byte for odd-sized chunks
    }
    return true;
}

} // namespace

std::optional<WavMetadata> read_wav_metadata(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (!read_file_bytes(path, bytes)) return std::nullopt;

    WavMetadata meta;
    const bool ok = walk_riff_chunks(bytes, [&](const uint8_t* id, const uint8_t* payload, size_t sz) {
        if (fourcc_eq(id, "bext")) {
            if (auto b = decode_bext_chunk(payload, sz)) meta.bwav = std::move(*b);
        } else if (fourcc_eq(id, "acid")) {
            if (auto a = decode_acid_chunk(payload, sz)) meta.acid = std::move(*a);
        } else if (fourcc_eq(id, "iXML")) {
            meta.ixml = std::string(reinterpret_cast<const char*>(payload), sz);
        } else if (fourcc_eq(id, "axml")) {
            meta.axml = std::string(reinterpret_cast<const char*>(payload), sz);
        } else if (!fourcc_eq(id, "fmt ") && !fourcc_eq(id, "data") && !fourcc_eq(id, "JUNK")) {
            // Stash any other ancillary chunk so callers can preserve it.
            WavMetadata::UnknownChunk u;
            u.id = {{static_cast<char>(id[0]), static_cast<char>(id[1]),
                     static_cast<char>(id[2]), static_cast<char>(id[3])}};
            u.data.assign(payload, payload + sz);
            meta.unknown_chunks.push_back(std::move(u));
        }
    });
    if (!ok) return std::nullopt;
    return meta;
}

bool write_wav_metadata(const std::string& path,
                        const WavMetadata& metadata,
                        uint32_t sample_rate,
                        uint16_t num_channels,
                        uint16_t bits_per_sample) {
    // Build payload chunks first so we can compute the RIFF size.
    std::vector<uint8_t> body;
    body.reserve(1024);
    // "WAVE" form type.
    body.push_back('W'); body.push_back('A'); body.push_back('V'); body.push_back('E');

    append_chunk(body, "fmt ", build_minimal_fmt(sample_rate, num_channels, bits_per_sample));
    if (metadata.bwav) append_chunk(body, "bext", encode_bext_chunk(*metadata.bwav));
    if (metadata.ixml) {
        std::vector<uint8_t> p(metadata.ixml->begin(), metadata.ixml->end());
        append_chunk(body, "iXML", p);
    }
    if (metadata.axml) {
        std::vector<uint8_t> p(metadata.axml->begin(), metadata.axml->end());
        append_chunk(body, "axml", p);
    }
    if (metadata.acid) append_chunk(body, "acid", encode_acid_chunk(*metadata.acid));
    for (const auto& unk : metadata.unknown_chunks) {
        char id[5] = {unk.id[0], unk.id[1], unk.id[2], unk.id[3], '\0'};
        append_chunk(body, reinterpret_cast<const char (&)[5]>(id), unk.data);
    }
    // Empty `data` chunk so the file is structurally a valid WAV.
    append_chunk(body, "data", std::vector<uint8_t>{});

    std::vector<uint8_t> file;
    file.reserve(body.size() + 8);
    file.push_back('R'); file.push_back('I'); file.push_back('F'); file.push_back('F');
    wr_u32(file, static_cast<uint32_t>(body.size()));
    file.insert(file.end(), body.begin(), body.end());

    return write_file_bytes(path, file);
}

bool replace_metadata_in_file(const std::string& path, const WavMetadata& metadata) {
    std::vector<uint8_t> bytes;
    if (!read_file_bytes(path, bytes)) return false;
    if (bytes.size() < 12) return false;
    if (!fourcc_eq(bytes.data(), "RIFF") && !fourcc_eq(bytes.data(), "RF64"))
        return false;
    if (!fourcc_eq(bytes.data() + 8, "WAVE")) return false;

    // Collect original `fmt ` and `data` chunks verbatim; drop any
    // metadata chunk kind we know how to rewrite (bext/iXML/axml/acid)
    // plus any chunks present in the caller-supplied `unknown_chunks`.
    std::vector<uint8_t> body;
    body.reserve(bytes.size());
    body.push_back('W'); body.push_back('A'); body.push_back('V'); body.push_back('E');

    bool fmt_written = false;
    walk_riff_chunks(bytes, [&](const uint8_t* id, const uint8_t* payload, size_t sz) {
        if (fourcc_eq(id, "bext") || fourcc_eq(id, "iXML")
            || fourcc_eq(id, "axml") || fourcc_eq(id, "acid")) {
            return; // dropped; will be re-emitted from `metadata`
        }
        if (fourcc_eq(id, "fmt ")) {
            std::vector<uint8_t> p(payload, payload + sz);
            append_chunk(body, "fmt ", p);
            fmt_written = true;
            // Immediately after fmt is a good place to emit metadata
            // chunks so they precede `data` and survive permissive
            // readers that stop scanning past the audio payload.
            if (metadata.bwav) append_chunk(body, "bext", encode_bext_chunk(*metadata.bwav));
            if (metadata.ixml) {
                std::vector<uint8_t> ix(metadata.ixml->begin(), metadata.ixml->end());
                append_chunk(body, "iXML", ix);
            }
            if (metadata.axml) {
                std::vector<uint8_t> ax(metadata.axml->begin(), metadata.axml->end());
                append_chunk(body, "axml", ax);
            }
            if (metadata.acid) append_chunk(body, "acid", encode_acid_chunk(*metadata.acid));
            return;
        }
        // Preserve everything else (data, JUNK, LIST/INFO, etc.) verbatim.
        std::vector<uint8_t> p(payload, payload + sz);
        char id_buf[5] = {static_cast<char>(id[0]), static_cast<char>(id[1]),
                          static_cast<char>(id[2]), static_cast<char>(id[3]), '\0'};
        append_chunk(body, reinterpret_cast<const char (&)[5]>(id_buf), p);
    });
    if (!fmt_written) return false;

    std::vector<uint8_t> out;
    out.reserve(body.size() + 8);
    out.push_back('R'); out.push_back('I'); out.push_back('F'); out.push_back('F');
    wr_u32(out, static_cast<uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());

    return write_file_bytes(path, out);
}

} // namespace pulp::audio
