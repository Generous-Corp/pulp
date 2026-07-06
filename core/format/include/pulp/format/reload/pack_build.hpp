#pragma once

// Build a signable swap-pack manifest from a UX bundle directory: walk the files,
// hash each, and infer the least-privilege capability set from the bundle's JS so
// the developer does not hand-maintain it. The result is an UNSIGNED manifest ready
// for the signing step to confirm + sign.

#include <pulp/format/reload/swap_pack.hpp>
#include <pulp/view/reload_autocaps.hpp>
#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pulp::format::reload {

struct PackBuildResult {
    bool ok = false;
    std::string error;
    SwapPackManifest manifest;
};

inline std::string pack_read_all_bytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

/// Walk @p bundle_dir, hash each regular file into a manifest entry (path relative
/// to the bundle root, POSIX separators), and infer declared_capabilities by
/// running autocaps over the concatenated JS. Files are sorted for a deterministic
/// manifest (identical bundles produce identical content ids). Rejects symlinks.
inline PackBuildResult build_signable_manifest(const std::filesystem::path& bundle_dir,
                                               const std::string& plugin_id,
                                               std::uint64_t pack_version,
                                               const std::string& update_channel) {
    namespace fs = std::filesystem;
    PackBuildResult r;
    std::error_code ec;
    if (!fs::is_directory(bundle_dir, ec) || ec) {
        r.error = "bundle is not a directory: " + bundle_dir.string();
        return r;
    }

    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(bundle_dir, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_symlink()) {
            r.error = "bundle contains a symlink: " + it->path().string();
            return r;
        }
        if (it->is_regular_file()) files.push_back(it->path());
    }
    if (files.empty()) {
        r.error = "bundle has no files: " + bundle_dir.string();
        return r;
    }
    std::sort(files.begin(), files.end());

    SwapPackManifest m;
    m.id = plugin_id + "@" + std::to_string(pack_version);
    m.plugin_id = plugin_id;
    m.pack_version = pack_version;
    m.pack_type = SwapPackKind::UiScript;
    m.update_channel = update_channel;

    std::string all_js;
    for (const auto& f : files) {
        const std::string bytes = pack_read_all_bytes(f);
        std::string rel = fs::relative(f, bundle_dir, ec).generic_string();
        SwapPackFile pf;
        pf.path = rel;
        pf.sha256_hex = runtime::sha256_hex(bytes);
        pf.kind = SwapPackKind::UiScript;
        m.files.push_back(std::move(pf));
        const auto ext = f.extension().string();
        if (ext == ".js" || ext == ".mjs") { all_js += bytes; all_js += '\n'; }
    }

    m.declared_capabilities = view::infer_capability_names_from_js(all_js);
    r.ok = true;
    r.manifest = std::move(m);
    return r;
}

}  // namespace pulp::format::reload
