#pragma once

/// @file design_shortcuts.hpp
/// Keyboard-shortcut import for designs: static-scan extracted shortcuts,
/// platform-convention default shortcuts (detect / apply / serialize), and
/// the small import heuristics (bundler-entry detection, audio-widget naming)
/// shared by the import pipeline.

#include <pulp/view/design_ir.hpp>  // AudioWidgetType (detect_audio_widget return)
#include <string>
#include <vector>

namespace pulp::view {

// ── Keyboard shortcut import (UX best-practice default) ─────────────────
//
// React designs commonly declare keyboard shortcuts inline via
// `onKeyDown={e => { if (e.key === 'Escape') ... }}`,
// `window.addEventListener('keydown', e => { if (e.metaKey && e.key === 's') ... })`,
// or `useHotkeys('cmd+s', ...)`. The design-import path scans the bundled
// React source for these patterns and emits a manifest the runtime can
// register via `registerShortcut(key, modifiers, callback)`. Default-on;
// opt out with `--no-import-shortcuts` (CLI) for designs where the host
// owns shortcut handling.

struct DetectedShortcut {
    /// Verbatim pattern as it appeared in source — `"e.key === 'Escape'"`,
    /// `"e.metaKey && e.key === 's'"`. Preserved for the manifest so a
    /// reviewer can audit the match without re-grepping source.
    std::string pattern;

    /// Human-readable key name in DOM-spec form: `"Escape"`, `"s"`, `"ArrowLeft"`.
    /// Empty if the pattern's key reference couldn't be resolved (e.g. the
    /// handler dispatches on a runtime variable instead of a literal).
    std::string key;

    /// Modifier names normalized to the extractor's emission order:
    /// `"meta"`, `"ctrl"`, `"alt"`, `"shift"`. The cross-platform
    /// `e.metaKey || e.ctrlKey` idiom emits both `meta` and `ctrl` so codegen
    /// can bind Cmd and Ctrl as distinct physical chords.
    std::vector<std::string> modifiers;

    /// Best-effort source location for the matched pattern, in `<file>:<line>`
    /// form when the input source carries filename hints (Claude bundles do;
    /// raw TSX/JS strings don't — caller can pass an empty filename).
    std::string source_location;

