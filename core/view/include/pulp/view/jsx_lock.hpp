#pragma once

/// @file jsx_lock.hpp
/// Phase 4b — Lock-to-source, Path B (hand-authored JSX/TSX patch).
///
/// Spec: planning/2026-05-18-inspector-direct-manipulation-roadmap.md,
///       "Phase 4 — Lock-to-source" / "Phase 4b — Path B (JSX/TSX AST
///       patch via element instrumentation)". GitHub issue #1308.
///
/// The inspector's direct-manipulation edits land in a tweaks layer
/// (`pulp-tweaks.json`, keyed by `stable_anchor_id`). "Lock to source" is
/// the explicit, opt-in promotion of a tweak back into the *authored*
/// source so the change survives a fresh re-import.
///
/// Where Phase 4a (`lock_to_source.hpp`) rewrites the *generated*
/// web-compat JS that `pulp import-design` emits, Phase 4b handles the
/// other half of the lock-to-source contract: the user is running a
/// **live React bundle** built from a hand-authored JSX/TSX file
/// (`pulp import-design --from jsx`, `--execute-bundle`). That JSX/TSX
/// is the user's own source — there is no generated artifact to rewrite.
///
/// ## How an authored element is located
///
/// At JSX-transform time an element-instrumentation pass injects a
/// `data-pulp-anchor="<stable_anchor_id>"` attribute onto each authored
/// element (the JS-side instrumentation is a separate deliverable; this
/// engine only consumes the marker). At lock time the engine finds the
/// opening JSX tag carrying `data-pulp-anchor="<anchor_id>"` and patches
/// the targeted prop.
///
/// ## Patch strategy — surgical, not a full AST re-emit
///
/// Phase 4a and 4c deliberately do *minimal* rewrites: they locate the
/// exact span that owns the value and rewrite only that span, preserving
/// every other byte (prose, comments, formatting, key order). Phase 4b
/// mirrors that discipline. It is NOT a general JSX-to-JSX AST printer.
/// It performs a tightly-scoped, anchored text edit:
///
///   * `style={{ padding: 8, background: '#888' }}` — the engine finds
///     the matching object property inside the inline-style literal and
///     rewrites just that property's value.
///   * `width={80}` / `width="80"` / `color="#888"` — a bare prop whose
///     value is a literal: the engine rewrites the literal in place.
///
/// Codex's caveat on the roadmap (lines 126-128, 483, 524) is explicit:
/// Path B AST patching must not balloon into a multi-quarter parser.
/// Anything the engine cannot patch *safely and unambiguously* — a
/// spread (`style={{ ...base }}`), a computed key, a non-literal value
/// (`padding={gap * 2}`, `color={theme.fg}`), a className-driven style,
/// a duplicate anchor — is reported as a typed failure so the caller
/// leaves the tweak in the sidecar with a "too dynamic to lock" message.
///
/// ## Conservatism contract (mirrors Phase 4a/4c)
///
/// If the target element or prop cannot be located *unambiguously*, the
/// lock FAILS and the source is returned byte-identical. The engine
/// never guesses and never emits a malformed edit. It is a pure
/// function: no filesystem I/O, never throws.

#include <optional>
#include <string>
#include <vector>

namespace pulp::view {

/// A single tweak to promote into authored JSX/TSX source. Mirrors the
/// tuple the inspector tweak-store keys on: `(anchor_id, property_path)`
/// → `value`, identical in shape to `LockToSourceTweak` (Phase 4a).
struct JsxLockTweak {
    /// The tweak's `stable_anchor_id` — matched against the element's
    /// injected `data-pulp-anchor="<id>"` attribute.
    std::string anchor_id;
    /// The canonical dotted property path used by `pulp-tweaks.json`
    /// (e.g. `paint.backgroundColor`, `style.width`, `layout.padding`).
    /// The leading `paint.` / `style.` / `layout.` namespace is optional
    /// and collapses onto the JSX `style={{…}}` / prop surface.
    std::string property_path;
    /// The tweaked value as it should appear in source. A value that
    /// looks numeric (optionally with a unit, e.g. `12`, `0.5`) is
    /// emitted as the right JS literal for the prop's existing form; a
    /// string value (`#5a5a5a`, `Mike's Font`) is emitted quoted.
    std::string value;
};

/// Outcome status for a single JSX/TSX lock attempt.
enum class JsxLockStatus {
    /// The prop existed and its literal value was rewritten.
    patched,
    /// The element and prop were found and the prop already held exactly
    /// `value` — source unchanged (idempotent re-lock).
    already_current,
    /// No element carried `data-pulp-anchor="<anchor_id>"`. Source
    /// returned unchanged. Graceful failure — leave the tweak in sidecar.
    anchor_not_found,
    /// More than one element carried the same `data-pulp-anchor`. The
    /// engine refuses to guess. Source unchanged.
    anchor_ambiguous,
    /// The `property_path` does not map onto a lockable JSX prop /
    /// style key. Source unchanged.
    unsupported_property,
    /// The element was found but the targeted prop is "too dynamic to
    /// lock" — its value is a spread, a computed key, a non-literal
    /// expression (`{gap * 2}`, `{theme.fg}`), or otherwise not a plain
    /// literal the engine can safely rewrite. Source unchanged. The
    /// caller should keep the tweak in the sidecar and surface the
    /// message verbatim.
    too_dynamic,
};

/// Result of locking one tweak into an authored JSX/TSX source string.
struct JsxLockResult {
    JsxLockStatus status = JsxLockStatus::anchor_not_found;
    /// The (possibly patched) source text. On any non-`patched` status
    /// this equals the input verbatim.
    std::string source;
    /// The resolved style / prop key the path mapped to (camelCase, e.g.
    /// "backgroundColor", "padding"). Empty when the path was
    /// unsupported.
    std::string property;
    /// Human-readable summary, suitable for CLI / inspector display.
    /// On `too_dynamic` this carries the specific reason.
    std::string message;
    /// 1-based line number of the patched value, or 0 when nothing was
    /// patched. Useful for a "jump to source" / diff-preview affordance.
    int line = 0;

