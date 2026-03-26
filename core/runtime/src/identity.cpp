// UUID generation and identity system implementation
// Uses platform random sources for UUIDv4 generation.

#include <pulp/runtime/identity.hpp>
#include <random>
#include <sstream>
#include <iomanip>
#include <charconv>

namespace pulp::runtime {

// Thread-local random engine seeded from hardware entropy
static std::mt19937_64& rng() {
    thread_local std::mt19937_64 engine(std::random_device{}());
    return engine;
}

Uuid Uuid::generate() {
    std::uniform_int_distribution<uint64_t> dist;
    Uuid id;
    id.hi = dist(rng());
    id.lo = dist(rng());

    // Set version 4 (bits 48-51 of hi)
    id.hi = (id.hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    // Set variant 1 (bits 62-63 of lo)
    id.lo = (id.lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    return id;
}

static uint8_t hex_digit(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
    return 0;
}

Uuid Uuid::from_string(std::string_view str) {
    // Parse "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (36 chars)
    // or compact "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" (32 chars)
    Uuid id;
    std::string hex;
    hex.reserve(32);
    for (char c : str) {
        if (c != '-') hex += c;
    }
    if (hex.size() != 32) return id; // nil on parse failure

    for (int i = 0; i < 8; ++i) {
        id.hi = (id.hi << 8) | (hex_digit(hex[i * 2]) << 4 | hex_digit(hex[i * 2 + 1]));
    }
    for (int i = 0; i < 8; ++i) {
        id.lo = (id.lo << 8) | (hex_digit(hex[16 + i * 2]) << 4 | hex_digit(hex[16 + i * 2 + 1]));
    }
    return id;
}

std::string Uuid::to_string() const {
    // Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    char buf[37];
    auto fmt = [](char* out, uint64_t val, int start_byte, int end_byte) {
        for (int i = start_byte; i < end_byte; ++i) {
            uint8_t byte = static_cast<uint8_t>((val >> (56 - i * 8)) & 0xFF);
            static const char hex[] = "0123456789abcdef";
            *out++ = hex[byte >> 4];
            *out++ = hex[byte & 0x0F];
        }
        return out;
    };

    char* p = buf;
    p = fmt(p, hi, 0, 4); *p++ = '-';  // 8 chars
    p = fmt(p, hi, 4, 6); *p++ = '-';  // 4 chars
    p = fmt(p, hi, 6, 8); *p++ = '-';  // 4 chars
    p = fmt(p, lo, 0, 2); *p++ = '-';  // 4 chars
    p = fmt(p, lo, 2, 8);              // 12 chars
    *p = '\0';

    return std::string(buf, 36);
}

std::string Uuid::to_hex() const {
    char buf[33];
    auto fmt = [](char* out, uint64_t val) {
        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < 8; ++i) {
            uint8_t byte = static_cast<uint8_t>((val >> (56 - i * 8)) & 0xFF);
            *out++ = hex[byte >> 4];
            *out++ = hex[byte & 0x0F];
        }
        return out;
    };

    char* p = buf;
    p = fmt(p, hi);
    p = fmt(p, lo);
    *p = '\0';

    return std::string(buf, 32);
}

} // namespace pulp::runtime
