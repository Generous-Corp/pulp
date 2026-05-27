#include <pulp/runtime/url.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pulp::runtime {

namespace {

// Parse an all-digits port. Returns nullopt on empty, non-digit, leading
// sign, or > 65535. strtol() alone accepts trailing junk like "80abc",
// which would silently parse as a valid URL — pinned by the Codex P2
// review comment "Reject non-numeric URL ports".
std::optional<uint16_t> parse_port(std::string_view text) {
    if (text.empty()) return std::nullopt;
    uint32_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<uint32_t>(c - '0');
        if (value > 65535u) return std::nullopt;
    }
    if (value == 0) return std::nullopt;
    return static_cast<uint16_t>(value);
}

bool is_unreserved(unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

void append_pct(std::string& out, unsigned char c) {
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%%%02X", c);
    out.append(buf, 3);
}

}  // namespace

uint16_t Url::default_port() const {
    // ASCII case-insensitive compare.
    auto eq = [](const std::string& a, const char* b) {
        if (a.size() != std::strlen(b)) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) != b[i]) return false;
        }
        return true;
    };
    if (eq(scheme, "http"))  return 80;
    if (eq(scheme, "https")) return 443;
    if (eq(scheme, "ftp"))   return 21;
    if (eq(scheme, "ws"))    return 80;
    if (eq(scheme, "wss"))   return 443;
    if (eq(scheme, "ssh"))   return 22;
    if (eq(scheme, "telnet")) return 23;
    if (eq(scheme, "smtp"))  return 25;
    if (eq(scheme, "dns"))   return 53;
    return 0;
}

std::string Url::to_string() const {
    std::string out;
    if (!scheme.empty()) {
        out += scheme;
        out += "://";
    }
    if (!user.empty() || !password.empty()) {
        out += user;
        if (!password.empty()) {
            out += ':';
            out += password;
        }
        out += '@';
    }
    if (!host.empty()) {
        // IPv6 host literals contain ':'; bracket on output to disambiguate
        // from the host:port boundary.
        bool needs_brackets = host.find(':') != std::string::npos
                              && host.front() != '[';
        if (needs_brackets) out += '[';
        out += host;
        if (needs_brackets) out += ']';
    }
    if (port != 0) {
        out += ':';
        out += std::to_string(port);
    }
    out += path;
    if (!query.empty()) {
        out += '?';
        out += query;
    }
    if (!fragment.empty()) {
        out += '#';
        out += fragment;
    }
    return out;
}

