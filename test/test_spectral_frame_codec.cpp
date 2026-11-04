#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/signal/spectral_frame_codec.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace pulp::signal;

namespace {
std::vector<std::uint8_t> golden(const char* name) {
    std::ifstream f(std::string(SPECTRAL_CODEC_FIXTURES) + "/" + name + ".hex");
    REQUIRE(f.good());
    std::string word, hex;
    while (f >> word)
        hex += word;
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < hex.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return out;
}
void put(std::vector<std::uint8_t>& b, std::size_t p, std::uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i)
        b[p + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void matches(const OwnedSpectralStream& stream, const std::vector<std::uint8_t>& expected) {
    const auto encoded = encode_spectral_stream(stream);
    REQUIRE(encoded);
    REQUIRE(std::ranges::equal(encoded.value->view(), expected));
}
} // namespace

TEST_CASE("Canonical spectral codec matches frozen byte and content identities",
          "[spectral-codec]") {
    auto zero = make_spectral_stream({}, 1);
    REQUIRE(zero);
    REQUIRE(zero.value->frames[0].origin == -192);
    auto z = golden("zero");
    REQUIRE(z.size() == 1112);
    REQUIRE(pulp::runtime::sha256_hex(z.data(), z.size()) ==
            "71a0b86adce3bd285971b7fdca3d5a0a4279b7d0296a20931ab3ff7151e1ac08");
    matches(*zero.value, z);
    auto decoded = decode_spectral_stream(z);
    REQUIRE(decoded);
    matches(*decoded.value, z);

    SpectralStreamGeometry g;
    g.channels = 2;
    auto asymmetric = make_spectral_stream(g, 2);
    REQUIRE(asymmetric);
    auto set = [&](std::size_t frame, std::size_t channel, std::size_t bin, float re, float im) {
        const auto p = ((frame * 2 + channel) * 129 + bin) * 2;
        asymmetric.value->components[p] = re;
        asymmetric.value->components[p + 1] = im;
    };
    set(0, 0, 0, 1, 0);
    set(0, 0, 1, .5f, -.25f);
    set(0, 0, 128, -.75f, 0);
    set(0, 1, 0, -2, 0);
    set(0, 1, 2, .125f, 1.5f);
    set(0, 1, 128, .25f, 0);
    set(1, 0, 0, .375f, 0);
    set(1, 0, 3, -.5f, .25f);
    set(1, 0, 128, -.125f, 0);
    set(1, 1, 0, 4, 0);
    set(1, 1, 4, -1.25f, -.0625f);
    set(1, 1, 128, .75f, 0);
    auto a = golden("asymmetric");
    REQUIRE(a.size() == 4224);
    REQUIRE(pulp::runtime::sha256_hex(a.data(), a.size()) ==
            "36d0cff052298093d4e83b4d4a7b2aff6e0c421cbcf71dcd9ca9c1d7b6197ca1");
    matches(*asymmetric.value, a);
    decoded = decode_spectral_stream(a);
    REQUIRE(decoded);
    REQUIRE(decoded.value->frames[1].sequence == 1);
    REQUIRE(decoded.value->frames[1].origin == -128);
    REQUIRE(decoded.value->component_count == asymmetric.value->component_count);
    for (std::size_t i = 0; i < decoded.value->component_count; ++i)
        REQUIRE(std::bit_cast<std::uint32_t>(decoded.value->components[i]) ==
                std::bit_cast<std::uint32_t>(asymmetric.value->components[i]));
    matches(*decoded.value, a);
}

TEST_CASE("Canonical spectral codec rejects malformed headers and chronology", "[spectral-codec]") {
    const auto original = golden("asymmetric");
    struct Mutation {
        std::size_t offset;
        std::uint64_t value;
        unsigned width;
    };
    const Mutation bad[] = {{0, 0, 1},
                            {8, 0, 2},
                            {8, 2, 2},
                            {10, 63, 2},
                            {12, 2, 4},
                            {16, 0, 4},
                            {16, 128, 4},
                            {16, 257, 4},
                            {16, 32768, 4},
                            {20, 0, 4},
                            {20, 129, 4},
                            {24, 0, 2},
                            {24, 17, 2},
                            {26, 0, 2},
                            {28, 1, 4},
                            {32, 0, 8},
                            {32, 0x8000000000000000ULL, 8},
                            {32, 0xbff0000000000000ULL, 8},
                            {32, 0x7ff0000000000000ULL, 8},
                            {32, 0x7ff8000000000001ULL, 8},
                            {40, 0, 8},
                            {40, 1048577, 8},
                            {40, UINT64_MAX, 8},
                            {48, UINT64_MAX, 8},
                            {48, 4159, 8},
                            {56, 1, 8},
                            {64, 1, 8},
                            {72, 0, 8},
                            {72, 0x8000000000000000ULL, 8},
                            {2144, 0, 8},
                            {2144, 2, 8},
                            {2152, 0x7fffffffffffffffULL, 8}};
    for (auto m : bad) {
        INFO(m.offset << " " << m.value);
        auto b = original;
        put(b, m.offset, m.value, m.width);
        const auto r = decode_spectral_stream(b);
        REQUIRE_FALSE(r);
        REQUIRE(r.error != SpectralCodecError::None);
    }
    for (std::size_t n = 0; n < original.size(); ++n)
        REQUIRE_FALSE(decode_spectral_stream(std::span(original).first(n)));
    auto extra = original;
    extra.push_back(0);
    REQUIRE_FALSE(decode_spectral_stream(extra));
}

TEST_CASE("Canonical spectral numbers preserve finite bits and normalize only zero",
          "[spectral-codec]") {
    auto stream = make_spectral_stream({}, 1);
    REQUIRE(stream);
    for (auto bits : {0U, 1U, 0x80000001U, 0x007fffffU, 0x00800000U, 0x7f7fffffU, 0xff7fffffU}) {
        stream.value->components[2] = std::bit_cast<float>(bits);
        auto e = encode_spectral_stream(*stream.value);
        REQUIRE(e);
        auto d = decode_spectral_stream(e.value->view());
        REQUIRE(d);
        REQUIRE(std::bit_cast<std::uint32_t>(d.value->components[2]) == bits);
    }
    stream.value->components[2] = -0.0f;
    stream.value->components[1] = -0.0f;
    matches(*stream.value, golden("zero"));
    REQUIRE(std::bit_cast<std::uint32_t>(stream.value->components[1]) == 0x80000000U);
    for (auto bits : {0x80000000U, 0x7f800000U, 0xff800000U, 0x7fc00001U}) {
        for (auto offset : {80U, 84U, 88U, 1108U}) {
            auto b = golden("zero");
            put(b, offset, bits, 4);
            REQUIRE_FALSE(decode_spectral_stream(b));
        }
    }
    for (auto offset : {84U, 1108U}) {
        auto b = golden("zero");
        put(b, offset, 1, 4);
        REQUIRE_FALSE(decode_spectral_stream(b));
    }
    for (auto bits : {1U, 0x7f800000U, 0x7fc00001U}) {
        stream.value->components[1] = std::bit_cast<float>(bits);
        REQUIRE_FALSE(encode_spectral_stream(*stream.value));
    }
    for (auto rate : {std::bit_cast<double>(1ULL), std::numeric_limits<double>::max()}) {
        stream.value->geometry.sample_rate = rate;
        stream.value->components[1] = 0;
        auto e = encode_spectral_stream(*stream.value);
        REQUIRE(e);
        auto d = decode_spectral_stream(e.value->view());
        REQUIRE(d);
        REQUIRE(std::bit_cast<std::uint64_t>(d.value->geometry.sample_rate) ==
                std::bit_cast<std::uint64_t>(rate));
    }
}

TEST_CASE("Canonical spectral resource limits precede allocation", "[spectral-codec]") {
    const auto b = golden("zero");
    auto decoded = decode_spectral_stream(b);
    REQUIRE(decoded);
    const auto bytes = decoded.memory.retained_bytes + decoded.memory.temporary_bytes;
    REQUIRE(decoded.memory.input_bytes == b.size());
    REQUIRE(decode_spectral_stream(b, bytes));
    REQUIRE_FALSE(decode_spectral_stream(b, bytes - 1));
    REQUIRE_FALSE(make_spectral_stream({}, 1, bytes - 1));
    auto encoded = encode_spectral_stream(*decoded.value);
    REQUIRE(encoded);
    REQUIRE(encode_spectral_stream(*decoded.value,
                                   encoded.memory.retained_bytes + encoded.memory.temporary_bytes));
    REQUIRE_FALSE(encode_spectral_stream(*decoded.value, encoded.memory.retained_bytes +
                                                             encoded.memory.temporary_bytes - 1));
    REQUIRE_FALSE(make_spectral_stream({}, 1048576));
    SpectralStreamGeometry max;
    max.fft_size = 16384;
    max.hop = 8192;
    max.channels = 16;
    auto largest = make_spectral_stream(max, 63);
    REQUIRE(largest);
    REQUIRE_FALSE(make_spectral_stream(max, 64));
    REQUIRE(encode_spectral_stream(*largest.value));
    for (auto hop : {1U, 127U, 128U}) {
        auto s = make_spectral_stream({256, hop, 16, 44100}, 2);
        REQUIRE(s);
        auto e = encode_spectral_stream(*s.value);
        REQUIRE(e);
        REQUIRE(decode_spectral_stream(e.value->view()));
    }
    decoded.value->frames[0].sequence = UINT64_MAX;
    REQUIRE_FALSE(encode_spectral_stream(*decoded.value));
    decoded.value->frames[0].sequence = 0;
    decoded.value->frames[0].origin = INT64_MAX;
    REQUIRE_FALSE(encode_spectral_stream(*decoded.value));
    decoded.value->frames[0].origin = -192;
    --decoded.value->component_count;
    REQUIRE_FALSE(encode_spectral_stream(*decoded.value));
}