    /// True when the lock changed the source text.
    bool mutated() const { return status == JsxLockStatus::patched; }
    /// True when the element + prop were located and the source carries
    /// the value (`already_current` counts — the value is already there).
    bool ok() const {
        return status == JsxLockStatus::patched ||
               status == JsxLockStatus::already_current;
    }
};

/// Map a dotted tweak `property_path` (`paint.backgroundColor`,
/// `style.width`, `layout.padding`, a bare `width`, …) to the camelCase
/// JSX style / prop key the engine looks for in `style={{…}}` or as a
/// bare attribute.
///
/// Accepted leading namespaces — all collapse to the same prop surface:
///   * `paint.*`  — visual properties.
///   * `style.*`  — the C++ `IRStyle` field surface.
///   * `layout.*` — box-model properties.
///   * bare       — a path with no dot is taken as the key directly.
///
/// Returns `std::nullopt` for paths with an unknown namespace, an empty
/// leaf, a nested leaf (`layout.padding.top`), or a key that is not on
/// the lockable allow-list — so the caller reports `unsupported_property`
/// cleanly instead of producing a bogus edit.
std::optional<std::string>
jsx_lock_property_to_key(const std::string& property_path);

/// Heuristic guard: does `source` look like *hand-authored* JSX/TSX (as
/// opposed to a Pulp-generated web-compat artifact)? Phase 4b only locks
/// into authored source; a file that carries the `generate_pulp_js`
/// banner or an `@generated` marker belongs to Phase 4a instead. Returns
/// true when the source contains JSX (`<Tag …>` / `</Tag>` / `<Tag/>`)
/// and is NOT flagged generated.
bool is_authored_jsx_source(const std::string& source);

/// Locate the JSX element carrying `data-pulp-anchor="<tweak.anchor_id>"`
/// in an authored JSX/TSX `source` string and surgically rewrite the
/// literal value of the prop named by `tweak.property_path` so it
/// carries `tweak.value`.
///
/// Behavior:
///   - If the prop appears as a key inside an inline `style={{…}}`
///     object literal, the matching property's value is rewritten.
///   - Otherwise, if a bare attribute of that name exists
///     (`width={80}` / `color="#888"`), its literal value is rewritten.
///   - String-shaped values are emitted quoted; numeric-shaped values
///     are emitted as bare JS number literals when the existing prop
///     used a `{…}` expression, preserving the author's literal form.
///   - Fails — source unchanged — on: no/duplicate anchor, an
///     unsupported property path, or a prop whose value is not a plain
///     literal the engine can rewrite (`too_dynamic`).
///
/// Pure function: never touches the filesystem, never throws.
JsxLockResult jsx_lock_tweak_into_source(const std::string& source,
                                         const JsxLockTweak& tweak);

/// Lock several tweaks into the same authored `source` in one pass. Each
/// tweak is applied to the running text so anchors stay valid even when
/// two tweaks target the same element. The returned vector is parallel
/// to `tweaks`; the fully-patched text is in every element's `source`
/// (they share the running buffer at completion).
std::vector<JsxLockResult>
jsx_lock_tweaks_into_source(const std::string& source,
                            const std::vector<JsxLockTweak>& tweaks);

}  // namespace pulp::view
