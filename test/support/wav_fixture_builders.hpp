#pragma once

// Byte-level RIFF/WAVE fixture builders shared by the in-memory decoder tests
// and the layout-report tests. Every fixture is built in code so a test can
// name the exact byte it corrupts.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pulp_test::wav {

using Bytes = std::vector<std::uint8_t>;

inline void append_id(Bytes& bytes, const char (&id)[5]) {
    for (std::size_t i = 0; i < 4u; ++i)
        bytes.push_back(static_cast<std::uint8_t>(id[i]));
}

inline void append_u16(Bytes& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8u));
}

inline void append_u32(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32u; shift += 8u)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

inline void append_u64(Bytes& bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64u; shift += 8u)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

inline void set_u32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32u; shift += 8u)
        bytes[offset + shift / 8u] = static_cast<std::uint8_t>(value >> shift);
}

inline void append_chunk(Bytes& bytes, const char (&id)[5], std::span<const std::uint8_t> payload) {
    append_id(bytes, id);
    append_u32(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    if ((payload.size() & 1u) != 0u)
        bytes.push_back(0u);
}

/// 16-byte PCM-style fmt payload.
inline Bytes make_format(std::uint16_t encoding, std::uint16_t channels, std::uint32_t sample_rate,
                         std::uint16_t bits) {
    Bytes format;
    const auto align = static_cast<std::uint16_t>(channels * (bits / 8u));
    append_u16(format, encoding);
    append_u16(format, channels);
    append_u32(format, sample_rate);
    append_u32(format, sample_rate * align);
    append_u16(format, align);
    append_u16(format, bits);
    return format;
}

/// 18-byte fmt payload (cbSize = 0), the form float WAVs commonly carry.
inline Bytes make_format_18(std::uint16_t encoding, std::uint16_t channels,
                            std::uint32_t sample_rate, std::uint16_t bits) {
    auto format = make_format(encoding, channels, sample_rate, bits);
    append_u16(format, 0u);
    return format;
}

inline const Bytes& pcm_subformat_guid() {
    static const Bytes guid{0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x10u, 0x00u,
                            0x80u, 0x00u, 0x00u, 0xaau, 0x00u, 0x38u, 0x9bu, 0x71u};
    return guid;
}

/// 40-byte WAVE_FORMAT_EXTENSIBLE PCM fmt payload. `valid_bits` defaults to
/// the container width; `channel_mask` defaults to a mask matching the
/// channel count (mono = FC, stereo = FL|FR).
inline Bytes make_extensible_pcm_format(std::uint16_t channels, std::uint32_t sample_rate,
                                        std::uint16_t bits, std::uint16_t valid_bits = 0u,
                                        std::uint32_t channel_mask = 0xffffffffu) {
    auto format = make_format(0xfffeu, channels, sample_rate, bits);
    append_u16(format, 22u);
    append_u16(format, valid_bits == 0u ? bits : valid_bits);
    append_u32(format, channel_mask == 0xffffffffu ? (channels == 1u ? 0x4u : 0x3u) : channel_mask);
    const auto& guid = pcm_subformat_guid();
    format.insert(format.end(), guid.begin(), guid.end());
    return format;
}

/// RIFF/WAVE around `format` and `data`. `data_first` writes data before fmt;
/// `odd_junk` prepends a one-byte JUNK chunk (padded). The RIFF size is
/// written to match the bytes produced.
inline Bytes make_wav(const Bytes& format, const Bytes& data, bool data_first = false,
                      bool odd_junk = false) {
    Bytes bytes;
    append_id(bytes, "RIFF");
    append_u32(bytes, 0u);
    append_id(bytes, "WAVE");
    if (odd_junk) {
        const Bytes junk{0x55u};
        append_chunk(bytes, "JUNK", junk);
    }
    if (data_first)
        append_chunk(bytes, "data", data);
    append_chunk(bytes, "fmt ", format);
    if (!data_first)
        append_chunk(bytes, "data", data);
    set_u32(bytes, 4u, static_cast<std::uint32_t>(bytes.size() - 8u));
    return bytes;
}

/// RIFF/WAVE with arbitrary chunks placed between fmt and data, RIFF size
/// matching the bytes produced.
struct NamedChunk {
    std::string id;
    Bytes payload;
};

inline Bytes make_wav_with_chunks(const Bytes& format, const Bytes& data,
                                  const std::vector<NamedChunk>& before_data,
                                  const std::vector<NamedChunk>& after_data = {}) {
    Bytes bytes;
    append_id(bytes, "RIFF");
    append_u32(bytes, 0u);
    append_id(bytes, "WAVE");
    append_chunk(bytes, "fmt ", format);
    auto append_named = [&](const NamedChunk& chunk) {
        char id[5] = {chunk.id[0], chunk.id[1], chunk.id[2], chunk.id[3], '\0'};
        append_chunk(bytes, id, chunk.payload);
    };
    for (const auto& chunk : before_data)
        append_named(chunk);
    append_chunk(bytes, "data", data);
    for (const auto& chunk : after_data)
        append_named(chunk);
    set_u32(bytes, 4u, static_cast<std::uint32_t>(bytes.size() - 8u));
    return bytes;
}

/// RF64 with a leading ds64. The data chunk's own size field is the sentinel
/// unless `data_size32` overrides it; `ds64_data_size` overrides the ds64
/// data size (defaults to the real payload size).
inline Bytes make_rf64(const Bytes& format, const Bytes& data,
                       std::uint32_t data_size32 = 0xffffffffu, std::uint64_t ds64_data_size = 0u,
                       const std::vector<NamedChunk>& after_data = {}) {
    Bytes bytes;
    append_id(bytes, "RF64");
    append_u32(bytes, 0xffffffffu);
    append_id(bytes, "WAVE");
    append_id(bytes, "ds64");
    append_u32(bytes, 28u);
    const auto riff_slot = bytes.size();
    append_u64(bytes, 0u);
    append_u64(bytes, ds64_data_size == 0u ? data.size() : ds64_data_size);
    append_u64(bytes, 0u);
    append_u32(bytes, 0u);
    append_chunk(bytes, "fmt ", format);
    append_id(bytes, "data");
    append_u32(bytes, data_size32);
    bytes.insert(bytes.end(), data.begin(), data.end());
    if ((data.size() & 1u) != 0u)
        bytes.push_back(0u);
    for (const auto& chunk : after_data) {
        char id[5] = {chunk.id[0], chunk.id[1], chunk.id[2], chunk.id[3], '\0'};
        append_chunk(bytes, id, chunk.payload);
    }
    const auto riff_size = static_cast<std::uint64_t>(bytes.size()) - 8u;
    for (unsigned shift = 0; shift < 64u; shift += 8u)
        bytes[riff_slot + shift / 8u] = static_cast<std::uint8_t>(riff_size >> shift);
    return bytes;
}

/// Sony Wave64: GUID ids, 64-bit sizes that include the 24-byte chunk
/// header, 8-byte alignment.
inline Bytes make_w64(const Bytes& format, const Bytes& data) {
    static const Bytes riff_guid{0x72, 0x69, 0x66, 0x66, 0x2E, 0x91, 0xCF, 0x11,
                                 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00};
    static const Bytes suffix{0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1,
                              0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A};
    auto guid = [&](const char (&id)[5]) {
        Bytes g;
        for (std::size_t i = 0; i < 4u; ++i)
            g.push_back(static_cast<std::uint8_t>(id[i]));
        g.insert(g.end(), suffix.begin(), suffix.end());
        return g;
    };
    auto chunk = [&](Bytes& out, const char (&id)[5], const Bytes& payload) {
        const auto g = guid(id);
        out.insert(out.end(), g.begin(), g.end());
        append_u64(out, 24u + payload.size());
        out.insert(out.end(), payload.begin(), payload.end());
        while ((out.size() & 7u) != 0u)
            out.push_back(0u);
    };
    Bytes bytes;
    bytes.insert(bytes.end(), riff_guid.begin(), riff_guid.end());
    append_u64(bytes, 0u);
    const auto wave = guid("wave");
    bytes.insert(bytes.end(), wave.begin(), wave.end());
    chunk(bytes, "fmt ", format);
    chunk(bytes, "data", data);
    const auto total = static_cast<std::uint64_t>(bytes.size());
    for (unsigned shift = 0; shift < 64u; shift += 8u)
        bytes[16u + shift / 8u] = static_cast<std::uint8_t>(total >> shift);
    return bytes;
}

template <typename T> void append_sample(Bytes& bytes, T value) {
    if constexpr (sizeof(T) == 2u) {
        append_u16(bytes, std::bit_cast<std::uint16_t>(value));
    } else if constexpr (sizeof(T) == 4u) {
        append_u32(bytes, std::bit_cast<std::uint32_t>(value));
    } else {
        const auto bits = std::bit_cast<std::uint64_t>(value);
        append_u32(bytes, static_cast<std::uint32_t>(bits));
        append_u32(bytes, static_cast<std::uint32_t>(bits >> 32u));
    }
}

/// A named fixture for tests that sweep every shape the decoder tests use.
struct Fixture {
    std::string name;
    Bytes bytes;
};

/// Every complete fixture the in-memory decoder tests exercise, valid and
/// invalid alike. Truncated prefixes are produced by the caller.
inline std::vector<Fixture> decoder_fixtures() {
    std::vector<Fixture> fixtures;
    auto add = [&](std::string name, Bytes bytes) {
        fixtures.push_back(Fixture{std::move(name), std::move(bytes)});
    };
    Bytes stereo16;
    append_sample(stereo16, std::int16_t{-32768});
    append_sample(stereo16, std::int16_t{16384});
    append_sample(stereo16, std::int16_t{32767});
    append_sample(stereo16, std::int16_t{-16384});
    add("pcm16 stereo", make_wav(make_format(1u, 2u, 48'000u, 16u), stereo16));
    add("pcm8", make_wav(make_format(1u, 1u, 8'000u, 8u), Bytes{0x00u, 0x80u, 0xffu}));
    add("pcm24", make_wav(make_format(1u, 1u, 44'100u, 24u),
                          Bytes{0x00u, 0x00u, 0x80u, 0xffu, 0xffu, 0x7fu}));
    Bytes pcm32;
    append_sample(pcm32, std::int32_t{-2147483647 - 1});
    append_sample(pcm32, std::int32_t{1073741824});
    add("pcm32", make_wav(make_format(1u, 1u, 96'000u, 32u), pcm32));
    Bytes f32;
    append_sample(f32, 1.25f);
    append_sample(f32, -0.25f);
    add("float32", make_wav(make_format(3u, 1u, 48'000u, 32u), f32));
    Bytes f64;
    append_sample(f64, 0.125);
    append_sample(f64, -2.0);
    add("float64", make_wav(make_format(3u, 1u, 48'000u, 64u), f64));
    Bytes ext16;
    append_sample(ext16, std::int16_t{4096});
    add("extensible pcm16", make_wav(make_extensible_pcm_format(1u, 48'000u, 16u), ext16));
    Bytes one16;
    append_sample(one16, std::int16_t{8192});
    add("data first with odd junk", make_wav(make_format(1u, 1u, 22'050u, 16u), one16, true, true));

    Bytes two16;
    append_sample(two16, std::int16_t{0});
    append_sample(two16, std::int16_t{1});
    const auto valid = make_wav(make_format(1u, 1u, 48'000u, 16u), two16);
    add("valid two-frame mono", valid);
    auto corrupt = valid;
    corrupt[0] = 'N';
    add("corrupt envelope id", corrupt);
    corrupt = valid;
    set_u32(corrupt, 40u, 0xfffffff0u);
    add("lying data size", corrupt);
    add("partial frame", make_wav(make_format(1u, 1u, 48'000u, 16u), Bytes{0u, 0u, 0u}));
    Bytes zero16;
    append_sample(zero16, std::int16_t{0});
    const auto one_frame = make_wav(make_format(1u, 1u, 48'000u, 16u), zero16);
    auto undersized = one_frame;
    set_u32(undersized, 4u, 28u);
    add("undersized envelope", undersized);
    auto trailing = one_frame;
    append_chunk(trailing, "JUNK", Bytes{0x42u, 0x43u});
    add("trailing chunk outside envelope", trailing);
    // A fact chunk between fmt and data at every size the decoder treats
    // differently: it reads four bytes regardless, so 0, 1 and 2 stop it, 3
    // lands back on the next header through its pad byte, 4 is well-formed.
    for (std::size_t fact_size = 0u; fact_size <= 5u; ++fact_size) {
        add("fact chunk of " + std::to_string(fact_size) + " bytes before data",
            make_wav_with_chunks(make_format(1u, 1u, 48'000u, 16u), two16,
                                 {{"fact", Bytes(fact_size, 0u)}}));
    }
    add("fact chunk of 2 bytes after data",
        make_wav_with_chunks(make_format(1u, 1u, 48'000u, 16u), two16, {},
                             {{"fact", Bytes(2u, 0u)}}));
    return fixtures;
}

} // namespace pulp_test::wav
