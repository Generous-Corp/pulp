#pragma once

#include <pulp/view/design_frame_view.hpp>

namespace pulp::view {

// Property Panel — Ink & Signal catalog component. Renders the faithful
// Figma-exported SVG 1:1 via DesignFrameView (SkSVGDOM). Generated from
// Figma node 208:2 by tools/import-design/make_catalog_component.py;
// reskin/extend via the faithful-vector lane, never by hand-painting.
class PropertyPanelView : public DesignFrameView {
public:
    PropertyPanelView();
};

}  // namespace pulp::view
