#include <pulp/format/host_version.hpp>

#include <cctype>
#include <charconv>
#include <string_view>

namespace pulp::format {

std::optional<HostVersion> parse_host_version(std::string_view s) {
    // Skip leading non-digit text (e.g., "Pro Tools 2024.6").
    size_t i = 0;
    while (i < s.size() && !std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    if (i == s.size()) return std::nullopt;

    HostVersion v;
    int* parts[3] = {&v.major, &v.minor, &v.patch};
    int part_idx = 0;

    while (i < s.size() && part_idx < 3) {
        size_t start = i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        if (start == i) break;
        int value = 0;
        auto [ptr, ec] = std::from_chars(s.data() + start, s.data() + i, value);
        if (ec != std::errc{}) return std::nullopt;
        *parts[part_idx++] = value;
        if (i < s.size() && s[i] == '.') ++i; // skip separator
        else break;
    }
    if (part_idx == 0) return std::nullopt;
    return v;
}

// Platform-specific version detection lives in
// `core/format/src/host_version_<platform>.{mm,cpp}`. The cross-platform
// fallback returns an unknown version — adapters must treat that as
// "version-gated quirks off".
#if !defined(__APPLE__) && !defined(_WIN32)
HostVersion detect_host_version(HostType /*type*/) {
    return {};
}
#endif

HostInfo detect_host_info() {
    HostInfo info;
    info.type = detect_host_type();
    info.version = detect_host_version(info.type);
    return info;
}

}  // namespace pulp::format
