#pragma once

#include <pulp/view/design_frame_view.hpp>

namespace pulp::view {

// ── MusicalTypingKeyboard ────────────────────────────────────────────────────
// Ink & Signal "Musical Typing Keyboard" catalog component (Category::audio).
//
// This is NOT a hand-painted widget. It renders the faithful, Figma-exported
// SVG 1:1 through DesignFrameView (SkSVGDOM) — verified at 1.08/255 vs the
// design by pulp-svg-probe. The SVG was lowered from Figma node 187:2 via the
// figma-plugin faithful-vector lane (tools/import-design/figma_rest_export.py),
// which is the source of truth for Figma → Pulp imports. Reskin/extend it
// through that lane (re-export → re-embed) rather than re-drawing by hand;
// interactivity is added via DesignFrameView's element callbacks.
class MusicalTypingKeyboard : public DesignFrameView {
public:
    MusicalTypingKeyboard();
};

}  // namespace pulp::view
