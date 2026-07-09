// Ship-time PULP_TRACING guard.
//
// A binary compiled with PULP_TRACING=ON carries the dev-only Perfetto SDK: an
// ~80 MB in-memory ring and a `.pftrace` written inside the customer's DAW. That
// must never reach a shipping artifact. The runtime emits a retained sentinel
// byte-string into any ON build (see core/runtime/src/trace.cpp,
// `pulp_tracing_ship_sentinel`); this helper scans a candidate artifact for
// those bytes so `pulp ship` can refuse to package it without `--allow-tracing`.
//
// Header-only + dependency-light on purpose: it is unit-tested directly with
// fixture files (test/test_ship_tracing_guard.cpp) rather than only through a
// full ON build, which the coverage/CI default (PULP_TRACING=OFF) cannot produce.
#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace pulp::cli {

// The exact bytes emitted by core/runtime/src/trace.cpp under PULP_TRACING=ON.
// Kept in sync by test/test_ship_tracing_guard.cpp, which also greps the source.
inline constexpr std::string_view kTracingShipSentinel =
    "PULP_TRACING_COMPILED_IN__DO_NOT_SHIP";

// Returns true if `file` contains the tracing sentinel. Streams the file in
// overlapping chunks so the sentinel is found even when it straddles a chunk
// boundary, and so a multi-MB Mach-O/ELF/PE is scanned without a full slurp.
// Non-regular / unreadable paths return false (nothing to refuse).
inline bool file_has_tracing_sentinel(const std::filesystem::path& file) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_regular_file(file, ec) || ec) return false;

    std::ifstream in(file, std::ios::binary);
    if (!in) return false;

    const std::size_t needle = kTracingShipSentinel.size();
    constexpr std::size_t kChunk = 1u << 16;  // 64 KiB
    // Carry the last (needle-1) bytes of each chunk into the next so a sentinel
    // split across the boundary is still matched.
    std::string window;
    window.reserve(kChunk + needle);
    std::string block(kChunk, '\0');

    while (in) {
        in.read(block.data(), static_cast<std::streamsize>(kChunk));
        const std::streamsize got = in.gcount();
        if (got <= 0) break;
        window.append(block.data(), static_cast<std::size_t>(got));
        if (window.find(kTracingShipSentinel) != std::string::npos) return true;
        if (window.size() > needle) {
            window.erase(0, window.size() - (needle - 1));
        }
    }
    return false;
}

// Scans a shipping artifact for the sentinel. If the path is a directory (a
// macOS plugin/app bundle), every regular file under it is scanned — the
// Mach-O executable lives at Contents/MacOS/<name>, but scanning the whole
// bundle is cheap and future-proof against layout changes. Returns the first
// offending file's path (empty if clean), so the caller can name it.
inline std::filesystem::path artifact_tracing_offender(
    const std::filesystem::path& artifact) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::is_directory(artifact, ec) && !ec) {
        for (fs::recursive_directory_iterator it(
                 artifact, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && !ec
                && file_has_tracing_sentinel(it->path())) {
                return it->path();
            }
        }
        return {};
    }
    if (file_has_tracing_sentinel(artifact)) return artifact;
    return {};
}

}  // namespace pulp::cli
