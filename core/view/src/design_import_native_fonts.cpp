// Registers the fonts a DesignIR ships (`fontFamilyAssets`) before its tree is
// materialized, so its text is shaped with them rather than with whatever
// same-named face the host has.
//
// A captured page may split one family across several files by unicode-range
// (Latin, Cyrillic, Greek...) and serve one file at several weights. Each
// unicode-range segment of a family is registered under its own private alias,
// and every font-family stack in the tree that names the family is rewritten
// to list the aliases, last-defined segment first. Text shaping then falls
// through the aliases per glyph, which picks the subset that covers the glyph
// the way CSS unicode-range matching does, and the weight within a segment is
// still chosen by the resolver from the faces registered there. The aliases
// are named from their content (the family, the range, and the bytes of every
// file), so two designs that both ship a "JetBrains Mono" never share a
// registry entry, and a changed file gets a new one.
#include "design_import_native_common.hpp"

#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/canvas/font_resolver.hpp>
#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace pulp::view {

namespace {

std::string lowered(std::string_view value) {
    std::string out(value);
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string unquoted(std::string_view entry) {
    if (entry.size() >= 2 && (entry.front() == '"' || entry.front() == '\'') &&
        entry.back() == entry.front())
        entry = entry.substr(1, entry.size() - 2);
    return std::string(entry);
}

struct Segment {
    std::string unicode_range;
    std::vector<std::string> asset_ids; // distinct, in definition order
    std::size_t last_rule = 0;          // index of the last rule defining it
    std::string alias;
};

// A shipped font larger than this is not read: no real face is near it, and
// the bytes are copied whole into the registry.
constexpr std::uintmax_t kMaxShippedFontBytes = 32u * 1024u * 1024u;

// TrueType (0x00010000 or 'true'), OpenType ('OTTO') or WOFF2 ('wOF2'): the
// containers the platform font managers and the WOFF2 decoder accept.
bool has_font_signature(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 4)
        return false;
    const std::string_view tag(reinterpret_cast<const char*>(bytes.data()), 4);
    return tag == std::string_view("\0\1\0\0", 4) || tag == "true" || tag == "OTTO" ||
           tag == "wOF2";
}

std::optional<std::vector<std::uint8_t>> read_font(const IRFontAsset& face,
                                                   const IRAssetManifest& manifest,
                                                   const NativeMaterializeOptions& options) {
    const IRAssetRef* asset = manifest.resolve(face.asset_id);
    std::string path = asset && asset->local_path ? *asset->local_path : face.resolved_path;
    if (path.empty())
        return std::nullopt;
    std::filesystem::path resolved{path};
    if (resolved.is_relative() && !options.asset_base_directory.empty())
        resolved = options.asset_base_directory / resolved;
    std::error_code ec;
    const auto size = std::filesystem::file_size(resolved, ec);
    if (ec || size == 0 || size > kMaxShippedFontBytes)
        return std::nullopt;
    std::ifstream in(resolved, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>()};
    // A file that no longer holds the bytes the manifest recorded is not the
    // face the design was captured with.
    if (!has_font_signature(bytes) ||
        (asset && !asset->content_hash.empty() &&
         pulp::runtime::sha256_hex(bytes.data(), bytes.size()) != asset->content_hash))
        return std::nullopt;
    return bytes;
}

bool register_face(const std::vector<std::uint8_t>& bytes, const std::string& family) {
    const bool woff2 = bytes[0] == 'w' && bytes[1] == 'O' && bytes[2] == 'F' && bytes[3] == '2';
    if (woff2 && pulp::canvas::register_font_woff2(bytes.data(), bytes.size(), family))
        return true;
    return pulp::canvas::register_font(bytes.data(), bytes.size(), family);
}

void rewrite_stack(std::optional<std::string>& stack,
                   const std::map<std::string, std::string>& aliases) {
    if (!stack || stack->empty())
        return;
    bool changed = false;
    std::string out;
    for (const auto& entry : pulp::canvas::split_css_font_family_list(*stack)) {
        if (!out.empty())
            out += ", ";
        if (const auto it = aliases.find(lowered(unquoted(entry))); it != aliases.end()) {
            out += it->second;
            changed = true;
        } else {
            out += entry;
        }
    }
    if (changed)
        stack = std::move(out);
}

void rewrite_tree(IRNode& node, const std::map<std::string, std::string>& aliases) {
    rewrite_stack(node.style.font_family, aliases);
    for (auto& run : node.text_runs)
        rewrite_stack(run.font_family, aliases);
    for (auto& child : node.children)
        rewrite_tree(child, aliases);
    for (auto& alternate : node.alternate_frames)
        rewrite_tree(alternate, aliases);
}

} // namespace

