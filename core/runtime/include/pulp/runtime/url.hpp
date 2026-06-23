#pragma once

/// @file url.hpp
/// Minimal URL parser + builder utility.
///
/// Transport already lives in `http_*` and `http_download`; this header
/// adds the structural parse/build that callers need for query-string
/// manipulation, scheme dispatch, and round-tripping a URL through state.
///
/// Scope:
///   - Parses the common URL grammar — `scheme://[user[:pass]@]host
///     [:port][/path][?query][#fragment]`.
///   - Does NOT implement the full WHATWG URL Living Standard (no IDNA
///     punycode, no percent-encoded host validation). Sufficient for
///     audio plugin / app use cases — preset URLs, update feeds, telemetry
///     endpoints, OSC bridges, license check-ins.
///   - Percent-encoding is provided as standalone helpers.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::runtime {

/// Parsed URL components. All fields default to empty / absent.
/// `port` is 0 when absent, which is also "scheme default".
struct Url {
    std::string scheme;       // "http", "https", "file", ...
    std::string user;         // userinfo before optional :password
    std::string password;     // userinfo after optional :password (rare)
    std::string host;         // hostname or bracketed IPv6 literal
    uint16_t    port = 0;     // 0 if not specified
    std::string path;         // includes leading '/' if any
    std::string query;        // raw query string, NOT decoded, no '?'
    std::string fragment;     // raw fragment, NOT decoded, no '#'

    /// True when all components are empty / default.
    bool empty() const {
        return scheme.empty() && host.empty() && path.empty()
            && query.empty() && fragment.empty();
    }

    /// Default port for the scheme — e.g. http=80, https=443, ftp=21,
    /// ws=80, wss=443. Returns 0 for unknown schemes.
    uint16_t default_port() const;

    /// Effective port — `port` if non-zero, otherwise `default_port()`.
    uint16_t effective_port() const {
        return port != 0 ? port : default_port();
    }

    /// Serialize back to text form. Includes user/password only when
    /// present. Brackets IPv6 hosts.
    std::string to_string() const;

    /// Parse a URL string. Returns `std::nullopt` on a structurally
    /// invalid input (missing scheme or unterminated bracketed IPv6
    /// host). Empty optional fields are normal and not an error.
    static std::optional<Url> parse(std::string_view text);

    /// True if `text` parses as a URL with at least a scheme.
    static bool is_valid(std::string_view text) {
        auto u = parse(text);
        return u.has_value() && !u->scheme.empty();
    }
};

/// Percent-encode `s` per RFC 3986 — unreserved characters
/// (A-Z a-z 0-9 - _ . ~) pass through untouched. Spaces become `%20`,
/// not `+` (callers that want form-style encoding should call
/// `form_encode` instead).
std::string percent_encode(std::string_view s);

/// Decode percent-escapes — `%20` → space, `%2F` → `/`. Invalid escape
/// sequences are passed through unchanged.
std::string percent_decode(std::string_view s);

/// `application/x-www-form-urlencoded` style — space → `+`, then
/// percent-encode everything else.
std::string form_encode(std::string_view s);

/// Reverse of `form_encode`. `+` decodes to space.
std::string form_decode(std::string_view s);

/// Parse a URL query string into key/value pairs. Treats `&` and `;`
/// as separators. Leading `?` is tolerated. Values without `=` produce
/// pairs with empty value. Values are NOT decoded — call
/// `form_decode()` per-component if needed.
std::vector<std::pair<std::string, std::string>>
parse_query(std::string_view query);

/// Build a query string from key/value pairs. Applies `form_encode`
/// to each key and value. Does NOT emit a leading `?`.
std::string build_query(
    const std::vector<std::pair<std::string, std::string>>& params);

}  // namespace pulp::runtime
