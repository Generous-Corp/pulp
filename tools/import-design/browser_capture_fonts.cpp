#include "browser_capture_fonts.hpp"

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <unordered_set>
#include <vector>

namespace pulp::import_design {

namespace fs = std::filesystem;
using pulp::view::ClaudeBundle;
using pulp::view::DesignIR;
using pulp::view::IRAssetRef;
using pulp::view::IRFontAsset;

namespace {

std::string trimmed(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return std::string(value);
}

// One token of a CSS @font-face font-weight descriptor, or 0 when it is not
// one (`auto`, or anything malformed, leaves the default).
int weight_token(std::string_view token) {
    if (token == "normal")
        return 400;
    if (token == "bold")
        return 700;
    int value = 0;
    const auto* end = token.data() + token.size();
    const auto parsed = std::from_chars(token.data(), end, value);
    if (parsed.ec != std::errc{})
        return 0;
    // A fractional weight ("450.5") rounds down to its integer part; CSS
    // weights in captured rules are integers in practice.
    if (parsed.ptr != end && *parsed.ptr != '.')
        return 0;
    return std::clamp(value, 1, 1000);
}

// `font-weight: 600` → {600, 0}; `font-weight: 100 800` → {100, 800}.
void parse_weight(const std::string& descriptor, IRFontAsset& face) {
    std::vector<int> weights;
    std::string token;
    for (const char c : descriptor + " ") {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!token.empty()) {
                if (const int w = weight_token(token); w > 0)
                    weights.push_back(w);
                token.clear();
            }
        } else {
            token.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (weights.empty())
        return;
    face.weight = weights.front();
    if (weights.size() >= 2 && weights[1] != weights[0]) {
        face.weight = std::min(weights[0], weights[1]);
        face.weight_max = std::max(weights[0], weights[1]);
    }
}

const char* font_extension(const std::string& mime) {
    if (mime == "font/woff2")
        return ".woff2";
    if (mime == "font/otf")
        return ".otf";
    return ".ttf";
}

} // namespace

bool attach_captured_font_faces(DesignIR& ir, const ClaudeBundle& materialized,
                                const fs::path& font_directory, std::string& error) {
    if (materialized.font_bindings.empty())
        return true;

    std::error_code ec;
    fs::create_directories(font_directory, ec);
    if (ec) {
        error = "could not create the captured font directory " + font_directory.string() + ": " +
                ec.message();
        return false;
    }

    std::unordered_set<std::string> manifest_ids;
    for (const auto& asset : ir.asset_manifest.assets)
        manifest_ids.insert(asset.asset_id);

    for (const auto& binding : materialized.font_bindings) {
        if (binding.asset_index >= materialized.assets.size()) {
            error = "captured font face " + binding.family + " names a missing asset";
            return false;
        }
        const auto& asset = materialized.assets[binding.asset_index];
        const std::string digest = pulp::runtime::sha256_hex(asset.data.data(), asset.data.size());
        const fs::path path = font_directory / (digest + font_extension(asset.mime));

        // Several rules share one file (a variable font bound at each weight
        // it serves); it is written once, the first time it is seen.
        if (!fs::exists(path, ec)) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(asset.data.data()),
                      static_cast<std::streamsize>(asset.data.size()));
            if (!out) {
                error = "could not write captured font " + path.string();
                return false;
            }
        }
        if (!manifest_ids.contains(asset.uuid)) {
            IRAssetRef ref;
            ref.asset_id = asset.uuid;
            ref.original_uri = "pulp-capture:///fonts/" + path.filename().string();
            ref.local_path = path.string();
            ref.content_hash = digest;
            ref.mime = asset.mime;
            ref.font_family = binding.family;
            ir.asset_manifest.assets.push_back(std::move(ref));
            manifest_ids.insert(asset.uuid);
        }

        IRFontAsset face;
        face.family = binding.family;
        face.asset_id = asset.uuid;
        face.resolved_path = path.string();
        parse_weight(binding.weight, face);
        face.font_style = trimmed(binding.style);
        face.unicode_range = trimmed(binding.unicode_range);
        ir.font_family_assets.push_back(std::move(face));
    }
    return true;
}

} // namespace pulp::import_design
