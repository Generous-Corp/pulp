#pragma once

#include <pulp/view/theme.hpp>
#include <string>
#include <vector>
#include <optional>

namespace pulp::view {

// ── Theme Preset System ─────────────────────────────────────────────────────
// Ports 38 tweakcn-sourced presets (via Solar) with light/dark variants.
// Each preset defines ~15 semantic UI tokens; the derivation layer auto-generates
// ~35 additional audio-specific tokens (Knob, Slider, Meter, Waveform, etc.).

/// Semantic color tokens from tweakcn/shadcn-ui (the base 11 + 5 chart colors).
struct SemanticColors {
    Color background;
    Color foreground;
    Color card;
    Color primary;
    Color secondary;
    Color muted;
    Color accent;
    Color destructive;
    Color border;
    Color input;
    Color ring;
    Color chart1;
    Color chart2;
    Color chart3;
    Color chart4;
    Color chart5;
};

/// A theme preset with light and dark variants.
struct ThemePreset {
    std::string id;           // Machine-readable key (e.g., "catppuccin")
    std::string name;         // Display name (e.g., "Catppuccin")
    SemanticColors light;
    SemanticColors dark;

    /// Optional explicit audio token overrides (per-variant).
    /// If a token is present here, it overrides the derived value.
    Theme light_overrides;
    Theme dark_overrides;
};

/// Get all built-in theme presets (37 tweakcn-sourced + "Ink & Signal",
/// Pulp's flagship design language = 38 total).
const std::vector<ThemePreset>& all_presets();

/// Look up a preset by id. Returns nullptr if not found.
const ThemePreset* find_preset(const std::string& id);

/// Get the list of preset ids.
std::vector<std::string> preset_ids();

// ── Derivation Layer ────────────────────────────────────────────────────────
// Generates a full Pulp Theme from semantic colors using a documented mapping.

/// Derive a complete Theme from semantic colors.
/// This generates all standard Pulp tokens including audio-specific ones:
///   accent.primary    → knob.arc, slider.fill, progress.fill, spinner, tab.active
///   background        → knob.arc.bg, slider.track, control.track, waveform.grid bg
///   foreground        → knob.thumb, slider.thumb, control.thumb
///   muted/mutedFg     → waveform.grid, control.border, text.secondary, tab.inactive
///   destructive       → meter.red, accent.error
///   accent + warm hue → meter.yellow, accent.warning
///   accent + green hue→ meter.green, accent.success
///   card              → card.empty, card.ready
///   card + muted tint → card.loading
///   destructive low α → card.error
///   border            → modal.border, control.border
///   secondary         → bg.secondary, overlay.bg
///   primary           → gradient.start; secondary → gradient.end
///   input             → bg.surface
Theme derive_theme(const SemanticColors& colors);

/// Build a complete Theme from a preset for the given appearance.
/// Applies derivation, then overlays any explicit overrides from the preset.
Theme theme_from_preset(const ThemePreset& preset, bool dark);

} // namespace pulp::view
