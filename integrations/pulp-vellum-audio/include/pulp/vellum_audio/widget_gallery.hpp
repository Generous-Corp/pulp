#pragma once

#include <vellum/view/theme.hpp>
#include <vellum/view/view.hpp>

#include <memory>

namespace pulp::vellum_audio {

inline constexpr float GALLERY_WIDTH = 940.0f;

// Build a themed board sized to its full content, suitable for headless capture.
std::unique_ptr<::vellum::view::View> build_widget_gallery(const ::vellum::view::Theme& theme);

// Wrap the full board in a viewport that scrolls in both axes.
std::unique_ptr<::vellum::view::View>
build_scrolling_widget_gallery(const ::vellum::view::Theme& theme, float viewport_w,
                               float viewport_h);

} // namespace pulp::vellum_audio
