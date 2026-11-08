#pragma once

// Byte-level builders for the container fixtures the layout-report tests
// hand-assemble: big-endian fields, ISO base media boxes, IFF chunks, Ogg
// pages and Vorbis comment bodies. Every fixture is built in code so a test
// can name the exact byte it corrupts.

#include "wav_fixture_builders.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace pulp_test::container {

using pulp_test::wav::append_u16;
using pulp_test::wav::append_u32;
using pulp_test::wav::append_u64;
using pulp_test::wav::Bytes;
// Big-endian helpers for the hand-built MP4, FLAC and ID3 fixtures below.
inline void put_be16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8u));
    b.push_back(static_cast<std::uint8_t>(v));
}
inline void put_be32(Bytes& b, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
        b.push_back(static_cast<std::uint8_t>(v >> shift));
}
inline void put_be64(Bytes& b, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8)
        b.push_back(static_cast<std::uint8_t>(v >> shift));
}
inline void put_text(Bytes& b, std::string_view text) {
    for (char c : text)
        b.push_back(static_cast<std::uint8_t>(c));
}
inline Bytes box(std::string_view type, const Bytes& body) {
    Bytes b;
    put_be32(b, static_cast<std::uint32_t>(8u + body.size()));
    put_text(b, type);
    b.insert(b.end(), body.begin(), body.end());
    return b;
}
// IFF chunk (AIFF): id first, then a big-endian payload size, then the pad.
inline Bytes iff_chunk(std::string_view id, const Bytes& body) {
    Bytes b;
    put_text(b, id);
    put_be32(b, static_cast<std::uint32_t>(body.size()));
    b.insert(b.end(), body.begin(), body.end());
    if ((body.size() & 1u) != 0u)
        b.push_back(0u);
    return b;
}
inline Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& part : parts)
        out.insert(out.end(), part.begin(), part.end());
    return out;
}

// One Ogg page: header type flags, granule, serial, sequence, a single
// segment table entry per 255 bytes of body. The CRC is left zero; the walk
// does not check it.
inline Bytes ogg_page(std::uint8_t header_type, std::uint64_t granule, std::uint32_t serial,
                      std::uint32_t sequence, const Bytes& body) {
    Bytes b;
    put_text(b, "OggS");
    b.push_back(0u);
    b.push_back(header_type);
    append_u64(b, granule);
    append_u32(b, serial);
    append_u32(b, sequence);
    append_u32(b, 0u);
    std::vector<std::uint8_t> lacing;
    std::size_t remaining = body.size();
    while (remaining >= 255u) {
        lacing.push_back(255u);
        remaining -= 255u;
    }
    lacing.push_back(static_cast<std::uint8_t>(remaining));
    b.push_back(static_cast<std::uint8_t>(lacing.size()));
    b.insert(b.end(), lacing.begin(), lacing.end());
    b.insert(b.end(), body.begin(), body.end());
    return b;
}

inline Bytes vorbis_comment_body(std::string_view vendor,
                                 std::initializer_list<std::string_view> entries) {
    Bytes b;
    append_u32(b, static_cast<std::uint32_t>(vendor.size()));
    put_text(b, vendor);
    append_u32(b, static_cast<std::uint32_t>(entries.size()));
    for (auto entry : entries) {
        append_u32(b, static_cast<std::uint32_t>(entry.size()));
        put_text(b, entry);
    }
    return b;
}

} // namespace pulp_test::container
