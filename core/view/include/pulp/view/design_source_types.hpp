#pragma once

/// @file design_source_types.hpp
/// Value types exchanged with the design-source adapters: DESIGN.md parse and
/// diff results, the Claude Design / runtime bundle envelope, runtime-import
/// options, the materialized-document cache counters, and the Claude classname
/// map. Part of `pulp-design-ir`: standard library and design_ir.hpp only. The
/// adapters that produce and consume these live in design_sources.hpp.

#include <pulp/view/design_ir.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pulp::view {

struct SnapshotDynamicApiScan {
    std::vector<std::string> tokens;

    bool has_dynamic_apis() const {
        return !tokens.empty();
    }
};

enum class DesignMdSeverity { info, warning, error };

struct DesignMdDiagnostic {
    DesignMdSeverity severity = DesignMdSeverity::info;
    std::string code; // e.g. "broken-ref", "duplicate-section"
    std::string path; // dotted token path or section name
    int line = 0;     // 1-based; 0 = unknown
    int column = 0;   // 1-based; 0 = unknown
    std::string message;
};

struct DesignMdOmittedSection {
    std::string section;
    std::string reason;
};

struct DesignMdParseResult {
    DesignIR ir;
    std::vector<DesignMdDiagnostic> diagnostics;
    // Sections seen in the body, in the order they appeared. Used by the
    // section-order lint rule.
    std::vector<std::string> sections;
    bool had_frontmatter = false;
    // Names of color tokens (the part after "colors.") that were
    // dereferenced during parse. `resolve_references` rewrites
    // `{colors.X}` to the resolved hex value in-place, which would
    // otherwise hide the reference from `lint_designmd`'s
    // orphaned-tokens rule. The rule consumes this set directly
    // instead of re-scanning post-resolution strings.
    std::vector<std::string> referenced_color_tokens;
    // DESIGN.md 0.4 `omitted` declarations. These are metadata rather than
    // design tokens; lint uses them to suppress intentional missing-section
    // findings and to report unknown or redundant declarations.
    std::vector<DesignMdOmittedSection> omitted_sections;
};

struct DesignMdTokenDiff {
    std::vector<std::string> added;    // tokens present in `after` only
    std::vector<std::string> removed;  // tokens present in `before` only
    std::vector<std::string> modified; // tokens whose value changed
};

struct DesignMdDiffResult {
    DesignMdTokenDiff colors;
    DesignMdTokenDiff dimensions;
    DesignMdTokenDiff strings;
    // True if the `after` file has more error- or warning-severity
    // diagnostics than `before` (matches @google/design.md's diff
    // regression semantic).
    bool regression = false;
};

/// One asset entry from a Claude Design bundle.
struct ClaudeBundleAsset {
    std::string uuid;          ///< envelope key (also referenced as <script src="<uuid>">)
    std::string mime;          ///< e.g. "text/javascript", "font/woff2"
    std::vector<uint8_t> data; ///< already base64-decoded + gunzipped if compressed
};

/// One captured @font-face rule joined to a content-addressed bundle asset.
/// Weight/style remain descriptive today; the font's own tables are the
/// renderer authority for variant selection after registration.
struct ClaudeBundleFontBinding {
    std::string family;
    std::string runtime_family;
    size_t asset_index = 0;
    std::string weight;
    std::string style;
    std::string unicode_range;
};

/// One id-addressed binding from a v2 materialized browser document. The
/// native runtime currently consumes the executable HTML; retaining this
/// validated projection on the parsed bundle lets import/re-import tooling and
/// diagnostics inspect the exact capture evidence without reparsing the
/// sidecar. `payload_json` is the canonical JSON object for the binding.
struct ClaudeBundleMaterializedBinding {
    std::string id;
    std::string kind; ///< semantic, layout, text, paint, or canvas
    std::string payload_json;
};

/// Result of unpacking a Claude Design bundle's `<script type="__bundler/manifest">`
/// + `<script type="__bundler/template">` pair.
struct ClaudeBundle {
    std::vector<ClaudeBundleAsset> assets;  ///< all assets (JS, fonts, etc.) in manifest order
    std::vector<size_t> javascript_indices; ///< indices into assets[] of MIME `text/javascript`,
    ///< in the order the template's <script src> tags reference them
    std::vector<ClaudeBundleFontBinding> font_bindings; ///< captured @font-face assets
    std::vector<ClaudeBundleMaterializedBinding> materialized_bindings;
    std::uint32_t materialized_schema_version = 1;
    std::string template_html; ///< the unwrapped HTML template (with `<div id="root">` etc.)
};

struct MaterializedDocumentCacheStats {
    std::uint64_t verifies = 0; ///< full decode + verification passes run
    std::uint64_t hits = 0;     ///< calls served from an earlier verification
    std::uint64_t rejected = 0; ///< inputs that failed verification
    std::uint64_t waits = 0;    ///< lookups that waited for an in-flight verification
    std::size_t entries = 0;
    std::size_t bytes = 0; ///< input + decoded asset bytes held
};

/// Options for the `--execute-bundle` import lane.
struct ClaudeRuntimeOptions {
    /// Hard cap on bytes of bundled JS to evaluate. Bundles larger than
    /// this short-circuit back to the static parser so we don't crash
    /// the QuickJS parser stack on a 10 MB minified blob. Default ~6 MB
    /// covers a typical react.development + react-dom.development +
    /// app.bundle combo (~4.3 MB) with headroom.
    size_t max_total_js_bytes = 6 * 1024 * 1024;

    /// Override the JS engine backend. nullopt -> build default
    /// (QuickJS unless a PULP_DEFAULT_ENGINE_* compile option selects
    /// another backend). Useful for tests that want deterministic
    /// engine choice.
    std::optional<int> engine_override; // pulp::view::JsEngineType opaque to header

    /// If non-null, populated with a human-readable explanation when the
    /// runtime path bails out and the caller should fall back to the
    /// static parser. Empty when `parse_claude_html_with_runtime` returns
    /// a non-empty IR.
    std::string* error_out = nullptr;

    /// Optional viewport used when a runtime-native tree is frozen into
    /// DesignIR. When both values are positive, the bridge root is laid out
    /// at this size before computed view bounds are captured.
    int runtime_snapshot_viewport_width = 0;
    int runtime_snapshot_viewport_height = 0;
};

using ClaudeClassNameRules = std::map<std::string, std::map<std::string, std::string>>;

} // namespace pulp::view
