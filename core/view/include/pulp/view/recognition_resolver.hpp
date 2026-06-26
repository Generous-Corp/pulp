#pragma once

/// @file recognition_resolver.hpp
/// The single merge layer for KEY-BASED control recognition in the design
/// importer.
///
/// ── Why one module ──────────────────────────────────────────────────────────
/// Control recognition draws from MULTIPLE sources that all answer the same
/// question — "given a Figma `component_set_key` (or a layer-name prefix), which
/// Pulp control kind / custom factory does it materialize?" Today there are two
/// such sources:
///
///   1. the built-in Pulp Figma Library (tools/figma-plugin/library-manifest.json),
///   2. a USER-supplied recognition manifest (the designer's OWN component-set
///      keys → Pulp kinds), passed via `pulp import-design --recognition-manifest`.
///
/// A THIRD source — installed-package `design_controls` fragments → registered
/// custom-control `factory_id`s — is coming in issue #4677. The whole point of
/// this module is that #4677 adds that source by calling `add_source()` ONCE,
/// here, instead of threading a third lookup through the importer's parse lanes.
///
/// `RecognitionResolver` is that one place: it combines an ordered list of
/// `RecognitionSource`s into one resolved `component_set_key → kind` (and
/// `→ factory_id`) table, with later sources MERGED OVER earlier ones on key
/// collision. The built-in library is always source 0 (lowest precedence); the
/// user manifest is added over it; package fragments will be added over both.
///
/// ── Authoritative lane ──────────────────────────────────────────────────────
/// The C++ figma-plugin lane (design_import.cpp::parse_figma_plugin_json) is the
/// authoritative consumer: the plugin envelope carries each instance's
/// `figma.component_key` / `figma.main_component_name` EVEN WHEN the in-Figma TS
/// plugin did not recognize it (a third-party component). So re-resolving here,
/// after parse, is where a user manifest takes effect without touching the
/// in-Figma plugin. The TS plugin (extract.ts) and the Python REST lane
/// (figma_rest_export.py) bake recognition for the BUILT-IN library at capture
/// time; accepting a user manifest in those lanes too is a follow-up.
///
/// ── Never-silent-knob contract ───────────────────────────────────────────────
/// A component instance the resolver does NOT match is left exactly as parsed —
/// it is never guessed into a wrong kind. `apply_to_figma_plugin_ir` reports the
/// unmatched-but-present component instances so they surface in the import
/// report instead of being silently dropped.

#include <pulp/view/design_ir.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::view {

/// One recognition source: a flat map of a designer's component identity to the
/// Pulp control it materializes. Mirrors the `library-manifest.json` widget
/// shape (component_set_key + optional name_prefix), generalized to also carry a
/// custom-control `factory_id` for the #4677 installed-package lane.
struct RecognitionEntry {
    /// Pulp built-in control kind. `AudioWidgetType::none` means "this entry maps
    /// to a custom factory, not a built-in widget" — `factory_id` is then set.
    AudioWidgetType kind = AudioWidgetType::none;
    /// Figma published component-set key (authoritative identity). Empty when the
    /// entry is name-prefix only.
    std::string component_set_key;
    /// Optional layer/component name prefix fallback (case-insensitive), used
    /// when no key matched. Empty disables the prefix fallback for this entry.
    std::string name_prefix;
    /// #4677 (custom controls): the registered factory id this identity resolves
    /// to. Empty for built-in-kind entries. When set, a resolved control is a
    /// custom overlay built by `register_design_control_factory(factory_id)`.
    std::string factory_id;
};

/// A named bundle of recognition entries from one origin (built-in library,
/// user manifest, or — later — an installed package). The name is carried into
/// diagnostics so a wired control's provenance is traceable.
struct RecognitionSource {
    std::string name;  ///< e.g. "builtin-library", "user-manifest", "<pkg>"
    std::vector<RecognitionEntry> entries;
};

/// The result of resolving one node's identity against the merged table.
struct ResolvedControl {
    bool matched = false;
    AudioWidgetType kind = AudioWidgetType::none;
    std::string factory_id;       ///< non-empty only for custom-control matches
    std::string source_name;      ///< which source won the match
    /// How it matched: "key" (authoritative) or "name_prefix" (fallback).
    std::string via;
};

/// Map an `AudioWidgetType` to/from the lowercase id used in manifests and the
/// figma-plugin envelope ("knob"/"fader"/...). Shared so the resolver and the
/// envelope parser stay in lockstep. Returns `none` for unknown ids.
AudioWidgetType audio_widget_kind_from_manifest_id(const std::string& id);
const char* audio_widget_kind_to_manifest_id(AudioWidgetType kind);

/// The single merge layer. Build it, add sources in precedence order (built-in
/// first), then resolve. Resolution precedence inside the merged table:
///   1. component_set_key exact match (authoritative). Later sources win.
///   2. name_prefix case-insensitive prefix match (fallback). Later sources win.
class RecognitionResolver {
public:
    /// Seed source 0 with the built-in Pulp Figma Library
    /// (library-manifest.json), so the built-in path is always authoritative
    /// when no user manifest overrides a key. This mirrors the manifest shape in
    /// code so the C++ importer can re-resolve keys without reading the JSON at
    /// runtime; the table is verified against the JSON by a unit test.
    static RecognitionResolver with_builtin_library();

