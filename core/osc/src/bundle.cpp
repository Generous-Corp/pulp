#include <pulp/osc/bundle.hpp>
#include <cstring>
#include <functional>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace pulp::osc {

// ── TimeTag ─────────────────────────────────────────────────────────────────

// NTP epoch is 1900-01-01, Unix epoch is 1970-01-01.
// Difference: 70 years of seconds (including 17 leap years).
static constexpr uint32_t kNtpUnixEpochDiff = 2208988800u;

TimeTag TimeTag::from_unix(double unix_time) {
    if (unix_time <= 0) return immediate();
    auto total = static_cast<uint64_t>((unix_time + kNtpUnixEpochDiff) * (1ull << 32));
    return {static_cast<uint32_t>(total >> 32),
            static_cast<uint32_t>(total & 0xFFFFFFFF)};
}

double TimeTag::to_unix() const {
    double ntp = static_cast<double>(seconds) + static_cast<double>(fraction) / (1ull << 32);
    return ntp - kNtpUnixEpochDiff;
}

// ── Bundle serialization ────────────────────────────────────────────────────

static void write_bytes(std::vector<uint8_t>& buf, const void* data, size_t n) {
    auto* p = static_cast<const uint8_t*>(data);
    buf.insert(buf.end(), p, p + n);
}

static void pad_to_4(std::vector<uint8_t>& buf) {
    while (buf.size() % 4 != 0) buf.push_back(0);
}

std::vector<uint8_t> Bundle::serialize() const {
    std::vector<uint8_t> buf;

    // "#bundle\0" header
    const char header[] = "#bundle";
    buf.insert(buf.end(), header, header + 8);

    // Timetag (8 bytes, big-endian)
    uint32_t sec = htonl(timetag.seconds);
    uint32_t frac = htonl(timetag.fraction);
    write_bytes(buf, &sec, 4);
    write_bytes(buf, &frac, 4);

    // Elements
    for (auto& elem : elements) {
        std::vector<uint8_t> element_data;
        if (elem.is_message()) {
            element_data = encode(elem.message());
        } else if (elem.is_bundle()) {
            element_data = elem.bundle().serialize();
        }

        // Size prefix (4 bytes, big-endian)
        uint32_t size = htonl(static_cast<uint32_t>(element_data.size()));
        write_bytes(buf, &size, 4);
        buf.insert(buf.end(), element_data.begin(), element_data.end());
    }

    return buf;
}

std::optional<Bundle> Bundle::deserialize(const uint8_t* data, size_t size) {
    if (size < 16) return std::nullopt;  // minimum: header(8) + timetag(8)

    // Verify "#bundle\0" header
    if (std::memcmp(data, "#bundle", 8) != 0) return std::nullopt;

    Bundle bundle;

    // Read timetag
    uint32_t sec, frac;
    std::memcpy(&sec, data + 8, 4);
    std::memcpy(&frac, data + 12, 4);
    bundle.timetag.seconds = ntohl(sec);
    bundle.timetag.fraction = ntohl(frac);

    // Read elements
    size_t offset = 16;
    while (offset + 4 <= size) {
        uint32_t elem_size;
        std::memcpy(&elem_size, data + offset, 4);
        elem_size = ntohl(elem_size);
        offset += 4;

        if (offset + elem_size > size) return std::nullopt;

        // Nested bundle or message?
        if (elem_size >= 8 && std::memcmp(data + offset, "#bundle", 8) == 0) {
            auto nested = Bundle::deserialize(data + offset, elem_size);
            if (!nested) return std::nullopt;
            bundle.add(std::move(*nested));
        } else {
            auto msg = decode(data + offset, elem_size);
            if (msg.address.empty()) return std::nullopt;
            bundle.add(std::move(msg));
        }

        offset += elem_size;
    }

    if (offset != size) return std::nullopt;

    return bundle;
}

// ── Address pattern matching ────────────────────────────────────────────────

bool address_matches(std::string_view pattern, std::string_view address) {
    std::function<bool(size_t, size_t)> match = [&](size_t pi, size_t ai) -> bool {
        while (pi < pattern.size()) {
            char pc = pattern[pi];
            if (pc == '*') {
                // Match any sequence up to the next '/'
                ++pi;
                for (size_t candidate = ai;; ++candidate) {
                    if (match(pi, candidate)) return true;
                    if (candidate >= address.size() || address[candidate] == '/') break;
                }
                return false;
            } else if (pc == '?') {
                // Match any single character except '/'
                if (ai >= address.size()) return false;
                if (address[ai] == '/') return false;
                ++pi; ++ai;
            } else if (pc == '[') {
                // Character class
                if (ai >= address.size()) return false;
                ++pi;
                bool negate = pi < pattern.size() && pattern[pi] == '!';
                if (negate) ++pi;
                bool matched = false;
                bool closed = false;
                while (pi < pattern.size() && pattern[pi] != ']') {
                    if (pi + 2 < pattern.size() && pattern[pi + 1] == '-') {
                        if (address[ai] >= pattern[pi] && address[ai] <= pattern[pi + 2])
                            matched = true;
                        pi += 3;
                    } else {
                        if (address[ai] == pattern[pi]) matched = true;
                        ++pi;
                    }
                }
                if (pi < pattern.size()) {
                    closed = true;
                    ++pi;  // skip ']'
                }
                if (!closed) return false;
                if (matched == negate) return false;
                ++ai;
            } else if (pc == '{') {
                // Alternatives: {foo,bar,baz}
                ++pi;
                size_t close = pi;
                while (close < pattern.size() && pattern[close] != '}') ++close;
                if (close == pattern.size()) return false;

                size_t scan = pi;
                while (scan < close) {
                    size_t start = scan;
                    while (scan < close && pattern[scan] != ',') ++scan;
                    if (scan == start) return false;
                    if (scan < close && pattern[scan] == ',') ++scan;
                }

                while (pi < pattern.size() && pattern[pi] != '}') {
                    size_t start = pi;
                    while (pi < pattern.size() && pattern[pi] != ',' && pattern[pi] != '}') ++pi;
                    std::string_view alt = pattern.substr(start, pi - start);
                    if (address.substr(ai).starts_with(alt)
                        && match(close + 1, ai + alt.size())) {
                        return true;
                    }
                    if (pi < pattern.size() && pattern[pi] == ',') ++pi;
                }
                return false;
            } else {
                if (ai >= address.size()) return false;
                if (pc != address[ai]) return false;
                ++pi; ++ai;
            }
        }
        return ai == address.size();
    };

    return match(0, 0);
}

}  // namespace pulp::osc
