#pragma once

// pulp::design — semantic knobs over a design's parameters.
//
// A knob is one expressive control whose value drives a *bundle* of parameter
// writes at once: a "minimalism" slider that moves spacing, borders, shadows and
// accent saturation together; a theme flip; a layout-variant switch. The value
// of a knob is not a pixel — it is a named position, and each position carries
// the exact set of writes that position means.
//
// Knobs are declared per-design (data-driven: adding a knob is a JSON edit, not
// a C++ change), resolved to concrete writes here, and each write is classified
// against the design's bound theme tokens:
//
//   * a write whose key names a real theme token is a THEME-TOKEN write — apply
//     it at the token layer so the whole design re-tints coherently;
//   * every other write is a LOCAL override — persist it into the artifact's
//     own EDITMODE block (see design_tweaks.hpp) so it survives a reload and
//     lands in the diff, with local overriding theme at render time.
//
// The bound theme is passed in as a flat set of token names, not a whole
// DesignManifest, on purpose: the knob model needs to know only which keys are
// theme tokens, so it stays decoupled from the manifest/component/view headers
// and is a pure, deterministic core — no I/O, no window, no render context —
// unit-testable directly. A caller with a DesignManifest passes
// `manifest.tokens[*].name`. The inspector panel and the send-to-agent
// affordance are UI layers that drive these primitives and live elsewhere.

#include <pulp/design/design_tweaks.hpp>  // TweakParam, read/rewrite EDITMODE block

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::design {

/// The kind of control a knob presents. It changes only how a value selects a
/// position (see select_position); the resolved writes are identical machinery.
enum class KnobKind { Slider, Toggle, Enum };

/// One parameter write a knob position produces. `json_value` is the value's
/// JSON text exactly as it will serialize (a quoted string with its quotes, a
/// bare number, true/false) — the same contract as TweakParam::json_value, so a
/// rewrite never re-quotes or reformats.
struct KnobWrite {
    std::string key;
    std::string json_value;
};

/// One selectable position of a knob. For a Slider, `at` is the numeric anchor
/// on the knob's track and a value selects the nearest anchor. For a Toggle or
/// Enum, `at` is unused and a value selects by label (then by 0-based index).
struct KnobPosition {
    std::string label;
    double at = 0.0;
    std::vector<KnobWrite> writes;
};

/// A declared knob: a stable id, a human label, its kind, and its positions in
/// authored order (Slider positions are additionally sorted by `at` on parse).
struct KnobSpec {
    std::string id;
    std::string label;
    KnobKind kind = KnobKind::Enum;
    std::vector<KnobPosition> positions;
};

/// A design's declared set of knobs.
struct KnobSchema {
    std::vector<KnobSpec> knobs;
};

/// Parse a declared knob schema from JSON. Shape:
///   {"knobs":[{"id","label","kind":"slider|toggle|enum",
///              "positions":[{"label","at",writes:[{"key","value"}]}]}]}
/// Degrades: unknown object members are ignored; a knob missing an id, or a
/// position/write missing its key, is skipped rather than throwing. Returns
/// nullopt only on a hard shape error: the top level is not an object, or has no
/// "knobs" array.
std::optional<KnobSchema> parse_knob_schema(std::string_view json);

/// Serialize a schema to stable JSON. Round-trips with parse_knob_schema:
/// parse(to_json(s)) yields an equivalent schema.
std::string knob_schema_to_json(const KnobSchema& schema);

/// Look up a knob by id (nullptr when absent).
const KnobSpec* find_knob(const KnobSchema& schema, std::string_view id);

/// Select the position of `knob` for `value`:
///   * Slider — `value` is parsed as a number and the position with the nearest
///     `at` is chosen; a non-numeric `value` selects nothing.
///   * Toggle / Enum — `value` is matched against a position label
///     (case-sensitive); failing that, against a 0-based position index.
/// Returns nullptr when the knob has no positions or `value` selects none.
const KnobPosition* select_position(const KnobSpec& knob, std::string_view value);

/// Where a resolved write lands.
enum class WriteTarget { ThemeToken, LocalBlock };

/// Classify a write against the design's bound theme tokens: ThemeToken iff
/// `write.key` is one of `theme_tokens`, else LocalBlock. A caller with a
/// DesignManifest passes the token names (`manifest.tokens[*].name`).
WriteTarget classify_write(const KnobWrite& write,
                           const std::vector<std::string>& theme_tokens);

/// The concrete effect of setting a knob to a value: its writes split by target.
struct KnobEffect {
    std::vector<KnobWrite> token_writes;  ///< apply at the theme/token layer
    std::vector<KnobWrite> local_writes;  ///< persist into the EDITMODE block
};

/// Resolve `knob` set to `value` into its split writes against `theme_tokens`.
/// Returns nullopt when `value` selects no position.
std::optional<KnobEffect> resolve_knob(const KnobSpec& knob, std::string_view value,
                                       const std::vector<std::string>& theme_tokens);

/// The result of applying a knob to a design artifact.
struct KnobApply {
    std::string text;                     ///< artifact with the local writes persisted
    std::vector<KnobWrite> token_writes;  ///< the token-layer writes, for the caller to apply
};

/// Apply `knob` set to `value` to a design `artifact`: persist the local writes
/// into the artifact's EDITMODE block (updating keys in place, appending new
/// ones, preserving every byte outside the block) and hand back the token writes
/// for the caller to apply at the theme layer.
///
/// Returns nullopt when `value` selects no position, when a local write is not
/// serializable (e.g. a non-UTF-8 key), or when there are local writes to make
/// but the artifact has no well-formed EDITMODE block to anchor them. When the
/// value resolves to token writes only, the artifact is returned unchanged (no
/// block is required) alongside those token writes.
std::optional<KnobApply> apply_knob(std::string_view artifact, const KnobSpec& knob,
                                    std::string_view value,
                                    const std::vector<std::string>& theme_tokens);

/// Built-in theme-flip knob: an Enum over appearance with "light"/"dark"
/// positions, each writing a local `appearance` param. Appearance is a mode the
/// host recompiles the token set for, not a single token, so its write is local
/// by construction — which makes this the one archetype that is fully general
/// across designs and needs no per-design authoring. Minimalism and
/// layout-variant knobs are design-specific token bundles and ship as authored
/// schema, not built-ins.
KnobSpec builtin_theme_flip();

}  // namespace pulp::design
