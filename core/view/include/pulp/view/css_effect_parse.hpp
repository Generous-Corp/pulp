#pragma once

// CSS effect values that a renderer needs as numbers and enums rather than as
// strings: the blur radius out of a filter list, and a blend-mode keyword.
//
// These live apart from the import path because both render lanes need them
// and neither owns them. Keeping them together is also what stops the two
// lanes drifting — the JS bridge and the native tree read the same rules.

#include <pulp/canvas/canvas.hpp>

#include <optional>
#include <string>

namespace pulp::view {

/// The blur radius out of a CSS filter list, in px.
///
/// Only `blur()` is read. A list may carry brightness, saturate and the rest;
/// those need a real filter chain, and lowering them to a blur radius would be
/// a lie. Returning nothing leaves the node unfiltered rather than wrongly
/// blurred — an unfiltered element is visibly missing, a wrongly blurred one
/// looks intentional.
std::optional<float> css_blur_radius(const std::string& filter);

/// A CSS mix-blend-mode keyword. Unknown keywords return nothing rather than
/// falling back to `normal`, so a mode we cannot honor is visible as absent.
std::optional<canvas::Canvas::BlendMode> css_blend_mode(const std::string& keyword);

}  // namespace pulp::view
