#include <pulp/runtime/zip.hpp>
#include <miniz.h>
#include <cstring>

namespace pulp::runtime {

// ── gzip (RFC 1952) header / trailer constants ─────────────────────────────
// Header layout: ID1 ID2 CM FLG MTIME(4) XFL OS [optional FEXTRA / FNAME /
// FCOMMENT / FHCRC] then DEFLATE stream then trailer CRC32(4) ISIZE(4).
namespace {
constexpr uint8_t kGzipID1 = 0x1f;
constexpr uint8_t kGzipID2 = 0x8b;
constexpr uint8_t kGzipCM_Deflate = 0x08;
constexpr uint8_t kGzipFTEXT = 0x01;     // unused but present in spec
constexpr uint8_t kGzipFHCRC = 0x02;
constexpr uint8_t kGzipFEXTRA = 0x04;
constexpr uint8_t kGzipFNAME = 0x08;
constexpr uint8_t kGzipFCOMMENT = 0x10;
constexpr size_t kGzipFixedHeaderSize = 10;
constexpr size_t kGzipTrailerSize = 8;

// Returns the total header length (including all optional fields), or
// std::nullopt if the input is not a valid RFC 1952 gzip header.
std::optional<size_t> parse_gzip_header(const uint8_t* data, size_t size) {
    if (size < kGzipFixedHeaderSize)
        return std::nullopt;
    if (data[0] != kGzipID1 || data[1] != kGzipID2)
        return std::nullopt;
    if (data[2] != kGzipCM_Deflate)
        return std::nullopt;
    const uint8_t flg = data[3];
    // Reserved bits (FRESERVED1..3 = top 3 bits) must be zero per RFC 1952.
    if (flg & 0xE0)
        return std::nullopt;
    size_t pos = kGzipFixedHeaderSize;
    if (flg & kGzipFEXTRA) {
        if (pos + 2 > size) return std::nullopt;
        size_t xlen = static_cast<size_t>(data[pos]) |
                      (static_cast<size_t>(data[pos + 1]) << 8);
        pos += 2;
        if (pos + xlen > size) return std::nullopt;
        pos += xlen;
    }
    if (flg & kGzipFNAME) {
        // Zero-terminated string.
        while (pos < size && data[pos] != 0) ++pos;
        if (pos >= size) return std::nullopt;
        ++pos;  // skip the NUL
    }
    if (flg & kGzipFCOMMENT) {
        while (pos < size && data[pos] != 0) ++pos;
        if (pos >= size) return std::nullopt;
        ++pos;
    }
    if (flg & kGzipFHCRC) {
        if (pos + 2 > size) return std::nullopt;
        pos += 2;  // skip CRC16; we don't verify it (spec says it's optional).
    }
    return pos;
}

// Decompress a raw DEFLATE stream of unknown decompressed size into a vector.
// Used for the gzip path (no zlib header) and as a building block.
std::optional<std::vector<uint8_t>> inflate_raw(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0)
        return std::nullopt;

    std::vector<uint8_t> result;
    // Heuristic initial size; grows as needed.
    result.resize(size > 0 ? size * 4 : 64);

    mz_stream stream{};
    stream.next_in = data;
    stream.avail_in = static_cast<unsigned int>(size);
    stream.next_out = result.data();
    stream.avail_out = static_cast<unsigned int>(result.size());

    if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK)
        return std::nullopt;

    for (int attempt = 0; attempt < 32; ++attempt) {
        int status = mz_inflate(&stream, MZ_NO_FLUSH);
        if (status == MZ_STREAM_END) {
            if (stream.total_in != size) {
                mz_inflateEnd(&stream);
                return std::nullopt;
            }
            result.resize(stream.total_out);
            mz_inflateEnd(&stream);
            return result;
        }
        if (status == MZ_BUF_ERROR || status == MZ_OK) {
            if (stream.avail_in == 0 && stream.avail_out > 0) {
                mz_inflateEnd(&stream);
                return std::nullopt;
            }
            size_t old_size = result.size();
            result.resize(old_size * 2);
            stream.next_out = result.data() + stream.total_out;
            stream.avail_out =
                static_cast<unsigned int>(result.size() - stream.total_out);
            continue;
        }
        mz_inflateEnd(&stream);
        return std::nullopt;
    }

    mz_inflateEnd(&stream);
    return std::nullopt;
}

