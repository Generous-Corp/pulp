#include "browser_capture_fonts.hpp"

#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <unordered_map>
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
    if (mime == "font/woff")
        return ".woff";
    if (mime == "font/otf")
        return ".otf";
    return ".ttf";
}

// One captured @font-face rule and the bytes it names.
struct CapturedFace {
    std::string family;
    std::string weight;
    std::string style;
    std::string unicode_range;
    std::string asset_id;
    std::string mime;
    const std::vector<uint8_t>* data = nullptr;
};

// Writes and binds faces in order, so both capture lanes produce the same
// files, manifest entries and font_family_assets for the same bytes.
class FaceBinder {
  public:
    FaceBinder(DesignIR& ir, fs::path font_directory)
        : ir_(ir), font_directory_(std::move(font_directory)) {
        for (const auto& asset : ir_.asset_manifest.assets)
            manifest_ids_.insert(asset.asset_id);
    }

    bool bind(const CapturedFace& captured, std::string& error) {
        std::error_code ec;
        if (!directory_ready_) {
            fs::create_directories(font_directory_, ec);
            if (ec) {
                error = "could not create the captured font directory " + font_directory_.string() +
                        ": " + ec.message();
                return false;
            }
            directory_ready_ = true;
        }
        const auto& data = *captured.data;
        const std::string digest = pulp::runtime::sha256_hex(data.data(), data.size());
        const fs::path path = font_directory_ / (digest + font_extension(captured.mime));

        // Several rules share one file (a variable font bound at each weight
        // it serves); it is written once, the first time it is seen.
        if (!fs::exists(path, ec)) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size()));
            if (!out) {
                error = "could not write captured font " + path.string();
                return false;
            }
        }
        if (!manifest_ids_.contains(captured.asset_id)) {
            IRAssetRef ref;
            ref.asset_id = captured.asset_id;
            ref.original_uri = "pulp-capture:///fonts/" + path.filename().string();
            ref.local_path = path.string();
            ref.content_hash = digest;
            ref.mime = captured.mime;
            ref.font_family = captured.family;
            ir_.asset_manifest.assets.push_back(std::move(ref));
            manifest_ids_.insert(captured.asset_id);
        }

        IRFontAsset face;
        face.family = captured.family;
        face.asset_id = captured.asset_id;
        face.resolved_path = path.string();
        parse_weight(captured.weight, face);
        face.font_style = trimmed(captured.style);
        face.unicode_range = trimmed(captured.unicode_range);
        ir_.font_family_assets.push_back(std::move(face));
        return true;
    }

  private:
    DesignIR& ir_;
    fs::path font_directory_;
    std::unordered_set<std::string> manifest_ids_;
    bool directory_ready_ = false;
};

} // namespace

bool attach_captured_font_faces(DesignIR& ir, const ClaudeBundle& materialized,
                                const fs::path& font_directory, std::string& error) {
    FaceBinder binder(ir, font_directory);
    for (const auto& binding : materialized.font_bindings) {
        if (binding.asset_index >= materialized.assets.size()) {
            error = "captured font face " + binding.family + " names a missing asset";
            return false;
        }
        const auto& asset = materialized.assets[binding.asset_index];
        if (!binder.bind({binding.family, binding.weight, binding.style, binding.unicode_range,
                          asset.uuid, asset.mime, &asset.data},
                         error))
            return false;
    }
    return true;
}

bool attach_page_font_faces(DesignIR& ir, const fs::path& font_faces_json,
                            const fs::path& font_directory, std::string& error) {
    std::error_code ec;
    if (!fs::exists(font_faces_json, ec))
        return true;
    std::ifstream in(font_faces_json, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto invalid = [&](const std::string& why) {
        error = "captured page fonts " + font_faces_json.string() + " are invalid: " + why;
        return false;
    };
    choc::value::Value root;
    try {
        root = choc::json::parse(text);
    } catch (const std::exception& e) {
        return invalid(e.what());
    }
    if (!root.isObject() || root["schema"].toString() != "pulp-browser-capture-font-faces-v1" ||
        !root["faces"].isArray() || !root["assets"].isArray())
        return invalid("not a pulp-browser-capture-font-faces-v1 record");

    struct Bytes {
        std::string mime;
        std::vector<uint8_t> data;
    };
    std::unordered_map<std::string, Bytes> assets;
    const auto asset_list = root["assets"];
    for (uint32_t i = 0; i < asset_list.size(); ++i) {
        const auto asset = asset_list[i];
        const std::string sha = asset["sha256"].toString();
        auto decoded = pulp::runtime::base64_decode(asset["data_base64"].toString());
        if (!decoded)
            return invalid("font " + sha + " is not valid base64");
        Bytes bytes{asset["mime_type"].toString(), std::move(*decoded)};
        if (pulp::runtime::sha256_hex(bytes.data.data(), bytes.data.size()) != sha)
            return invalid("font bytes do not match their SHA-256 " + sha);
        assets.emplace(sha, std::move(bytes));
    }

    FaceBinder binder(ir, font_directory);
    const auto faces = root["faces"];
    for (uint32_t i = 0; i < faces.size(); ++i) {
        const auto face = faces[i];
        const std::string family = face["family"].toString();
        const auto found = assets.find(face["sha256"].toString());
        if (family.empty() || found == assets.end())
            return invalid("face " + std::to_string(i) + " names no carried font");
        if (!binder.bind({family, face["weight"].toString(), face["style"].toString(),
                          face["unicode_range"].toString(), "pulp-page-font-" + found->first,
                          found->second.mime, &found->second.data},
                         error))
            return false;
    }
    return true;
}

} // namespace pulp::import_design