DesignIR prepare_native_ir_with_fonts(const DesignIR& source, const IRAssetManifest& manifest,
                                      const NativeMaterializeOptions& options) {
    DesignIR ir = prepare_native_design_ir(source);
    if (ir.font_family_assets.empty())
        return ir;

    // family (lowercased) → its unicode-range segments, and the family as written.
    std::map<std::string, std::vector<Segment>> families;
    std::map<std::string, std::string> spelling;
    for (std::size_t i = 0; i < ir.font_family_assets.size(); ++i) {
        const auto& face = ir.font_family_assets[i];
        if (face.family.empty() || face.asset_id.empty())
            continue;
        const auto key = lowered(face.family);
        spelling.emplace(key, face.family);
        auto& segments = families[key];
        auto it = std::find_if(segments.begin(), segments.end(), [&](const Segment& s) {
            return s.unicode_range == face.unicode_range;
        });
        if (it == segments.end()) {
            segments.push_back({face.unicode_range, {}, i, {}});
            it = std::prev(segments.end());
        }
        it->last_rule = i;
        if (std::find(it->asset_ids.begin(), it->asset_ids.end(), face.asset_id) ==
            it->asset_ids.end())
            it->asset_ids.push_back(face.asset_id);
    }

    std::map<std::string, std::string> aliases; // family (lowercased) → alias stack
    for (auto& [key, segments] : families) {
        // CSS checks overlapping unicode-ranges from the last rule defined.
        std::stable_sort(segments.begin(), segments.end(), [](const Segment& a, const Segment& b) {
            return a.last_rule > b.last_rule;
        });
        std::string stack;
        for (auto& segment : segments) {
            std::vector<std::optional<std::vector<std::uint8_t>>> files;
            std::string identity = key + "\n" + segment.unicode_range;
            for (const auto& id : segment.asset_ids) {
                const auto face =
                    std::find_if(ir.font_family_assets.begin(), ir.font_family_assets.end(),
                                 [&](const IRFontAsset& f) { return f.asset_id == id; });
                files.push_back(read_font(*face, manifest, options));
                identity += "\n" + id;
                if (const auto& bytes = files.back())
                    identity += " " + pulp::runtime::sha256_hex(bytes->data(), bytes->size());
            }
            segment.alias =
                spelling[key] + " [" + pulp::runtime::sha256_hex(identity).substr(0, 12) + "]";
            bool registered = false;
            for (std::size_t i = 0; i < segment.asset_ids.size(); ++i) {
                const auto& id = segment.asset_ids[i];
                const auto face =
                    std::find_if(ir.font_family_assets.begin(), ir.font_family_assets.end(),
                                 [&](const IRFontAsset& f) { return f.asset_id == id; });
                const auto& bytes = files[i];
                if (bytes && register_face(*bytes, segment.alias)) {
                    registered = true;
                    // The rule's declared weight range decides where Chromium
                    // synthesizes bold, so the alias carries it too.
                    pulp::canvas::declare_font_face_weight_range(
                        segment.alias, face->weight,
                        face->weight_max > 0 ? face->weight_max : face->weight);
                } else if (options.diagnostics_out != nullptr) {
                    ImportDiagnostic d;
                    d.severity = ImportDiagnosticSeverity::warning;
                    d.code = "font-asset-unavailable";
                    d.kind = ImportDiagnosticKind::unresolved_asset;
                    d.message = "shipped font " + id + " for '" + face->family +
                                "' could not be read or registered; text naming it "
                                "falls back to the host's faces";
                    options.diagnostics_out->push_back(std::move(d));
                }
            }
            if (!registered)
                continue;
            if (!stack.empty())
                stack += ", ";
            stack += "\"" + segment.alias + "\"";
        }
        if (!stack.empty())
            aliases.emplace(key, std::move(stack));
    }
    if (!aliases.empty())
        rewrite_tree(ir.root, aliases);
    return ir;
}

} // namespace pulp::view
