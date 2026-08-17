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
#include <pulp/runtime/log.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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
inline std::string asset_uri(
    const IRAssetRef& asset,
    const std::filesystem::path& asset_base_directory = {}) {
    if (asset.local_path && !asset.local_path->empty()) {
        auto path = std::filesystem::path(*asset.local_path);
        if (path.is_relative() && !asset_base_directory.empty())
            path = asset_base_directory / path;
        return "file://" + path.lexically_normal().generic_string();
    }
    if (!asset.original_uri.empty() &&
        (asset.original_uri.rfind("data:", 0) == 0 ||
         asset.original_uri.rfind("resource:", 0) == 0 ||
         asset.original_uri.rfind("memory:", 0) == 0)) {
        return asset.original_uri;
    }
    return {};
}

// ── On-disk asset resolution ─────────────────────────────────────────────
//
// asset_uri() above is the *compile-time* lowering: it names where the bytes
// are supposed to be, for a generated program that will run somewhere else.
// The functions below are the *runtime* counterpart — they only ever name a
// file that is actually readable right now, and they recover from a manifest
// whose local_path is wrong.
//
// The recovery matters because local_path is not always a usable path. A
// writer that stores a bare `<content_hash>.<ext>` filename produces an entry
// that resolves against the document's own directory, while the bytes were
// materialized into a shared, content-addressed asset folder alongside it.
// Assets ARE content-addressed, so the hash is enough to find them again.

// True when `path` names a readable regular file. Never throws: a permission
// or I/O failure reads as "not a candidate" rather than aborting resolution.
inline std::optional<std::filesystem::path> existing_asset_file(
    const std::filesystem::path& path) {
    if (path.empty()) return std::nullopt;
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec))
        return path.lexically_normal();
    return std::nullopt;
}

// The bare digest of a content hash: `sha256:abc…` and `abc…` both yield
// `abc…`, which is the stem content-addressed writers name files by.
inline std::string asset_content_hash_stem(const IRAssetRef& asset) {
    const auto separator = asset.content_hash.find_last_of(':');
    return separator == std::string::npos
        ? asset.content_hash
        : asset.content_hash.substr(separator + 1);
}

// The extension (including the dot) of a URI or path, ignoring any query or
// fragment; "" when there is none.
inline std::string asset_uri_extension(std::string_view uri) {
    const auto cut = uri.find_first_of("?#");
    if (cut != std::string_view::npos) uri = uri.substr(0, cut);
    const auto slash = uri.find_last_of("/\\");
    if (slash != std::string_view::npos) uri = uri.substr(slash + 1);
    const auto dot = uri.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 == uri.size()) return {};
    return std::string(uri.substr(dot));
}

// Directories a content-addressed asset may have been materialized into,
// given the directory the DesignIR document was loaded from: the document
// directory itself, a `design-assets/` folder inside it, and a
// `design-assets/` folder beside it (assets shared across sibling documents).
inline std::vector<std::filesystem::path> asset_search_roots(
    const std::filesystem::path& asset_base_directory) {
    std::vector<std::filesystem::path> roots;
    if (asset_base_directory.empty()) return roots;
    roots.push_back(asset_base_directory);
    roots.push_back(asset_base_directory / "design-assets");
    const auto parent = asset_base_directory.parent_path();
    if (!parent.empty() && parent != asset_base_directory)
        roots.push_back(parent / "design-assets");
    return roots;
}

// The readable file backing a manifest asset, or nullopt when none of the
// candidates exist. Order: the recorded local_path (absolute, or relative to
// the document directory), then a `file://` original_uri, then the
// content-addressed `<hash><ext>` under each search root. Recovering by hash
// is logged — a manifest path that no longer points at its bytes is a defect
// upstream, and one diagnosable line is what keeps it from reading as a
// rendering bug.
inline std::optional<std::filesystem::path> resolve_asset_file(
    const IRAssetRef& asset,
    const std::filesystem::path& asset_base_directory = {}) {
    if (asset.local_path && !asset.local_path->empty()) {
        auto path = std::filesystem::path(*asset.local_path);
        if (path.is_relative() && !asset_base_directory.empty())
            path = asset_base_directory / path;
        if (auto found = existing_asset_file(path)) return found;
    }
    if (asset.original_uri.rfind("file://", 0) == 0) {
        if (auto found =
                existing_asset_file(std::filesystem::path(asset.original_uri.substr(7))))
            return found;
    }

    const auto stem = asset_content_hash_stem(asset);
    if (stem.empty()) return std::nullopt;

    std::vector<std::string> extensions;
    for (std::string_view candidate :
         {asset.local_path ? std::string_view(*asset.local_path) : std::string_view{},
          std::string_view(asset.original_uri)}) {
        auto extension = asset_uri_extension(candidate);
        if (!extension.empty() &&
            std::find(extensions.begin(), extensions.end(), extension) == extensions.end()) {
            extensions.push_back(std::move(extension));
        }
    }
    extensions.emplace_back();  // a hash-named file with no extension

    for (const auto& root : asset_search_roots(asset_base_directory)) {
        for (const auto& extension : extensions) {
            if (auto found = existing_asset_file(root / (stem + extension))) {
                runtime::log_warn(
                    "design-import: asset '{}' recovered by content hash at '{}' — "
                    "manifest local_path '{}' does not resolve under '{}'",
                    asset.asset_id,
                    found->generic_string(),
                    asset.local_path.value_or(std::string{}),
                    asset_base_directory.generic_string());
                return found;
            }
        }
    }
    return std::nullopt;
}

