#pragma once

/// @file lock_to_source.hpp
/// Phase 4a — Lock-to-source, Path A (generated-TSX/JS rewrite).
///
/// Spec: planning/2026-05-18-inspector-direct-manipulation-roadmap.md,
///       "Phase 4 — Lock-to-source" / "Phase 4a — Path A".
///
/// The inspector's direct-manipulation edits land in a tweaks layer
/// (`pulp-tweaks.json`, keyed by `stable_anchor_id`). "Lock to source" is
/// the explicit, opt-in promotion of a tweak back into the *authored*
/// source so the change is permanent and survives a fresh re-import.
///
/// Path A handles the **generated** import target: when `pulp
/// import-design` lowered a design into web-compat JS/TSX, that text is
/// owned by the user and carries machine-traceable provenance:
///
///   * a `// @pulp-anchor <id>` trail comment before every element
///     (emitted by `generate_pulp_js` when `include_comments` is on), and
///   * a `setAnchor(<var>._id, '<id>')` call binding the live widget.
///
/// Locking a tweak means: locate the element block for `anchor_id`, then
/// rewrite (or insert) the `el.style.<prop>` assignment that corresponds
/// to the tweak's dotted `property_path` so the generated file now
/// reflects the tweaked value. After a successful lock the tweak-store
/// entry can be retired — the value is baked into the source.
///
/// Scope discipline (per the roadmap): this is **Path A only**. Path B
/// (live-runtime React-bundle AST patch, #1308) and Path C (DESIGN.md
/// token export) are separate phases and are NOT implemented here.
///
/// The engine is pure text in / text out — no filesystem I/O — so it is
/// trivially unit-testable and the CLI / inspector layers own the
/// read-confirm-write loop and the `@generated`-boundary guard.

#include <pulp/view/design_import.hpp>

#include <optional>
#include <string>
#include <vector>

namespace pulp::view {

/// A single tweak to promote into source. Mirrors the tuple the inspector
/// tweak-store keys on: `(anchor_id, property_path)` → `value`.
///
/// `property_path` is the canonical dotted path used by `pulp-tweaks.json`
/// — e.g. `paint.backgroundColor`, `style.width`, `layout.padding`. The
/// engine maps the path onto the generated codegen's `el.style.<camel>`
/// line. See `lock_property_to_style_name()` for the exact mapping.
struct LockToSourceTweak {
    std::string anchor_id;
    std::string property_path;
    /// The tweaked value, already stringified the way it should appear in
    /// the generated source's JS string literal (e.g. "#5a5a5a", "12px",
    /// "120px"). Numeric layout dimensions should carry their unit.
    std::string value;
};

/// Outcome status for a single lock attempt.
enum class LockStatus {
    /// The property line existed and its value was rewritten.
    rewritten,
    /// No line for the property existed; a new `el.style.<prop>` line was
    /// inserted into the element block.
    inserted,
    /// The element block was found and the property already held exactly
    /// `value` — the source text is unchanged (idempotent re-lock).
    already_current,
    /// No `// @pulp-anchor <id>` comment matched `anchor_id`. The source
    /// text is returned unchanged. This is a graceful failure — the
    /// caller should leave the tweak in the sidecar.
    anchor_not_found,
    /// The `property_path` could not be mapped onto a generated style
    /// property (unsupported path). Source returned unchanged.
    unsupported_property,
};

/// Result of locking one tweak into a generated source string.
struct LockResult {
    LockStatus status = LockStatus::anchor_not_found;
    /// The (possibly rewritten) source text. On a non-mutating status
    /// (`already_current`, `anchor_not_found`, `unsupported_property`)
    /// this equals the input verbatim.
    std::string source;
    /// The CSS-style property name the path resolved to (camelCase, e.g.
    /// "backgroundColor"). Empty for `unsupported_property`.
    std::string style_property;
    /// Human-readable summary, suitable for CLI / inspector display.
    std::string message;
    /// 1-based line number of the rewritten / inserted assignment, or 0
    /// when nothing was located. Useful for a "jump to source" affordance.
    int line = 0;