// Like inflate_raw, but for a deflate stream that may be followed by trailing
// bytes (e.g. a gzip trailer + the next member's header). Reports how many
// input bytes were consumed up to and including MZ_STREAM_END so the caller
// can continue past the stream. Codex P2 on PR #747 — required for
// concatenated RFC 1952 gzip members.
std::optional<std::vector<uint8_t>> inflate_raw_consumed(
        const uint8_t* data, size_t size, size_t* consumed_in) {
    if (data == nullptr || size == 0)
        return std::nullopt;

    std::vector<uint8_t> result;
    result.resize(size > 0 ? size * 4 : 64);

    mz_stream stream{};
    stream.next_in = data;
    stream.avail_in = static_cast<unsigned int>(size);
    stream.next_out = result.data();
    stream.avail_out = static_cast<unsigned int>(result.size());

    if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK)
        return std::nullopt;

    // MZ_NO_FLUSH so the decoder reports MZ_STREAM_END the moment the
    // deflate stream ends, instead of treating any remaining input as
    // garbage and erroring out. total_in then names the boundary.
    for (int attempt = 0; attempt < 32; ++attempt) {
        int status = mz_inflate(&stream, MZ_NO_FLUSH);
        if (status == MZ_STREAM_END) {
            result.resize(stream.total_out);
            if (consumed_in) *consumed_in = stream.total_in;
            mz_inflateEnd(&stream);
            return result;
        }
        if (status == MZ_BUF_ERROR || status == MZ_OK) {
            if (stream.avail_in == 0 && stream.avail_out > 0) {
                mz_inflateEnd(&stream);
                return std::nullopt;
            }
            // Need more output room (or we ran out of input prematurely).
            // Doubling output is the right move for MZ_BUF_ERROR; if
            // avail_in is already 0 we'll loop here briefly and the
            // attempt counter will bound it.
            size_t old_size = result.size();
            result.resize(old_size * 2);
            stream.next_out = result.data() + stream.total_out;
            stream.avail_out =
                static_cast<unsigned int>(result.size() - stream.total_out);
            continue;
        }
        mz_inflateEnd(&stream);
        return std::nullopt;
    }

    mz_inflateEnd(&stream);
    return std::nullopt;
}
}  // namespace

std::optional<std::vector<uint8_t>> gzip_compress(const uint8_t* data, size_t size, int level) {
    // Build a real RFC 1952 gzip stream:
    //   header(10) || raw deflate || CRC32(4, little-endian) || ISIZE(4, little-endian)
    // ISIZE is the uncompressed size modulo 2^32.
    auto deflated = deflate_compress(data, size, level);
    if (!deflated)
        return std::nullopt;

    std::vector<uint8_t> out;
    out.reserve(kGzipFixedHeaderSize + deflated->size() + kGzipTrailerSize);
    // Fixed header — no FNAME/FCOMMENT/FEXTRA/FHCRC.
    out.push_back(kGzipID1);
    out.push_back(kGzipID2);
    out.push_back(kGzipCM_Deflate);
    out.push_back(0x00);                        // FLG = 0
    out.insert(out.end(), {0, 0, 0, 0});        // MTIME = 0
    out.push_back(0x00);                        // XFL = 0
    out.push_back(0xff);                        // OS = unknown (per RFC 1952)

    out.insert(out.end(), deflated->begin(), deflated->end());

    const uint32_t crc = static_cast<uint32_t>(
        mz_crc32(MZ_CRC32_INIT, data, size));
    const uint32_t isize = static_cast<uint32_t>(size & 0xFFFFFFFFu);
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>((crc >> (8 * i)) & 0xff));
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>((isize >> (8 * i)) & 0xff));
    return out;
}