// One diagnosable line for an asset that named a file and reached none of them.
// A recorded-but-absent path is the interesting case: an asset with no
// local_path at all was never materialized and is not a defect.
inline void log_unresolvable_asset(const IRAssetRef& asset,
                                   const std::filesystem::path& asset_base_directory) {
    if (!asset.local_path || asset.local_path->empty()) return;
    runtime::log_warn(
        "design-import: asset '{}' is unresolvable — manifest local_path '{}' "
        "does not exist under '{}', and no content-addressed file matched hash '{}'",
        asset.asset_id,
        *asset.local_path,
        asset_base_directory.generic_string(),
        asset.content_hash);
}

// A loadable URI for a manifest asset at RUNTIME: a `file://` URI only for a
// file that exists, else an already-self-contained original URI (data /
// resource / memory). A manifest path that resolves to nothing yields "" and a
// logged line — never a `file://` URI naming a file that isn't there, because
// the loader downstream drops that silently and the design just renders wrong.
inline std::string resolved_asset_uri(
    const IRAssetRef& asset,
    const std::filesystem::path& asset_base_directory = {}) {
    if (auto file = resolve_asset_file(asset, asset_base_directory))
        return "file://" + file->generic_string();
    if (!asset.original_uri.empty() &&
        (asset.original_uri.rfind("data:", 0) == 0 ||
         asset.original_uri.rfind("resource:", 0) == 0 ||
         asset.original_uri.rfind("memory:", 0) == 0)) {
        return asset.original_uri;
    }
    log_unresolvable_asset(asset, asset_base_directory);
    return {};
}

// The backing file's bytes as text, or "" plus a logged line when nothing
// resolves. Host-side / codegen-time only — does file I/O; never the
// audio/render thread.
inline std::string read_asset_text(
    const IRAssetRef& asset,
    const std::filesystem::path& asset_base_directory = {}) {
    auto file = resolve_asset_file(asset, asset_base_directory);
    if (!file) {
        log_unresolvable_asset(asset, asset_base_directory);
        return {};
    }
    std::ifstream input(*file, std::ios::binary);
    if (!input) return {};
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
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
// color — returns nullopt for the caller to handle or skip.
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

// ── Resize constraints ───────────────────────────────────────────────────

// What a Figma-style resize constraint pair lowers to in flex terms.
//
// `IRLayout::h_constraint` / `v_constraint` are declared in design_ir.hpp as a
// codegen-wide contract — "mapped onto flex/position at codegen (margin:auto /
// flex-grow / align-self), never a new layout primitive". The mapping itself
// lives here, once, because a target that re-derives it is free to disagree:
// a right-pinned button that reaches one lane's mapping and not another's is
// pinned in one render of the design and flush-left in the next, and nothing
// reports the difference.
//
// Pure decision only. Each lane applies the result in its own idiom — the
// materializer writes FlexStyle fields, the C++ emitter writes the equivalent
// source lines, the script emitter writes setFlex calls — because only the
// application differs per target, not the meaning.
//
// Takes the two tokens rather than the whole IRLayout on purpose. The
// cross-surface parity test (test/test_design_import_parity.cpp) proves a
// field is lowered by finding a `.field` member access in each surface's own
// source, so a lane that reached the mapping through `resolve(node.layout)`
// would share the logic and go dark to the ledger that tracks it. Naming both
// fields at every call site keeps one definition AND a per-lane signal.
struct ResolvedLayoutConstraints {
    bool margin_left_auto = false;
    bool margin_right_auto = false;
    bool margin_top_auto = false;
    bool margin_bottom_auto = false;
    bool grow = false;         // flex-grow: 1
    bool stretch = false;      // align-self: stretch
    bool fill_width = false;   // min-width: 100%
    bool fill_height = false;  // min-height: 100%
};

// `left` / `top` need no expression: they are the flex default, anchored start.
// An unrecognized token is left alone for the same reason — the token set is
// normalized at ingest (design_ir_json.cpp), so an unknown value means a
// producer Pulp does not model, and guessing at it would move the node.
//
// `stretch` pins both edges, which is fill-the-cross-axis. It carries min-width
// (or min-height) 100% alongside align-self so it stays effective against a
// node that ALSO has an explicit size: Yoga clamps a final size up to
// min-width, so without it a stretch constraint on a sized node is a no-op.
inline ResolvedLayoutConstraints resolve_layout_constraints(
    const std::optional<std::string>& h_constraint,
    const std::optional<std::string>& v_constraint) {
    ResolvedLayoutConstraints r;
    if (h_constraint) {
        const std::string h = lower_copy(*h_constraint);
        if (h == "center") {
            r.margin_left_auto = true;
            r.margin_right_auto = true;
        } else if (h == "right") {
            r.margin_left_auto = true;
        } else if (h == "scale") {
            r.grow = true;
        } else if (h == "stretch") {
            r.stretch = true;
            r.fill_width = true;
        }
    }
    if (v_constraint) {
        const std::string v = lower_copy(*v_constraint);
        if (v == "center") {
            r.margin_top_auto = true;
            r.margin_bottom_auto = true;
        } else if (v == "bottom") {
            r.margin_top_auto = true;
        } else if (v == "scale") {
            r.grow = true;
        } else if (v == "stretch") {
            r.stretch = true;
            r.fill_height = true;
        }
    }
    return r;
}

}  // namespace pulp::view