    /// True when the lock changed the source text.
    bool mutated() const {
        return status == LockStatus::rewritten || status == LockStatus::inserted;
    }
    /// True when the anchor was located, regardless of whether a rewrite
    /// was needed (`already_current` counts as success — the source
    /// already carries the value).
    bool ok() const {
        return status == LockStatus::rewritten ||
               status == LockStatus::inserted ||
               status == LockStatus::already_current;
    }
};

/// Map a dotted tweak `property_path` (`paint.backgroundColor`,
/// `style.width`, `layout.padding`, …) to the camelCase CSS property name
/// the web-compat codegen emits as `el.style.<name>`.
///
/// Accepted path prefixes — all collapse to the same style surface:
///   * `paint.*`  — visual properties from the TS canonical IR.
///   * `style.*`  — the C++ `IRStyle` field surface.
///   * `layout.*` — box-model properties (padding, gap, width, height …).
///   * bare       — a path with no dot is taken as the property directly.
///
/// Returns `std::nullopt` for paths the generated codegen has no line for
/// (so the caller can report `unsupported_property` cleanly rather than
/// emitting a bogus assignment).
std::optional<std::string>
lock_property_to_style_name(const std::string& property_path);

/// Locate the element anchored by `tweak.anchor_id` in a generated
/// web-compat JS/TSX `source` string and rewrite (or insert) the style
/// assignment for `tweak.property_path` so it carries `tweak.value`.
///
/// The element block is delimited by its `// @pulp-anchor <id>` trail
/// comment and runs until the next `// @pulp-anchor` comment or a blank
/// line — whichever comes first. Within that block the engine looks for
/// `<var>.style.<prop> = '<old>';` and rewrites the literal; if no such
/// line exists it inserts one just before the block's `appendChild` /
/// `setAnchor` tail (or at the end of the block).
///
/// Pure function: never touches the filesystem, never throws.
LockResult lock_tweak_into_source(const std::string& source,
                                  const LockToSourceTweak& tweak);

/// Lock several tweaks into the same `source` in one pass. Each tweak is
/// applied to the running text so anchors stay valid even when two
/// tweaks target the same element. The returned vector is parallel to
/// `tweaks`; the final fully-rewritten text is in the last element's
/// `source` (and in every element's `source`, since they share the
/// running buffer at completion).
std::vector<LockResult>
lock_tweaks_into_source(const std::string& source,
                        const std::vector<LockToSourceTweak>& tweaks);

// ── WYSIWYG T5 — structural reparent lock-to-source ──────────────────────────
//
// A reflow-aware drag can "drop element A inside container B" — a STRUCTURAL
// tree edit, not a style tweak. In the generated web-compat artifact every
// element block ends with `<parentVar>.appendChild(<var>);` wiring it to its
// parent. Reparenting therefore means rewriting the dragged element's
// `appendChild` receiver from its old parent's var to the NEW parent's var.
//
// This is the structural sibling of lock_tweak_into_source(): it is a pure
// text-in/text-out rewrite over the same anchor-comment block model. It does
// NOT (yet) move the element block's TEXT to sit physically inside the new
// parent's block — the appendChild receiver rewrite is what changes the live
// DOM parent, and re-running the element block in source order still produces
// the reparented tree because createElement + appendChild are order-independent
// once the receiver is correct. Reordering the physical block position to match
// is a follow-up (see the T5 notes in the import-design SKILL).

/// A single structural reparent to promote into source: move the element
/// anchored by `child_anchor_id` to become a child of the element anchored by
/// `new_parent_anchor_id`.
struct ReparentToSourceEdit {
    std::string child_anchor_id;
    std::string new_parent_anchor_id;
};

/// Rewrite the dragged element's `appendChild` receiver so the generated source
/// wires it under the new parent. Locates the child block by its
/// `// @pulp-anchor` comment, finds its `<oldParent>.appendChild(<childVar>);`
/// line, resolves the new parent block's `const <var> = …` variable name, and
/// rewrites the receiver to that var. Status semantics mirror
/// lock_tweak_into_source: `rewritten` on success, `already_current` when the
/// child already appends to the new parent, `anchor_not_found` when either
/// anchor (or the appendChild line, or the parent's var) cannot be located.
/// `unsupported_property` is never returned. Pure function; never throws.
LockResult reparent_in_source(const std::string& source,
                              const ReparentToSourceEdit& edit);

/// Heuristic guard for the `@generated`-boundary rule (roadmap: "only
/// lock into files marked `@generated`"). Pulp's `generate_pulp_js`
/// emits `// Generated by Pulp import-design from <source>` as the first
/// line; an `// @generated` marker is also accepted for files that adopt
/// the conventional banner. Returns true when `source` looks like a
/// Pulp-generated import artifact safe to rewrite.
bool is_generated_source(const std::string& source);

}  // namespace pulp::view
