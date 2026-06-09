#pragma once

// Pure (X11-free) helpers for the Linux XDND target on the plugin-host child
// window. The XDND protocol machinery (atoms, ClientMessage handshake,
// XConvertSelection) lives in plugin_view_host_linux.cpp and needs Xlib; the
// payload parsing and coordinate mapping below are pure arithmetic / string
// work, so they live here as header-only free functions that compile on every
// platform. That lets the cross-platform drag-drop test suite exercise them
// without an X server or Xlib link — the handshake itself needs xvfb (see
// test_plugin_view_host_factory.cpp).

#include <pulp/view/geometry.hpp>

#include <string>
#include <vector>

namespace pulp::view::xdnd {

// Decode percent-encoded octets in a `file://` URI path component (RFC 3986).
// XDND `text/uri-list` payloads percent-encode reserved + non-ASCII bytes, so a
// path with a space arrives as `%20`. Unknown / malformed `%` escapes are left
// verbatim rather than dropped, so a stray `%` never silently truncates a path.
inline std::string percent_decode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            int hi = hex_val(in[i + 1]);
            int lo = hex_val(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

// Convert a single `file://` URI to a filesystem path. Mirrors the macOS
// producer's [[url path] UTF8String] result: the local path with the scheme +
// (optional) authority stripped and percent-escapes decoded. Forms handled:
//   file:///abs/path        -> /abs/path           (empty authority)
//   file://host/abs/path    -> /abs/path           (named authority dropped)
//   file:/abs/path          -> /abs/path           (no `//`, lenient)
//   /abs/path               -> /abs/path           (bare path, passthrough)
// A non-file scheme (http://, data:) returns empty — XDND file drops should
// only ever carry file URIs, and we refuse to hand a URL to a file consumer.
inline std::string file_uri_to_path(const std::string& uri) {
    constexpr const char* kScheme = "file:";
    constexpr size_t kSchemeLen = 5;  // strlen("file:")
    std::string s = uri;
    if (s.rfind(kScheme, 0) == 0) {
        s = s.substr(kSchemeLen);
        if (s.rfind("//", 0) == 0) {
            // Strip the authority component up to the first '/' of the path.
            size_t path_start = s.find('/', 2);
            if (path_start == std::string::npos) return {};  // authority only
            s = s.substr(path_start);
        }
        // else `file:/abs` (no authority) — s already begins with '/'.
    } else if (!s.empty() && s.front() == '/') {
        // Bare absolute path (tolerated; some sources omit the scheme).
    } else {
        return {};  // non-file scheme or relative junk
    }
    return percent_decode(s);
}

// Parse an XDND `text/uri-list` payload into filesystem paths. The format
// (RFC 2483) is CRLF-separated URIs; lines beginning with '#' are comments and
// blank lines are ignored. Lenient on the line ending: handles CRLF, lone LF,
// and a missing trailing newline. Non-file URIs are skipped (see
// file_uri_to_path), so a mixed payload keeps only the droppable files.
inline std::vector<std::string> parse_uri_list(const std::string& payload) {
    std::vector<std::string> paths;
    size_t start = 0;
    const size_t n = payload.size();
    while (start <= n) {
        size_t end = payload.find('\n', start);
        std::string line = payload.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        // Trim a trailing '\r' (CRLF) and any surrounding whitespace.
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos) line = line.substr(first);

        if (!line.empty() && line.front() != '#') {
            std::string path = file_uri_to_path(line);
            if (!path.empty()) paths.push_back(std::move(path));
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return paths;
}

// Map an XDND root-space drop point to the plugin-host child window's local
// coordinates. XDND XdndPosition carries the pointer position in ROOT-window
// coordinates; the host knows the child window's absolute origin on the root
// (from XTranslateCoordinates). Subtracting it yields the child-local point that
// then feeds window_to_root_point() (the design-viewport inverse) before
// dispatch — exactly mirroring how the mac producer converts draggingLocation.
inline Point root_to_child_local(int root_x, int root_y, int child_origin_x,
                                 int child_origin_y) {
    return {static_cast<float>(root_x - child_origin_x),
            static_cast<float>(root_y - child_origin_y)};
}

}  // namespace pulp::view::xdnd
