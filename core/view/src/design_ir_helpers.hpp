// design_ir_helpers.hpp — PRIVATE shared helpers for the design-import and
// design-codegen translation units.
//
// One definition for the small accessors and pure parsers every design lane
// reads the IR through. Each lane used to carry its own copy, which let the
// copies drift — and a drifted copy means the same IR value lowers differently
// per target by accident rather than by decision.
//
// Scope is deliberately narrow: only helpers whose contract is identical for
// every lane live here. Per-target string escaping, indenting, and number
// formatting stay local to their emitter — those genuinely differ per target.
//
// PRIVATE: lives under core/view/src/, not the public include tree. Not part
// of the installed SDK surface — do not reference from headers outside
// core/view/src/.

#pragma once

#include <pulp/view/design_import.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::view {

// ── Semantic IR accessors ────────────────────────────────────────────────

// A node attribute by key, or nullopt when the node does not carry it.
inline std::optional<std::string> attr(const IRNode& node, std::string_view key) {
    auto it = node.attributes.find(std::string(key));
    if (it == node.attributes.end()) return std::nullopt;
    return it->second;
}

// A node attribute read as a boolean. Accepts the spellings design sources
// emit for both polarities (case-insensitive); anything else — including an
// absent attribute — yields `fallback`.
inline bool attr_bool(const IRNode& node, std::string_view key, bool fallback = false) {
    auto value = attr(node, key);
    if (!value) return fallback;
    std::string lower;
    lower.reserve(value->size());
    for (unsigned char c : *value) lower += static_cast<char>(std::tolower(c));
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") return true;
    if (lower == "false" || lower == "0" || lower == "no" || lower == "off") return false;
    return fallback;
}

// The asset a node references: the explicit src / background / href / ref keys
// in priority order, then any other `*AssetId` attribute. The fallback scan is
// sorted by key so a node with several asset attributes resolves the same way
// on every run and every platform.
inline std::optional<std::string> first_asset_id(const IRNode& node) {
    for (std::string_view key : {"srcAssetId", "backgroundImageAssetId", "hrefAssetId", "asset_ref"}) {
        auto value = attr(node, key);
        if (value && !value->empty()) return value;
    }
    std::vector<std::pair<std::string, std::string>> candidates;
    for (const auto& [key, value] : node.attributes) {
        constexpr std::string_view kSuffix = "AssetId";
        if (key.size() >= kSuffix.size() &&
            key.compare(key.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0 &&
            !value.empty()) {
            candidates.emplace_back(key, value);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    if (!candidates.empty()) return candidates.front().second;
    return std::nullopt;
}

// A loadable URI for a manifest asset: a materialized local file wins, else an
// already-self-contained original URI (data / resource / memory). A remote URI
// is NOT returned — nothing downstream fetches it — so the caller sees "" and
// treats the asset as unresolved.
inline std::string asset_uri(const IRAssetRef& asset) {
    if (asset.local_path && !asset.local_path->empty())
        return "file://" + *asset.local_path;
    if (!asset.original_uri.empty() &&
        (asset.original_uri.rfind("data:", 0) == 0 ||
         asset.original_uri.rfind("resource:", 0) == 0 ||
         asset.original_uri.rfind("memory:", 0) == 0)) {
        return asset.original_uri;
    }
    return {};
}

// ── Pure helpers ─────────────────────────────────────────────────────────

// ASCII lowercase. Byte-wise, so a UTF-8 multibyte sequence passes through
// unchanged — the design-IR keywords these compare against are all ASCII.
inline std::string lower_copy(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) out += static_cast<char>(std::tolower(c));
    return out;
}

// A single hex nibble, or -1 when `c` is not a hex digit.
inline int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse "#rgb", "#rgba", "#rrggbb", "#rrggbbaa" → [r, g, b, a] in 0..255. The
// short forms expand each nibble to a byte (#abc → #aabbcc); an omitted alpha
// is opaque. Any other shape — including a CSS rgb()/rgba() token or a named
// colour — returns nullopt for the caller to handle or skip.
inline std::optional<std::array<unsigned, 4>> parse_hex_color_rgba(std::string_view value) {
    if (value.empty() || value.front() != '#') return std::nullopt;
    auto nibble = [](int v) -> unsigned { return static_cast<unsigned>((v << 4) | v); };
    if (value.size() == 4 || value.size() == 5) {
        const int r = hex_digit(value[1]);
        const int g = hex_digit(value[2]);
        const int b = hex_digit(value[3]);
        const int a = value.size() == 5 ? hex_digit(value[4]) : 15;
        if (r < 0 || g < 0 || b < 0 || a < 0) return std::nullopt;
        return std::array<unsigned, 4>{nibble(r), nibble(g), nibble(b), nibble(a)};
    }
    if (value.size() == 7 || value.size() == 9) {
        auto pair = [&](std::size_t offset) -> std::optional<unsigned> {
            const int hi = hex_digit(value[offset]);
            const int lo = hex_digit(value[offset + 1]);
            if (hi < 0 || lo < 0) return std::nullopt;
            return static_cast<unsigned>((hi << 4) | lo);
        };
        auto r = pair(1);
        auto g = pair(3);
        auto b = pair(5);
        auto a = value.size() == 9 ? pair(7) : std::optional<unsigned>(255);
        if (!r || !g || !b || !a) return std::nullopt;
        return std::array<unsigned, 4>{*r, *g, *b, *a};
    }
    return std::nullopt;
}

}  // namespace pulp::view
