#pragma once

#include <pulp/view/design_ir.hpp>

#include <optional>
#include <stdexcept>

namespace pulp::view {

/// Canonical lowering from an authoritative browser-capture node to the
/// ordinary image-node contract shared by JS, baked C++, and native
/// materialization. Semantic overlays are deliberately not promoted here.
inline std::optional<IRNode> lower_faithful_capture_to_image(
    const IRNode& node) {
    if (node.render_mode != NodeRenderMode::faithful_capture)
        return std::nullopt;
    if (!node.capture_asset_id || node.capture_asset_id->empty())
        throw std::invalid_argument(
            "faithful_capture node requires capture_asset_id");
    IRNode image = node;
    image.type = "image";
    // Visual capture is authoritative. Semantic evidence is stored out of band;
    // leaving audio_widget set would let recognition reclassify this image as
    // a native silver knob/fader and overwrite the authored pixels.
    image.audio_widget = AudioWidgetType::none;
    image.render_mode = NodeRenderMode::normal;
    image.children.clear();
    image.alternate_frames.clear();
    image.attributes["asset_ref"] = *node.capture_asset_id;
    return image;
}

inline void lower_faithful_captures_in_place(IRNode& node) {
    if (auto image = lower_faithful_capture_to_image(node))
        node = std::move(*image);
    for (auto& child : node.children)
        lower_faithful_captures_in_place(child);
    for (auto& frame : node.alternate_frames)
        lower_faithful_captures_in_place(frame);
}

}  // namespace pulp::view
