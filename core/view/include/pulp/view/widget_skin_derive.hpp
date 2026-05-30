#pragma once

#include <pulp/canvas/canvas.hpp>
#include <cstdint>
#include <vector>

namespace pulp::view {

/// Derives native-widget skin styling from a captured design asset (a flat
/// PNG exported by a design tool such as the Figma plugin).
///
/// Rationale (Pulp issue #3191): the figma-plugin export ships a single flat
/// PNG per recognised widget — a fader baked at its captured value, a meter
/// baked at its captured level. Skinning the native widget with that image
/// verbatim would FREEZE the control (the thumb / fill couldn't move). Instead
/// we *sample* the captured pixels to recover the underlying style (track /
/// fill / thumb colours for a fader; the gradient stops for a meter) and hand
/// those to the widget, which redraws them procedurally — value-driven, so the
/// thumb still moves with set_value() and the fill still tracks set_level().
///
/// This is a GENERALIZABLE importer rule: it reads the design data (the actual
/// exported pixels) and derives style. It hardcodes no per-instance colours,
/// pixel offsets, or widget names.

/// Minimal RGBA8 image view consumed by the derivation. Row-major,
/// 4 bytes/pixel. Borrowed — the caller owns the storage.
struct SkinImage {
    const uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    bool valid() const { return pixels && width > 0 && height > 0; }
};

/// Derived fader skin. `has_*` flags mark which channels were recovered; an
/// importer should only emit a setter for the channels that were found.
struct FaderSkin {
    canvas::Color track_color{};
    canvas::Color fill_color{};
    canvas::Color thumb_color{};
    canvas::Color thumb_border_color{};
    bool has_track = false;
    bool has_fill = false;
    bool has_thumb = false;
    bool has_thumb_border = false;
    // Horizontal extents recovered from the captured art, in ASSET PIXELS
    // (the importer divides by the asset scale to get logical px). The track
    // is the thin low-saturation central column at a non-thumb row; the thumb
    // is the widest opaque row (the silver slab). pulp #3191 width fix.
    float track_width_px = 0.0f;
    float thumb_width_px = 0.0f;
    bool has_track_width = false;
    bool has_thumb_width = false;
    // Normalised thumb position recovered from the capture (0 = bottom, 1 =
    // top), i.e. where the design actually drew the thumb. An audio fader's
    // value→position map is non-linear (a taper), so a linear (value-min)/(max-
    // min) seed lands the thumb in the wrong place; seeding from the captured
    // position reproduces the design. pulp #3191.
    float thumb_position = 0.0f;
    bool has_thumb_position = false;
    bool any() const { return has_track || has_fill || has_thumb || has_thumb_border; }
};

/// Derived meter skin: gradient stops ordered low→high (bottom→top), plus an
/// optional background colour. `gradient` is empty when no gradient could be
/// recovered (caller should then leave the meter on its default look).
struct MeterSkin {
    std::vector<canvas::Color> gradient;  // low → high
    canvas::Color background{};
    bool has_background = false;
    // Horizontal extent of the coloured bar, in ASSET PIXELS (the importer
    // divides by the asset scale to get logical px). Recovered from the bar's
    // own vertical region so faint label glyphs below it don't widen it.
    // pulp #3191 width fix.
    float bar_width_px = 0.0f;
    bool has_bar_width = false;
    // Normalised fill level recovered from the capture (0 = empty, 1 = full):
    // the height of the contiguous saturated fill from the bottom of the bar.
    // Like the fader, a meter's dB→position map is non-linear, so seed the
    // initial level from where the capture actually filled to. pulp #3191.
    float fill_level = 0.0f;
    bool has_fill_level = false;
    bool valid() const { return gradient.size() >= 2; }
};

/// Sample a captured fader PNG → FaderSkin. Locates the widget art (tallest
/// opaque vertical run in the centre column), then recovers the dark track,
/// the saturated fill, the bright thumb body, and its bevel edge.
FaderSkin derive_fader_skin(const SkinImage& img);

/// Sample a captured meter PNG → MeterSkin. Locates the widget art, then walks
/// the contiguous saturated fill from the bottom up and samples `stop_count`
/// gradient stops across it (low→high).
MeterSkin derive_meter_skin(const SkinImage& img, int stop_count = 5);

}  // namespace pulp::view
