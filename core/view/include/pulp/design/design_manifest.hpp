#pragma once

// pulp::design — compiled design contract.
//
// A DesignManifest is the closed set of tokens and components a generated or
// imported UI is allowed to bind to. It is compiled from the buildable sources
// of truth — a Theme (the token allowlist) and the component catalog (each
// widget's native class plus the exact theme tokens it paints through) — into
// one deterministic artifact. Two consumers use it:
//
//   1. A binding prompt (Markdown) embedded in an importer / codegen prompt, so
//      the model that writes UI JS is told up front which tokens and widgets are
//      real. Inventing a token name or targeting an unknown widget is then a
//      visible contract break rather than a silent drift.
//   2. An adherence check that validates already-generated UI against the same
//      manifest — the prompt and the check share one source of truth.
//
// The manifest is deterministic: every list is sorted by name and serialized in
// a fixed key order, so it diffs cleanly in review and can gate CI.

#include <pulp/design/design_system.hpp>  // ComponentInfo, catalog()
#include <pulp/view/theme.hpp>            // Theme

#include <string>
#include <string_view>
#include <vector>

namespace pulp::design {

/// The on-disk format identifier stamped into DesignManifest::manifest_version.
/// Bump when the manifest JSON shape changes in a non-additive way.
inline constexpr std::string_view kManifestVersion = "2026.07-design-manifest-v1";

/// One token in the compiled allowlist: a name an importer/codegen may bind to,
/// its kind, and its value in the chosen appearance.
struct ManifestToken {
    std::string name;   // e.g. "color.bg", "radius.md", "font.family"
    std::string kind;   // "color" | "dimension" | "string"
    std::string value;  // "#rrggbb"/"#rrggbbaa" | trimmed number | verbatim string
};

/// One component contract: a widget an importer may target and the exact theme
/// tokens it legitimately paints through (its reskin allowlist).
struct ManifestComponent {
    std::string name;             // "Knob"
    std::string category;         // category_name(...)
    std::string native_class;     // "pulp::view::Knob"
    std::string header;           // "pulp/view/widgets.hpp"
    std::string figma_component;  // component-set name in the authored library
    std::string usage;            // one-line intent
    std::vector<std::string> reskin_tokens;  // token allowlist for this widget
};

/// The compiled design contract. All vectors are sorted by name so the artifact
/// is byte-stable for a given (theme, catalog) pair.
struct DesignManifest {
    std::string manifest_version;   // kManifestVersion
    std::string source;             // provenance ("ink-signal", a DESIGN.md path, ...)
    std::string appearance;         // "light" | "dark"
    int theme_schema_version = 0;   // Theme::kSchemaVersion the tokens follow
    std::vector<ManifestToken> tokens;
    std::vector<ManifestComponent> components;
};

/// Compile a manifest from an explicit theme + component set. `source` and
/// `appearance` are recorded as provenance; the token values come from `theme`.
DesignManifest compile_design_manifest(const pulp::view::Theme& theme,
                                       const std::vector<ComponentInfo>& components,
                                       std::string source,
                                       std::string appearance);

/// Compile from the built-in Ink & Signal system: its theme for the requested
/// appearance plus the full component catalog.
DesignManifest compile_ink_signal_manifest(bool dark = false);

/// Serialize to stable, pretty JSON. Object keys and array order are fixed so
/// the output is byte-deterministic for a given manifest.
std::string manifest_to_json(const DesignManifest& manifest);

/// Parse a manifest back from the JSON that manifest_to_json produces. Missing
/// or malformed fields degrade to empty/default rather than throwing, so a
/// partial file still yields a usable (if smaller) contract. Round-trips:
/// manifest_from_json(manifest_to_json(m)) preserves tokens and components.
DesignManifest manifest_from_json(const std::string& json);

/// Emit an LLM-ready binding prompt (Markdown): the closed token allowlist and
/// the component contracts, with an explicit adherence directive. This is the
/// text an importer/codegen prompt embeds so generated UI binds only to real
/// tokens and widgets.
std::string emit_binding_prompt(const DesignManifest& manifest);

}  // namespace pulp::design
