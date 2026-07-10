#pragma once

// The parts of CV-to-OSC that have nothing to do with sockets.
//
// The whole plug-in turns on one rule: **the audio thread never sends a packet.**
// `sendto()` is a syscall. It can block, it can allocate inside the kernel, and
// it takes a lock somewhere in every network stack. Calling it from `process()`
// is an xrun waiting for a busy network. So the audio thread does exactly one
// thing — a relaxed atomic store of the latest sample — and a background thread
// reads that store on its own clock and sends.
//
// This costs nothing in fidelity, because OSC is not a sample-accurate transport
// in the first place. It is UDP: unordered, unacknowledged, and delivered when it
// gets there. Sending at 60 Hz what a 48 kHz signal is doing loses nothing a
// receiver could have used.
//
// The destination and the addresses are *text*, not parameters. A float parameter
// cannot hold a hostname, and a bridge whose path and host cannot be typed only
// talks to itself. So they live in the plug-in's own state blob, which every
// adapter already round-trips through `serialize_plugin_state`.
//
// Everything here is pure so the tests can pin the decisions without a socket.

#include <brew/channels.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::examples::brew {

/// One OSC stream per CV channel, as everywhere else in the suite.
inline constexpr std::size_t kOscChannels = kChannelCount;

/// Where a fresh instance points. The loopback, because a plug-in that begins
/// emitting UDP at a stranger's address the moment a project loads is a surprise
/// nobody asked for. It is editable; it is just not a *default*.
inline constexpr const char* kDefaultOscHost = "127.0.0.1";
inline constexpr std::uint16_t kDefaultOscPort = 9000;

/// Below 1024 a listener needs root, so nothing a user should be typing here.
inline constexpr std::uint16_t kMinOscPort = 1024;
inline constexpr std::uint16_t kMaxOscPort = 65535;

inline constexpr float kMinRateHz = 1.0f;
inline constexpr float kMaxRateHz = 500.0f;

/// The mDNS service type OSC receivers advertise themselves under.
inline constexpr const char* kOscServiceType = "_osc._udp";

/// `/brew/cv/0`, `/brew/cv/1`, ... What a fresh instance sends to.
[[nodiscard]] inline std::string default_osc_path(std::size_t channel) {
    return "/brew/cv/" + std::to_string(channel);
}

/// Whether a value has moved far enough to be worth a packet.
///
/// A threshold, not a change test. Float noise on the last bit of an otherwise
/// steady voltage would otherwise flood the receiver at the full send rate, and
/// a receiver smoothing its input cannot tell that flood from a real signal.
///
/// `first` forces the very first send, so a receiver learns the resting value
/// rather than waiting for it to move. Without it, a DC source that never
/// changes would never announce itself, and the patch would look dead.
[[nodiscard]] inline bool should_send(bool first, float last_sent, float current,
                                      float threshold) noexcept {
    if (first) return true;
    if (!std::isfinite(current)) return false;
    return std::abs(current - last_sent) >= threshold;
}

/// Seconds between sends. Clamped, because a rate of zero is a divide by zero and
/// a rate of a million is a denial of service against the loopback interface.
[[nodiscard]] inline double send_interval_seconds(float rate_hz) noexcept {
    return 1.0 / static_cast<double>(std::clamp(rate_hz, kMinRateHz, kMaxRateHz));
}

// ── Target ───────────────────────────────────────────────────────────────────

struct OscTarget {
    std::string host = kDefaultOscHost;
    std::uint16_t port = kDefaultOscPort;

    friend bool operator==(const OscTarget& a, const OscTarget& b) noexcept {
        return a.host == b.host && a.port == b.port;
    }
};

/// Characters an OSC *address pattern* reserves for matching, plus the ones the
/// spec forbids outright. A path containing one of these is a pattern, and a
/// pattern is a thing a sender may not send.
[[nodiscard]] inline constexpr bool osc_path_char_ok(char c) noexcept {
    if (c < 0x21 || c > 0x7e) return false;  // no space, no control, no non-ASCII
    switch (c) {
        case '#': case '*': case ',': case '?':
        case '[': case ']': case '{': case '}':
            return false;
        default:
            return true;
    }
}

/// An OSC address the plug-in is allowed to send to.
///
/// Begins with `/`, has at least one part, no empty parts (so no `//` and no
/// trailing slash), and no pattern-matching characters. The last rule is the one
/// that matters: `/cv/*` is a legal thing for a *receiver* to match against and
/// an illegal thing for a sender to put in a packet, and a bridge that shipped
/// one would look like it was working while the receiver ignored every message.
[[nodiscard]] inline bool is_valid_osc_path(std::string_view p) noexcept {
    if (p.size() < 2 || p.front() != '/' || p.back() == '/') return false;
    bool previous_was_slash = false;
    for (std::size_t i = 0; i < p.size(); ++i) {
        const char c = p[i];
        if (c == '/') {
            if (previous_was_slash) return false;
            previous_was_slash = true;
            continue;
        }
        previous_was_slash = false;
        if (!osc_path_char_ok(c)) return false;
    }
    return true;
}

