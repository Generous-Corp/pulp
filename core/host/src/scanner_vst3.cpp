// VST3 bundle FUID extraction for the scanner.
//
// Reads `Contents/Resources/moduleinfo.json` (Steinberg's
// declarative bundle metadata) and returns the first audio-effect
// class's CID as a 32-char lowercase hex string. moduleinfo.json is
// the recommended discovery mechanism since VST 3.7 precisely because
// it lets hosts enumerate a bundle's classes without running its
// native code — scanning thousands of plugins is then both fast and
// safe (no dlopen, no bundleEntry, no ObjC class collisions).
//
// Bundles that don't ship a moduleinfo.json fall back to the previous
// stem-based unique_id. Graph_serializer rehydration tolerates stem
// IDs; moduleinfo.json only upgrades plugins that already declare
// their identity.

#include <pulp/runtime/log.hpp>

#include <choc/text/choc_JSON.h>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace pulp::host {
namespace {

namespace fs = std::filesystem;

std::string resolve_moduleinfo_path(const std::string& bundle_path) {
    // Steinberg's convention across macOS/Windows/Linux:
    //   <Name>.vst3/Contents/Resources/moduleinfo.json
    fs::path p(bundle_path);
    std::error_code ec;
    if (!fs::is_directory(p, ec)) return {};
    auto candidate = p / "Contents" / "Resources" / "moduleinfo.json";
    if (fs::exists(candidate, ec)) return candidate.string();
    return {};
}

// Normalize a CID to 32-char lowercase hex. moduleinfo.json commonly
// stores CIDs already in this shape, but older tooling emits them
// as raw 16-byte strings with embedded non-hex bytes. We accept both
// and normalize to hex so graph_serializer matches are
// representation-agnostic.
std::string normalize_cid(const std::string& cid) {
    // If cid is exactly 32 chars and all hex, lowercase and return.
    if (cid.size() == 32) {
        std::string out;
        out.reserve(32);
        bool all_hex = true;
        for (char c : cid) {
            char lower = static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
            if (!((lower >= '0' && lower <= '9') ||
                  (lower >= 'a' && lower <= 'f'))) {
                all_hex = false;
                break;
            }
            out += lower;
        }
        if (all_hex) return out;
    }
    // Treat cid as 16 raw bytes. Serialize as 32-char hex.
    if (cid.size() == 16) {
        char buf[33] = {};
        for (int i = 0; i < 16; ++i) {
            std::snprintf(buf + i * 2, 3, "%02x",
                          static_cast<unsigned>(
                              static_cast<unsigned char>(cid[i])));
        }
        return std::string(buf, 32);
    }
    return {};
}

}  // namespace

std::string read_vst3_bundle_fuid(const std::string& path) {
    auto info_path = resolve_moduleinfo_path(path);
    if (info_path.empty()) return {};

    std::ifstream f(info_path);
    if (!f) return {};
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());

    choc::value::Value parsed;
    try {
        parsed = choc::json::parse(content);
    } catch (const std::exception& e) {
        runtime::log_warn("VST3 scan: moduleinfo.json parse failed for '{}': {}",
                          path, e.what());
        return {};
    }

    if (!parsed.isObject() || !parsed.hasObjectMember("Classes")) {
        return {};
    }
    auto classes = parsed["Classes"];
    if (!classes.isArray()) return {};

    for (uint32_t i = 0; i < classes.size(); ++i) {
        auto cls = classes[i];
        if (!cls.isObject()) continue;
        if (!cls.hasObjectMember("Category")) continue;
        auto category = cls["Category"].getWithDefault<std::string>("");
        if (category != "Audio Module Class") continue;
        if (!cls.hasObjectMember("CID")) continue;
        auto cid = cls["CID"].getWithDefault<std::string>("");
        auto hex = normalize_cid(cid);
        if (!hex.empty()) return hex;
    }
    return {};
}

}  // namespace pulp::host
