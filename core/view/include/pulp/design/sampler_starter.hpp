#pragma once

// Sampler starter — a real, runnable sampler UI assembled entirely from the
// Ink & Signal design-system components. It proves the ingested system is
// genuinely usable:
// someone who wants to build a sampler picks components out of pulp::design and
// gets a themed, reskinnable UI with no bespoke painting.
//
// Everything paints through theme tokens, so apply a different theme (or flip
// light/dark) and the whole panel restyles — the reskin contract end to end.

#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>

#include <memory>

namespace pulp::design {

// Fixed design size of the starter panel. Render it headlessly with
// render_to_png / render_to_file at this size, or mount it in a window.
inline constexpr float kSamplerWidth = 720.0f;
inline constexpr float kSamplerHeight = 440.0f;

// Build the sampler starter panel with `theme` applied to the root (descendants
// resolve colors up the parent chain). Pass ink_signal_theme(dark) for the
// flagship look, or any other Theme to preview a reskin.
std::unique_ptr<pulp::view::View> build_sampler_starter(const pulp::view::Theme& theme);

}  // namespace pulp::design
