// SPDX-License-Identifier: MIT
#include "browser_probe_cache.hpp"

#include <choc/text/choc_JSON.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

namespace pulp::import_design::browser_capture {

namespace fs = std::filesystem;

namespace {

constexpr int kSchemaVersion = 1;

// Size and modification time of one file, prefixed by its canonical path, so a
// browser swapped in place, updated, or reached through a different symlink
// all produce a different identity.
std::optional<std::string> file_identity(const fs::path& path) {
    std::error_code ec;
    const auto canonical = fs::canonical(path, ec);
    if (ec)
        return std::nullopt;
    const auto size = fs::file_size(canonical, ec);
    if (ec)
        return std::nullopt;
    const auto modified = fs::last_write_time(canonical, ec);
    if (ec)
        return std::nullopt;
    const auto ticks = modified.time_since_epoch().count();
    return canonical.string() + "|" + std::to_string(size) + "|" +
           std::to_string(static_cast<long long>(ticks));
}

// A macOS app's MacOS/ executable is a launcher whose bytes can survive an
// update that replaces the framework beside it; the bundle's Info.plist
// carries the version and is rewritten by every update.
std::optional<fs::path> bundle_info_plist(const fs::path& executable) {
    const auto macos = executable.parent_path();
    if (macos.filename() != "MacOS")
        return std::nullopt;
    const auto plist = macos.parent_path() / "Info.plist";
    std::error_code ec;
    if (!fs::is_regular_file(plist, ec))
        return std::nullopt;
    return plist;
}

std::string hex64(std::uint64_t value) {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

// The file name only spreads keys across entries; the full key stored inside
// the entry is what a lookup compares.
std::string entry_name(const std::string& key) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char c : key) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hex64(hash) + ".json";
}

} // namespace

std::optional<std::string> browser_probe_cache_key(const fs::path& browser_executable,
                                                   const fs::path& capture_script,
                                                   const fs::path& node_executable) {
    std::string key = "v" + std::to_string(kSchemaVersion);
    const auto browser = file_identity(browser_executable);
    const auto script = file_identity(capture_script);
    const auto node = file_identity(node_executable);
    if (!browser || !script || !node)
        return std::nullopt;
    key += "\nbrowser=" + *browser;
    std::error_code ec;
    if (const auto plist = bundle_info_plist(fs::canonical(browser_executable, ec))) {
        const auto identity = file_identity(*plist);
        if (!identity)
            return std::nullopt;
        key += "\nbundle=" + *identity;
    }
    key += "\nscript=" + *script;
    key += "\nnode=" + *node;
    return key;
}

std::optional<CachedBrowserIdentity> load_cached_browser_identity(const fs::path& cache_directory,
                                                                  const std::string& key) {
    if (cache_directory.empty())
        return std::nullopt;
    std::ifstream input(cache_directory / entry_name(key), std::ios::binary);
    if (!input)
        return std::nullopt;
    const std::string text{std::istreambuf_iterator<char>(input), {}};
    try {
        const auto entry = choc::json::parse(text);
        if (!entry.isObject() || entry["key"].toString() != key)
            return std::nullopt;
        CachedBrowserIdentity identity;
        identity.product = entry["product"].toString();
        identity.version = entry["version"].toString();
        identity.major_version = entry["major_version"].getWithDefault<int>(0);
        identity.protocol_version = entry["protocol_version"].toString();
        identity.build_hash = entry["build_hash"].toString();
        if (identity.product.empty() || identity.version.empty() || identity.major_version <= 0 ||
            identity.protocol_version.empty() || identity.build_hash.empty())
            return std::nullopt;
        return identity;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void store_cached_browser_identity(const fs::path& cache_directory, const std::string& key,
                                   const CachedBrowserIdentity& identity) {
    if (cache_directory.empty())
        return;
    std::error_code ec;
    fs::create_directories(cache_directory, ec);
    if (ec)
        return;
    auto entry = choc::value::createObject("");
    entry.addMember("key", key);
    entry.addMember("product", identity.product);
    entry.addMember("version", identity.version);
    entry.addMember("major_version", identity.major_version);
    entry.addMember("protocol_version", identity.protocol_version);
    entry.addMember("build_hash", identity.build_hash);

    // Write beside the entry and rename over it, so a concurrent import reads
    // either the old entry or the new one, never half of one.
    const auto destination = cache_directory / entry_name(key);
    auto staged = destination;
    staged += ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream output(staged, std::ios::binary | std::ios::trunc);
        output << choc::json::toString(entry, true) << "\n";
        if (!output) {
            fs::remove(staged, ec);
            return;
        }
    }
    fs::rename(staged, destination, ec);
    if (ec)
        fs::remove(staged, ec);
}

} // namespace pulp::import_design::browser_capture
