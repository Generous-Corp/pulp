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
#include <vector>

namespace pulp::view {

/// The blur radius out of a CSS filter list, in px.
///
/// Only `blur()` is read. A list may carry brightness, saturate and the rest;
/// those need a real filter chain, and lowering them to a blur radius would be
/// a lie. Returning nothing leaves the node unfiltered rather than wrongly
/// blurred — an unfiltered element is visibly missing, a wrongly blurred one
/// looks intentional.
std::optional<float> css_blur_radius(const std::string& filter);

/// A whole CSS filter list as the canvas layer verbs consume it, in source
/// order — `saturate(0.15)`, `grayscale(1) brightness(1.6)`, `blur(6px)`.
///
/// Percentages and bare numbers both resolve to the spec's 0..1-relative
/// amount, so `saturate(15%)` and `saturate(0.15)` produce the same entry.
/// `hue-rotate` is read as DEGREES: a computed style always serializes the
/// angle that way, and a `rad` / `turn` / `grad` author value would be taken
/// as degrees. Feed this computed values, not authored ones.
///
/// A function this parser does not know is DROPPED and the rest of the list is
/// kept, because the alternative — refusing the list — turns one unrecognized
/// function into no filtering at all. `drop-shadow()` is among the dropped:
/// its colour argument needs the full colour parser, and a drop-shadow with
/// the wrong colour is worse than an absent one. An empty result means nothing
/// in the value was recognized; the caller then leaves the node unfiltered.
std::vector<canvas::Canvas::FilterChainEntry> css_filter_chain(
    const std::string& filter);

/// A CSS mix-blend-mode keyword. Unknown keywords return nothing rather than
/// falling back to `normal`, so a mode we cannot honor is visible as absent.
std::optional<canvas::Canvas::BlendMode> css_blend_mode(const std::string& keyword);

}  // namespace pulp::view
