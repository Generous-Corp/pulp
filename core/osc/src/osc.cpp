#include <pulp/osc/osc.hpp>
#include <cstring>
#include <algorithm>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace pulp::osc {

// ── Message accessors ────────────────────────────────────────────────────────

int32_t Message::get_int(size_t i, int32_t def) const {
    if (i >= args.size()) return def;
    if (auto* v = std::get_if<int32_t>(&args[i])) return *v;
    return def;
}

float Message::get_float(size_t i, float def) const {
    if (i >= args.size()) return def;
    if (auto* v = std::get_if<float>(&args[i])) return *v;
    return def;
}

std::string Message::get_string(size_t i, const std::string& def) const {
    if (i >= args.size()) return def;
    if (auto* v = std::get_if<std::string>(&args[i])) return *v;
    return def;
}

ColorRgba Message::get_colour(size_t i, ColorRgba def) const {
    if (i >= args.size()) return def;
    if (auto* v = std::get_if<ColorRgba>(&args[i])) return *v;
    return def;
}

// ── OSC encoding helpers ─────────────────────────────────────────────────────

// Pad to 4-byte boundary
static size_t pad4(size_t n) { return (n + 3) & ~3; }

static void write_string(std::vector<uint8_t>& buf, const std::string& s) {
    buf.insert(buf.end(), s.begin(), s.end());
    buf.push_back(0); // null terminator
    // Pad to 4-byte boundary
    while (buf.size() % 4 != 0) buf.push_back(0);
}

static void write_int32(std::vector<uint8_t>& buf, int32_t v) {
    uint32_t n = htonl(static_cast<uint32_t>(v));
    auto* p = reinterpret_cast<const uint8_t*>(&n);
    buf.insert(buf.end(), p, p + 4);
}

static void write_float32(std::vector<uint8_t>& buf, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    bits = htonl(bits);
    auto* p = reinterpret_cast<const uint8_t*>(&bits);
    buf.insert(buf.end(), p, p + 4);
}

static void write_blob(std::vector<uint8_t>& buf, const std::vector<uint8_t>& data) {
    write_int32(buf, static_cast<int32_t>(data.size()));
    buf.insert(buf.end(), data.begin(), data.end());
    while (buf.size() % 4 != 0) buf.push_back(0);
}

// ── Encode ───────────────────────────────────────────────────────────────────

std::vector<uint8_t> encode(const Message& msg) {
    std::vector<uint8_t> buf;

    // Address pattern
    write_string(buf, msg.address);

    // Type tag string
    std::string tags = ",";
    for (auto& arg : msg.args) {
        if (std::holds_alternative<int32_t>(arg)) tags += 'i';
        else if (std::holds_alternative<float>(arg)) tags += 'f';
        else if (std::holds_alternative<std::string>(arg)) tags += 's';
        else if (std::holds_alternative<std::vector<uint8_t>>(arg)) tags += 'b';
        else if (std::holds_alternative<ColorRgba>(arg)) tags += 'r';
    }
    write_string(buf, tags);

    // Arguments
    for (auto& arg : msg.args) {
        if (auto* v = std::get_if<int32_t>(&arg)) write_int32(buf, *v);
        else if (auto* v = std::get_if<float>(&arg)) write_float32(buf, *v);
        else if (auto* v = std::get_if<std::string>(&arg)) write_string(buf, *v);
        else if (auto* v = std::get_if<std::vector<uint8_t>>(&arg)) write_blob(buf, *v);
        else if (auto* v = std::get_if<ColorRgba>(&arg)) {
            // OSC 'r' encodes RGBA as a packed big-endian uint32.
            uint8_t packed[4] = {v->r, v->g, v->b, v->a};
            buf.insert(buf.end(), packed, packed + 4);
        }
    }

    return buf;
}

// ── Decode helpers ───────────────────────────────────────────────────────────

