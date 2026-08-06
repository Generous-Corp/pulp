// Per-control sprite production for the browser-capture lane.
//
// A browser capture is ONE flat picture of the whole panel, so a lowered
// control owns no art of its own and the design's indicator stays frozen
// wherever the screenshot caught it. This pass gives each knob or fader whose
// author declared an indicator (`data-pulp-indicator`) the capture slices it
// needs: knobs get a cleaned disc for their rotating pointer; faders get a
// cleaned body plus the authored thumb as a separate value-driven overlay.
//
// It emits the attribute names the Figma lane already emits, so neither the
// materializer nor the renderer needs a browser-specific branch. Only the
// producer differs: Figma finds a hairline child layer; a capture has no
// layers, so the author declares which pixels are the pointer.

#pragma once

#include <pulp/view/design_ir.hpp>

#include <filesystem>
#include <string>

namespace pulp::import_design {

/// Crop, clean, and stamp per-control sprites in-place on `ir`.
///
/// Acts only on knob/fader nodes carrying the hand-off rectangles that
/// `lower_browser_capture_to_ir` stamps for an author-declared indicator, and
/// consumes those rectangles. A control without a declared indicator, and
/// every other node, is left byte-for-byte as it was.
///
/// Sprites are written into `sprite_directory` (the capture directory, so they
/// share the reference PNG's lifetime and are localized alongside it) and named
/// by content hash, like the rest of the asset pipeline.
///
/// Returns the number of controls skinned. `error` is set, and 0 returned, only
/// when a declared indicator could not be honoured — an unreadable capture or
/// an unwritable sprite — so a silent half-import is not possible.
int apply_browser_capture_control_sprites(
    pulp::view::DesignIR& ir,
    const std::filesystem::path& capture_png,
    const std::filesystem::path& sprite_directory,
    std::string* error = nullptr);

}  // namespace pulp::import_design