    /// Best-effort handler-body excerpt — the first ~80 chars after the
    /// condition match. Surfaced in the manifest so a reviewer can decide
    /// whether the shortcut is safe to auto-wire vs needs human triage.
    std::string handler_excerpt;
};

/// Static-scan a TSX/JS source string for keyboard-shortcut patterns.
/// Recognized forms:
///   * `if (e.key === 'X') ...` / `if (e.code === 'X') ...`
///   * `e.metaKey`, `e.ctrlKey`, `e.altKey`, `e.shiftKey` (singular or
///     combined with `&&` or `||`)
///   * Inline `onKeyDown={e => {...}}` JSX prop bodies
///   * `window.addEventListener('keydown', handler)` /
///     `document.addEventListener('keydown', handler)`
/// Pure regex / lexical pass — does NOT attempt to evaluate handler bodies
/// or resolve dynamic `e.key` references. Returns an empty vector when no
/// patterns match. Never throws. The `filename` arg is woven into each
/// shortcut's `source_location` for traceability; pass `""` if unknown.
std::vector<DetectedShortcut> extract_keyboard_shortcuts(
    const std::string& source, const std::string& filename = "");

/// Serialize a list of `DetectedShortcut`s to JSON in the caller-provided order.
/// `extract_keyboard_shortcuts` already sorts by (key, modifiers,
/// source_location) before returning its deterministic scan result.
std::string serialize_detected_shortcuts(const std::vector<DetectedShortcut>& shortcuts);

// ── Default shortcuts (source-matched only) ─────────────────────────────
//
// On top of the explicit-source extractor (which captures shortcuts the
// developer *already wrote*), the default-shortcuts pass adds bindings the
// developer *would expect* per platform convention but didn't write: `Cmd+,` for
// Settings, `Cmd+?` / `F1` for Help, bare `?` for a shortcut cheatsheet,
// `Cmd+S` for Save, etc.
//
// Hard rule: a wrong auto-binding is worse than no binding. The detector
// requires AT LEAST TWO independent signals (component name, canonical name,
// ARIA role/label, modal heading, content shape) before firing, and emits a
// `default_collision` entry instead of a binding when multiple modals look like
// the same pattern.
//
// What this pass does NOT cover: Pulp-framework-provided surfaces (Audio /
// MIDI Settings tabs in standalone). Those need a separate codegen path that
// drives the TabPanel select-tab API and is gated on the build mode.

/// Which platform-convention surface a default binding maps to. Used by
/// `detect_default_shortcuts` to label matches and by `generate_pulp_js`
/// to skip Pulp-framework-provided rows that don't exist on the target.
enum class DefaultShortcutPattern {
    settings,    // Settings / Preferences modal
    help,        // Help / About / Documentation modal (prose-style)
    cheatsheet,  // Shortcut cheatsheet modal (<kbd>-list style)
    new_file,    // "New" button
    open_file,   // "Open" button
    save_file,   // "Save" button
    find,        // Find / Search input
};

/// One default-binding emission produced by the source heuristic. A
/// `DefaultShortcut` is appended into `CodeGenOptions::shortcuts` as a
/// `DetectedShortcut` (with `pattern = "default:<name>"`) so it flows
/// through the V2 codegen path with no fork — same registerShortcut +
/// synthetic-keydown thunk as an extracted shortcut.
struct DefaultShortcutCandidate {
    DefaultShortcutPattern pattern;
    /// Component name / identifier that won the match — for traceability
    /// in `shortcuts.json` and for the developer's mental model.
    std::string target;
    /// "high" (≥3 signals) or "medium" (exactly 2). The detector only
    /// emits bindings for high+medium — anything weaker is dropped.
    std::string confidence;
    /// Source signals that fired, in order. Surfaced in `shortcuts.json`
    /// so a reviewer can audit the heuristic.
    std::vector<std::string> signals;
};

/// One unresolved candidate set — emitted when multiple components match
/// the same pattern with comparable confidence. No binding fires; the
/// caller can decide whether to triage manually or accept the ambiguity.
struct DefaultShortcutCollision {
    DefaultShortcutPattern pattern;
    std::vector<std::string> candidates;  // component names
    std::string reason;
};

/// Result of running `detect_default_shortcuts`. The accepted bindings
/// are mapped to DetectedShortcut form by `apply_default_shortcuts` and
/// fed through the V2 codegen; collisions go into shortcuts.json as
/// diagnostic-only entries.
struct DefaultShortcutScan {
    std::vector<DefaultShortcutCandidate> accepted;
    std::vector<DefaultShortcutCollision> collisions;
};

/// Static-scan a TSX/JS source string for components matching the
/// platform-convention defaults table. Signals: component identifier
/// (`SettingsModal`, `HelpDialog`, `ShortcutsCheatsheet`, etc.), canonical
/// component name, `role=` attribute, `aria-label=` text, modal heading text,
/// presence of `<kbd>` tags (cheatsheet disambiguator). Conservative —
/// requires ≥2 signals to fire; multiple-candidate matches go into
/// `collisions` not `accepted`.
///
/// Already-extracted shortcuts (`existing_extracted`) suppress the same-
/// chord default so a developer's hand-written binding always wins.
///
/// Pure lexical pass; no JSX parsing. Returns an empty scan when no
/// patterns match. Never throws.
DefaultShortcutScan detect_default_shortcuts(
    const std::string& source,
    const std::vector<DetectedShortcut>& existing_extracted);

/// Convert accepted candidates from `detect_default_shortcuts` into the
/// `DetectedShortcut` form that V2 codegen expects. The platform chord
/// is selected based on `target_platform`: macOS gets the Cmd / Cmd+?
/// chords, Win/Linux get Ctrl / F1. `pattern` is namespaced with a
/// `"default:"` prefix so manifest readers can tell auto-bound from
/// extracted entries at a glance.
enum class TargetPlatform { macos, win_linux };

std::vector<DetectedShortcut> apply_default_shortcuts(
    const std::vector<DefaultShortcutCandidate>& accepted,
    TargetPlatform platform);

/// Render the auto-bound defaults + collisions into JSON for emission
/// alongside the extracted-shortcuts manifest. Mirror of
/// `serialize_detected_shortcuts` shape but namespaced under
/// `"defaults"` and `"collisions"` top-level keys.
std::string serialize_default_shortcut_scan(const DefaultShortcutScan& scan);


/// Heuristic: does this HTML look like a JS-bundler entry (React, Vue,
/// Svelte, @pulp/react, generic webpack/vite/bun output) rather than a
/// hand-authored Claude Design page? The static-HTML parser only sees
/// the literal DOM, so for bundler entries it captures the empty mount
/// placeholder. Used by `pulp import-design --from claude` to print a
/// "use pulp-design-tool --script <bundle>.js" hint instead of letting
/// the user wonder why their 1000-element editor became a 9-element
/// ui.js. Never throws; returns false on any empty/unparseable input.
bool looks_like_bundler_entry(const std::string& html);

/// Detect audio widget type from a node name.
AudioWidgetType detect_audio_widget(const std::string& name);

} // namespace pulp::view