// Lenient string reader for the address and type-tag string: a bounded,
// unterminated trailing string is tolerated (a packet with no type-tag section
// is a valid zero-argument message).
static std::string read_string(const uint8_t* data, size_t size, size_t& offset) {
    if (offset >= size) return {};
    auto* start = reinterpret_cast<const char*>(data + offset);
    auto len = strnlen(start, size - offset);
    std::string result(start, len);
    offset += pad4(len + 1);
    return result;
}

// Strict typed-string argument reader: a missing null terminator within the
// remaining bytes means the payload is truncated → fail closed.
static std::string read_string_arg(const uint8_t* data, size_t size,
                                   size_t& offset, bool& ok) {
    if (offset >= size) { ok = false; return {}; }
    auto* start = reinterpret_cast<const char*>(data + offset);
    const size_t avail = size - offset;
    auto len = strnlen(start, avail);
    if (len == avail) { ok = false; return {}; }  // no terminator → truncated
    std::string result(start, len);
    offset += pad4(len + 1);
    return result;
}

static int32_t read_int32(const uint8_t* data, size_t size, size_t& offset, bool& ok) {
    if (offset + 4 > size) { ok = false; return 0; }
    uint32_t n;
    std::memcpy(&n, data + offset, 4);
    offset += 4;
    return static_cast<int32_t>(ntohl(n));
}

static float read_float32(const uint8_t* data, size_t size, size_t& offset, bool& ok) {
    if (offset + 4 > size) { ok = false; return 0; }
    uint32_t bits;
    std::memcpy(&bits, data + offset, 4);
    bits = ntohl(bits);
    float v;
    std::memcpy(&v, &bits, 4);
    offset += 4;
    return v;
}

static std::vector<uint8_t> read_blob(const uint8_t* data, size_t size,
                                      size_t& offset, bool& ok) {
    auto blob_size = read_int32(data, size, offset, ok);
    if (!ok) return {};
    if (blob_size < 0 ||
        offset + static_cast<size_t>(blob_size) > size) { ok = false; return {}; }
    std::vector<uint8_t> result(data + offset, data + offset + blob_size);
    offset += pad4(blob_size);
    return result;
}

// ── Decode ───────────────────────────────────────────────────────────────────

Message decode(const uint8_t* data, size_t size) {
    Message msg;
    size_t offset = 0;

    msg.address = read_string(data, size, offset);
    if (msg.address.empty() || msg.address[0] != '/') return {};

    std::string tags = read_string(data, size, offset);
    if (tags.empty() || tags[0] != ',') return msg; // No args is valid

    // Fail closed on a truncated/impossible typed argument: a malformed packet
    // must decode to the empty-address sentinel (osc.hpp contract), which the
    // receiver and bundle paths already reject as "malformed OSC message" —
    // rather than fabricating 0/default arg values that would dispatch a real
    // control change built from garbage. The address/type-tag reads stay lenient
    // so a zero-argument or unterminated-address packet still decodes.
    bool ok = true;
    for (size_t i = 1; i < tags.size() && ok; ++i) {
        switch (tags[i]) {
            case 'i': msg.args.emplace_back(read_int32(data, size, offset, ok)); break;
            case 'f': msg.args.emplace_back(read_float32(data, size, offset, ok)); break;
            case 's': msg.args.emplace_back(read_string_arg(data, size, offset, ok)); break;
            case 'b': msg.args.emplace_back(read_blob(data, size, offset, ok)); break;
            case 'r': {
                if (offset + 4 > size) { ok = false; break; }
                ColorRgba c{data[offset], data[offset + 1],
                             data[offset + 2], data[offset + 3]};
                offset += 4;
                msg.args.emplace_back(c);
                break;
            }
            default: break;  // unknown tag: skip, consume nothing
        }
    }

    if (!ok) return {};  // malformed typed payload → empty-address sentinel

    return msg;
}

} // namespace pulp::osc
