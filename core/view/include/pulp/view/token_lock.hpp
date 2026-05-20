#pragma once

/// @file token_lock.hpp
/// Phase 4c of the inspector direct-manipulation roadmap
/// (planning/2026-05-18-inspector-direct-manipulation-roadmap.md):
/// **token lock-to-source via DESIGN.md export**.
///
/// When a user makes an inspector tweak that corresponds to a design
/// *token* — a palette color, a spacing-scale step, a corner radius, a
/// typography field — "locking" that tweak should write the corrected
/// value back into the project's `DESIGN.md`, so the next re-import from
/// the design tool (Figma / Stitch / v0 / Pencil / Claude) picks up the
/// fixed token rather than re-introducing the stale one.
///
/// This is the token-level sibling of Phase 4a (lock-to-source for
/// generated TSX) and Phase 4b (JSX/TSX AST patch). Where 4a/4b rewrite
/// *element* source, 4c rewrites a single *token value* in the YAML
/// frontmatter of DESIGN.md.
///
/// Design rationale — why a surgical text rewrite, not `export_designmd`:
///   `export_designmd(Theme, ...)` (design_export.hpp) re-emits a whole
///   DESIGN.md from a Theme and is gated on pulp #1307. Even ungated it
///   would be the wrong tool here: it cannot preserve the author's prose
///   sections, comment placement, key ordering, or original dimension
///   spelling ("1.5rem" vs "24px"). Locking ONE token must not reflow
///   the entire file. So `lock_token_in_designmd` instead parses the
///   frontmatter to *locate* the token, then performs a minimal
///   value-only edit on the original text — every other byte of the
///   file (prose, comments, ordering, indentation) is preserved.
///
/// Conservatism contract (mirrors Phase 4a error handling): if the token
/// cannot be located *unambiguously* in DESIGN.md the lock FAILS with a
/// clear message rather than guessing. A failed lock leaves DESIGN.md
/// byte-identical and the tweak untouched in `pulp-tweaks.json`.

#include <optional>
#include <string>
#include <vector>

namespace pulp::view {

// ── Token classification ────────────────────────────────────────────────
//
// Not every inspector tweak is a token. A one-off element edit ("make
// THIS knob 4px wider") is an element tweak — it goes through Phase 4a/4b.
// A token tweak edits a value the *whole design system* shares.
//
// Two signals classify a tweak as token-typed, in priority order:
//   1. Anchor convention. Token tweaks carry an anchor of the form
//      `designtoken:<group>.<name>` (e.g. `designtoken:colors.primary`).
//      The inspector stamps this anchor when the selected property is
//      backed by a resolved design token rather than a literal.
//   2. Property-path convention. A dotted path whose first segment is a
//      canonical DESIGN.md token group (`colors`, `spacing`, `rounded`,
//      `typography`) is treated as a token path even without the anchor
//      prefix — this lets the CLI and tests address tokens directly.
//
// Element-only paths (`paint.*`, `layout.*`, `text.*`) never classify as
// tokens; `classify_token_tweak` returns std::nullopt for them.

/// The four canonical DESIGN.md frontmatter token groups Phase 4c can
/// lock into. `components` is intentionally excluded — a component entry
/// is a *reference bundle*, not a primitive token value, and locking one
/// would mean rewriting a `{group.name}` reference, which Phase 4c does
/// not attempt.
enum class TokenGroup {
    colors,      ///< `colors:` map — hex string values.
    spacing,     ///< `spacing:` map — dimension values.
    rounded,     ///< `rounded:` map — corner-radius dimension values.
    typography,  ///< `typography:` map — nested per-level field maps.
};

/// Stringify a TokenGroup as its DESIGN.md frontmatter key.
const char* token_group_key(TokenGroup group);

/// A tweak classified as targeting a design token.
struct TokenTarget {
    TokenGroup group;
    /// The token name within the group, e.g. "primary", "md", "lg".
    std::string name;
    /// For `typography` tokens only: the field within the level map,
    /// e.g. "fontSize", "fontWeight", "lineHeight". Empty for the other
    /// three groups (they are flat scalar maps).
    std::string field;
};

/// Classify a tweak as token-typed or not.
///
/// @param anchor_id      The tweak's `stable_anchor_id` (may be empty).
/// @param property_path  The tweak's dotted property path.
///
/// Returns a populated TokenTarget when the tweak addresses a DESIGN.md
/// token, std::nullopt when it is an element-only tweak (the Phase 4a/4b
/// domain). Classification is purely structural — it does NOT check
/// whether the token actually exists in any DESIGN.md.
std::optional<TokenTarget> classify_token_tweak(const std::string& anchor_id,
                                                 const std::string& property_path);

// ── Lock result ─────────────────────────────────────────────────────────

/// Outcome of a `lock_token_in_designmd` call. `ok` is the only success
/// path; on failure `updated_markdown` equals the input byte-for-byte.
struct TokenLockResult {
    bool ok = false;
    /// Human-readable failure reason when `ok` is false; empty on success.
    std::string error;
    /// The rewritten DESIGN.md text. On success this differs from the
    /// input by exactly the one token value; on failure it is the input
    /// unchanged (callers may write it back unconditionally and be safe).
    std::string updated_markdown;
    /// The value found at the token before the rewrite — lets the
    /// inspector show a "was X, now Y" confirmation and supports an
    /// eventual Unlock (inverse delta). Empty on failure.
    std::string previous_value;
    /// 1-based line number within DESIGN.md that was rewritten; 0 on
    /// failure. Useful for a diff-preview UI.
    int line = 0;
};

/// Lock a single token tweak back into DESIGN.md text.
///
/// @param markdown       The current DESIGN.md file contents.
/// @param target         The classified token (see classify_token_tweak).
/// @param new_value      The corrected value to write. For `colors` this
///                       is a hex string ("#5a5a5a"); for `spacing` /
///                       `rounded` a dimension string ("12px", "1rem");
///                       for `typography` the raw field value.
///
/// Behavior:
///   - Parses the YAML frontmatter to confirm the token exists and is
///     reachable unambiguously.
///   - Rewrites ONLY that token's value in the original text, preserving
///     every other byte (prose, comments, key order, indentation, the
///     value's surrounding quotes if present).
///   - Fails (ok=false, markdown unchanged) when:
///       * DESIGN.md has no YAML frontmatter,
///       * the token group is absent,
///       * the token name is absent from its group,
///       * a `typography` target's field is absent from the level,
///       * the token line cannot be located unambiguously in the source
///         text (e.g. the same `name:` key appears under more than one
///         group and the parse cannot disambiguate).
///
/// The function never throws — all failure modes are reported via the
/// result's `error` string.
TokenLockResult lock_token_in_designmd(const std::string& markdown,
                                        const TokenTarget& target,
                                        const std::string& new_value);

/// Convenience overload: classify + lock in one call. Returns a failure
/// result with a descriptive error when the tweak is not token-typed.
TokenLockResult lock_token_in_designmd(const std::string& markdown,
                                        const std::string& anchor_id,
                                        const std::string& property_path,
                                        const std::string& new_value);

}  // namespace pulp::view