/// A hostname, an IPv4 literal, or a bracketed IPv6 literal.
///
/// Not a resolver: this only rejects the things that could never resolve, so a
/// typo lands in the editor rather than in a socket call on the sender thread.
[[nodiscard]] inline bool is_valid_osc_host(std::string_view h) noexcept {
    if (h.empty() || h.size() > 255) return false;
    if (h.front() == '[') return h.size() > 2 && h.back() == ']';
    for (const char c : h) {
        if (c == ':' || c == '/' || c == '[' || c == ']') return false;
        if (static_cast<unsigned char>(c) <= 0x20 ||
            static_cast<unsigned char>(c) >= 0x7f)
            return false;
    }
    return true;
}

/// A decimal port in the unprivileged range, or nothing.
///
/// Hand-rolled rather than `std::stoi`, which throws on a user's typo, and rather
/// than `std::from_chars`, whose float overload this repo has already been bitten
/// by. Five digits cannot overflow a 32-bit accumulator.
[[nodiscard]] inline std::optional<std::uint16_t> parse_osc_port(std::string_view digits) {
    if (digits.empty() || digits.size() > 5) return std::nullopt;
    std::uint32_t port = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') return std::nullopt;
        port = port * 10 + static_cast<std::uint32_t>(c - '0');
    }
    if (port < kMinOscPort || port > kMaxOscPort) return std::nullopt;
    return static_cast<std::uint16_t>(port);
}

/// `host:port`. The port is taken from the *last* colon, so `[::1]:9000` parses
/// as the loopback rather than as a host named `[` and a port named `:1]:9000`.
///
/// Returns nothing rather than a partial target. A half-parsed destination is how
/// a plug-in ends up sending to a port it was never told to use.
[[nodiscard]] inline std::optional<OscTarget> parse_osc_target(std::string_view text) {
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos) return std::nullopt;

    const std::string_view host = text.substr(0, colon);
    if (!is_valid_osc_host(host)) return std::nullopt;

    const auto port = parse_osc_port(text.substr(colon + 1));
    if (!port) return std::nullopt;

    return OscTarget{std::string(host), *port};
}

[[nodiscard]] inline std::string format_osc_target(const OscTarget& t) {
    return t.host + ":" + std::to_string(t.port);
}

// ── The whole text-shaped half of the plug-in's state ────────────────────────

struct OscSettings {
    OscTarget target{};
    std::array<std::string, kOscChannels> paths{default_osc_path(0),
                                                default_osc_path(1)};

    friend bool operator==(const OscSettings& a, const OscSettings& b) noexcept {
        return a.target == b.target && a.paths == b.paths;
    }
};

/// The blob every format adapter round-trips through `serialize_plugin_state`.
///
/// Line-oriented text, and versioned. Text because a hostname is text and a
/// binary layout for four strings buys nothing; versioned because the day a field
/// is added, an old blob must still load rather than silently arriving as a
/// target of `:0`.
inline constexpr int kOscStateVersion = 1;

[[nodiscard]] inline std::string serialize_osc_settings(const OscSettings& s) {
    std::string out = "brew.cvosc " + std::to_string(kOscStateVersion) + "\n";
    out += "host=" + s.target.host + "\n";
    out += "port=" + std::to_string(s.target.port) + "\n";
    for (std::size_t c = 0; c < kOscChannels; ++c)
        out += "path" + std::to_string(c) + "=" + s.paths[c] + "\n";
    return out;
}

/// Parses what `serialize_osc_settings` wrote, and refuses everything else.
///
/// A field that fails validation keeps the default rather than failing the whole
/// load: a project with one bad path should open with a good target, not refuse to
/// open. A bad *header* does fail, because a blob we did not write is a blob whose
/// fields we cannot claim to understand.
[[nodiscard]] inline std::optional<OscSettings> deserialize_osc_settings(
    std::string_view blob) {
    OscSettings s{};
    bool header_seen = false;

    std::size_t pos = 0;
    while (pos <= blob.size()) {
        const auto nl = blob.find('\n', pos);
        const std::string_view line =
            blob.substr(pos, nl == std::string_view::npos ? blob.size() - pos : nl - pos);
        pos = nl == std::string_view::npos ? blob.size() + 1 : nl + 1;
        if (line.empty()) continue;

        if (!header_seen) {
            if (line != "brew.cvosc " + std::to_string(kOscStateVersion))
                return std::nullopt;
            header_seen = true;
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;   // unknown shape: skip, don't fail
        const std::string_view key = line.substr(0, eq);
        const std::string_view value = line.substr(eq + 1);

        if (key == "host") {
            if (is_valid_osc_host(value)) s.target.host = std::string(value);
        } else if (key == "port") {
            if (const auto port = parse_osc_port(value)) s.target.port = *port;
        } else {
            for (std::size_t c = 0; c < kOscChannels; ++c) {
                if (key != "path" + std::to_string(c)) continue;
                if (is_valid_osc_path(value)) s.paths[c] = std::string(value);
            }
        }
    }

    if (!header_seen) return std::nullopt;
    return s;
}

}  // namespace pulp::examples::brew