std::optional<std::vector<uint8_t>> gzip_decompress(const uint8_t* data, size_t size) {
    // RFC 1952 gzip path — detect via magic bytes 0x1f 0x8b.
    //
    // Loop over concatenated members (Codex P2 on PR #747). RFC 1952 §2.2
    // permits a gzip file to contain multiple members back-to-back; tools
    // like `pigz`, `cat a.gz b.gz`, and gzip producers that split streams
    // emit them. Each member has its own header + deflate stream + 8-byte
    // trailer (CRC32 + ISIZE). The output is the concatenation of every
    // member's inflated payload.
    if (size >= 2 && data[0] == kGzipID1 && data[1] == kGzipID2) {
        std::vector<uint8_t> all;
        size_t pos = 0;
        while (pos < size && data[pos] == kGzipID1
                          && (pos + 1 < size) && data[pos + 1] == kGzipID2) {
            const uint8_t* member = data + pos;
            const size_t member_max = size - pos;

            auto header_len = parse_gzip_header(member, member_max);
            if (!header_len || *header_len + kGzipTrailerSize > member_max)
                return std::nullopt;

            // Deflate stream starts right after the header. Use the
            // consuming variant so we can find where this member's
            // deflate ends (and the trailer begins) — the deflate
            // payload's length isn't recorded anywhere in the header.
            size_t deflate_consumed = 0;
            const size_t deflate_max = member_max - *header_len - kGzipTrailerSize;
            auto inflated = inflate_raw_consumed(
                member + *header_len, deflate_max, &deflate_consumed);
            if (!inflated)
                return std::nullopt;

            // Trailer is exactly the 8 bytes immediately after the
            // consumed deflate stream.
            const uint8_t* trailer = member + *header_len + deflate_consumed;
            if (trailer + kGzipTrailerSize > data + size)
                return std::nullopt;

            const uint32_t expected_crc =
                static_cast<uint32_t>(trailer[0]) |
                (static_cast<uint32_t>(trailer[1]) << 8) |
                (static_cast<uint32_t>(trailer[2]) << 16) |
                (static_cast<uint32_t>(trailer[3]) << 24);
            const uint32_t expected_isize =
                static_cast<uint32_t>(trailer[4]) |
                (static_cast<uint32_t>(trailer[5]) << 8) |
                (static_cast<uint32_t>(trailer[6]) << 16) |
                (static_cast<uint32_t>(trailer[7]) << 24);
            const uint32_t actual_crc = static_cast<uint32_t>(
                mz_crc32(MZ_CRC32_INIT, inflated->data(), inflated->size()));
            const uint32_t actual_isize =
                static_cast<uint32_t>(inflated->size() & 0xFFFFFFFFu);
            if (actual_crc != expected_crc || actual_isize != expected_isize)
                return std::nullopt;

            all.insert(all.end(), inflated->begin(), inflated->end());
            pos += *header_len + deflate_consumed + kGzipTrailerSize;
        }
        // Stray trailing bytes that don't start another gzip member are
        // a structural error per RFC 1952 — the well-formed contract is
        // "concatenated complete members." Reject it rather than silently
        // returning a partial decode.
        if (pos != size) return std::nullopt;
        return all;
    }

    // Backward-compatibility zlib (RFC 1950) path — historical callers and
    // older test fixtures still pass zlib-wrapped data through this entry
    // point. Keep accepting it so existing call sites don't regress.
    mz_ulong decomp_size = static_cast<mz_ulong>(size > 0 ? size * 4 : 64);
    std::vector<uint8_t> result(decomp_size);

    for (int attempt = 0; attempt < 8; ++attempt) {
        mz_ulong this_size = decomp_size;
        int status = mz_uncompress(result.data(), &this_size, data,
                                   static_cast<mz_ulong>(size));
        if (status == MZ_OK) {
            result.resize(this_size);
            return result;
        }
        if (status == MZ_BUF_ERROR) {
            decomp_size *= 2;
            result.resize(decomp_size);
            continue;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> gzip_compress(std::string_view data, int level) {
    return gzip_compress(reinterpret_cast<const uint8_t*>(data.data()), data.size(), level);
}

std::optional<std::string> gzip_decompress_string(const uint8_t* data, size_t size) {
    auto result = gzip_decompress(data, size);
    if (!result) return std::nullopt;
    return std::string(result->begin(), result->end());
}

std::optional<std::vector<uint8_t>> deflate_compress(const uint8_t* data, size_t size, int level) {
    mz_ulong comp_size = mz_deflateBound(nullptr, static_cast<mz_ulong>(size));
    std::vector<uint8_t> result(comp_size);

    mz_stream stream{};
    stream.next_in = data;
    stream.avail_in = static_cast<unsigned int>(size);
    stream.next_out = result.data();
    stream.avail_out = static_cast<unsigned int>(result.size());

    if (mz_deflateInit2(&stream, level, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS,
                        9, MZ_DEFAULT_STRATEGY) != MZ_OK)
        return std::nullopt;

    int status = mz_deflate(&stream, MZ_FINISH);
    mz_deflateEnd(&stream);

    if (status != MZ_STREAM_END)
        return std::nullopt;

    result.resize(stream.total_out);
    return result;
}

std::optional<std::vector<uint8_t>> deflate_decompress(const uint8_t* data, size_t size) {
    return inflate_raw(data, size);
}

}  // namespace pulp::runtime