std::optional<Url> Url::parse(std::string_view text) {
    Url u;
    if (text.empty()) return std::nullopt;
    size_t i = 0;

    // Scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":"
    size_t scheme_end = text.find(':');
    if (scheme_end == std::string_view::npos || scheme_end == 0) {
        return std::nullopt;
    }
    {
        char c0 = text[0];
        if (!std::isalpha(static_cast<unsigned char>(c0))) return std::nullopt;
        for (size_t k = 1; k < scheme_end; ++k) {
            char c = text[k];
            if (!std::isalnum(static_cast<unsigned char>(c))
                && c != '+' && c != '-' && c != '.') {
                return std::nullopt;
            }
        }
    }
    u.scheme.assign(text.substr(0, scheme_end));
    i = scheme_end + 1;

    // Optional authority: "//" userinfo? host (":" port)?
    if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '/') {
        i += 2;
        size_t auth_end = i;
        while (auth_end < text.size()
               && text[auth_end] != '/'
               && text[auth_end] != '?'
               && text[auth_end] != '#') {
            ++auth_end;
        }
        std::string_view authority = text.substr(i, auth_end - i);
        i = auth_end;

        size_t at = authority.rfind('@');
        std::string_view host_port;
        if (at != std::string_view::npos) {
            std::string_view userinfo = authority.substr(0, at);
            host_port = authority.substr(at + 1);
            size_t colon = userinfo.find(':');
            if (colon == std::string_view::npos) {
                u.user.assign(userinfo);
            } else {
                u.user.assign(userinfo.substr(0, colon));
                u.password.assign(userinfo.substr(colon + 1));
            }
        } else {
            host_port = authority;
        }
        // host[:port] — IPv6 hosts wrap in [].
        if (!host_port.empty() && host_port.front() == '[') {
            auto rb = host_port.find(']');
            if (rb == std::string_view::npos) return std::nullopt;
            u.host.assign(host_port.substr(1, rb - 1));
            if (rb + 1 < host_port.size()) {
                if (host_port[rb + 1] != ':') return std::nullopt;
                auto port_text = host_port.substr(rb + 2);
                auto parsed = parse_port(port_text);
                if (!parsed) return std::nullopt;
                u.port = *parsed;
            }
        } else {
            size_t colon = host_port.rfind(':');
            if (colon == std::string_view::npos) {
                u.host.assign(host_port);
            } else {
                u.host.assign(host_port.substr(0, colon));
                auto port_text = host_port.substr(colon + 1);
                if (port_text.empty()) {
                    // trailing ':' with no port — treat as no port.
                } else {
                    auto parsed = parse_port(port_text);
                    if (!parsed) return std::nullopt;
                    u.port = *parsed;
                }
            }
        }
    }

    // Path: everything up to '?' or '#'.
    size_t q = text.find('?', i);
    size_t f = text.find('#', i);
    size_t path_end = std::min(q == std::string_view::npos ? text.size() : q,
                               f == std::string_view::npos ? text.size() : f);
    u.path.assign(text.substr(i, path_end - i));
    i = path_end;

    // Query: '?' up to '#'.
    if (i < text.size() && text[i] == '?') {
        ++i;
        size_t end = text.find('#', i);
        if (end == std::string_view::npos) end = text.size();
        u.query.assign(text.substr(i, end - i));
        i = end;
    }
    // Fragment: '#' to end.
    if (i < text.size() && text[i] == '#') {
        ++i;
        u.fragment.assign(text.substr(i));
    }

    return u;
}

std::string percent_encode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        auto uc = static_cast<unsigned char>(c);
        if (is_unreserved(uc)) {
            out.push_back(c);
        } else {
            append_pct(out, uc);
        }
    }
    return out;
}

std::string percent_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex_value(s[i + 1]);
            int lo = hex_value(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::string form_encode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        auto uc = static_cast<unsigned char>(c);
        if (c == ' ') {
            out.push_back('+');
        } else if (is_unreserved(uc)) {
            out.push_back(c);
        } else {
            append_pct(out, uc);
        }
    }
    return out;
}

std::string form_decode(std::string_view s) {
    std::string swap;
    swap.reserve(s.size());
    for (char c : s) swap.push_back(c == '+' ? ' ' : c);
    return percent_decode(swap);
}

std::vector<std::pair<std::string, std::string>>
parse_query(std::string_view query) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!query.empty() && query.front() == '?') query.remove_prefix(1);
    if (query.empty()) return out;

    size_t i = 0;
    while (i <= query.size()) {
        size_t sep = i;
        while (sep < query.size() && query[sep] != '&' && query[sep] != ';') {
            ++sep;
        }
        auto pair = query.substr(i, sep - i);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            if (eq == std::string_view::npos) {
                out.emplace_back(std::string(pair), std::string());
            } else {
                out.emplace_back(std::string(pair.substr(0, eq)),
                                 std::string(pair.substr(eq + 1)));
            }
        }
        if (sep == query.size()) break;
        i = sep + 1;
    }
    return out;
}

std::string build_query(
    const std::vector<std::pair<std::string, std::string>>& params) {
    std::string out;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i != 0) out += '&';
        out += form_encode(params[i].first);
        out += '=';
        out += form_encode(params[i].second);
    }
    return out;
}

}  // namespace pulp::runtime