    /// Empty resolver (no built-in seed) — used by tests and by callers that
    /// want full control over the source list.
    RecognitionResolver() = default;

    /// Append a source. Later sources override earlier ones on key/prefix
    /// collision. Returns *this for chaining.
    RecognitionResolver& add_source(RecognitionSource source);

    /// Parse a recognition manifest in the flat `library-manifest.json` shape:
    ///   { "widgets": { "<name>": { "kind"?, "component_set_key", "name_prefix"?,
    ///                              "factory_id"? }, ... } }
    /// `kind` defaults to the widget's map key when omitted (so the built-in
    /// shape, which keys by kind, parses directly). On parse failure returns
    /// nullopt and, if `error_out` is non-null, sets a human-readable reason.
    static std::optional<RecognitionSource> parse_manifest_json(
        const std::string& json,
        const std::string& source_name,
        std::string* error_out = nullptr);

    /// Resolve one node's identity. `component_key` is the authoritative signal;
    /// `name` feeds the prefix fallback. An empty/unmatched identity returns
    /// `matched == false`.
    ResolvedControl resolve(const std::string& component_key,
                            const std::string& name) const;

    /// Whether any source contributed at least one entry.
    bool empty() const { return sources_.empty(); }

private:
    std::vector<RecognitionSource> sources_;
};

/// Apply the resolver to a parsed figma-plugin IR tree IN PLACE: for every node
/// that carries a Figma component identity (attributes `figmaComponentKey` /
/// `figmaMainComponentName`, stamped by parse_ir_node) but is NOT already a
/// recognized audio widget, resolve it against the merged table and stamp
/// `audio_widget` (built-in kinds) on a match. Custom-factory matches are
/// recorded for the materializer via the node's attributes.
///
/// Never overrides an existing `audio_widget` (the built-in TS lane already
/// recognized it) — strictly additive. A component instance present but matched
/// by NO source is collected into `unmatched_out` (deduplicated by key) so the
/// caller can surface it in the import report — never silently guessed.
///
/// Returns the number of nodes newly wired by the resolver.
struct UnmatchedComponent {
    std::string component_key;
    std::string name;
};
int apply_recognition_resolver(IRNode& root,
                               const RecognitionResolver& resolver,
                               std::vector<UnmatchedComponent>* unmatched_out = nullptr);

// ── Installed-package custom-control source ───────────────────────────────────
// Gather every installed package's `design_controls` fragments into recognition
// sources the resolver consumes. A package declares custom controls in its
// registry entry under `design_controls`: an array of
//   { "factory_id": "<id>", "component_set_key"?: "<key>", "name_prefix"?: "<p>" }
// each mapping a Figma identity to the `factory_id` the package's
// `pulp::view::View` registers via `register_design_control_factory`. The merge
// reads two project-local files:
//   - the lockfile (packages.lock.json): WHICH packages are installed, by id;
//   - the registry (registry.json): each package's `design_controls` fragment.
// One `RecognitionSource` is produced per installed package that declares any
// design control, named by package id so a wired control's provenance is
// traceable. A package with no `design_controls` contributes nothing — so with
// no custom-control package installed the result is empty and behavior is
// unchanged.
//
// Reads files directly with choc::json (no CLI dependency) so the importer can
// build the merged table without linking the package CLI. Missing/empty/invalid
// files yield an empty result (with an optional human-readable warning), never
// an error — a malformed registry must not break an import.
struct PackageDesignControlSources {
    std::vector<RecognitionSource> sources;
    std::vector<std::string> warnings;  ///< non-fatal parse/skip notes
};

// Build the per-package custom-control sources from explicit file paths. Either
// file may be absent. Use this overload in tests and when the paths are known.
PackageDesignControlSources gather_package_design_controls(
    const std::filesystem::path& lockfile_path,
    const std::filesystem::path& registry_path);

// Discover the project's packages.lock.json and registry.json by walking up
// from `start_dir` (looking for a sibling pair), then gather. Returns an empty
// result when no lockfile is found. Convenience wrapper used by the importer.
PackageDesignControlSources discover_package_design_controls(
    const std::filesystem::path& start_dir);

// ── Materialize half: stamp resolved custom controls onto the IR ─────────────
// Walk the IR and, for every node the resolver stamped with a custom-control
// `recognitionFactoryId` (see apply_recognition_resolver), append a
// `kind=custom` IRInteractiveElement carrying that `factory_id` so the native
// materializer builds the registered overlay — or renders inert + diagnoses
// when the factory is unregistered (never a silent knob). Geometry
// is taken from the node's own box; `source_node_id` is carried so the
// inspector can map the control back to its design node.
//
// Idempotent and strictly additive: a node that already carries a custom
// interactive element for the same factory_id is not double-stamped, and a node
// without a `recognitionFactoryId` is untouched. Returns the number of custom
// elements newly materialized.
int materialize_recognized_custom_controls(IRNode& root);

} // namespace pulp::view
