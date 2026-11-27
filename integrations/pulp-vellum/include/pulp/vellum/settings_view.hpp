#pragma once

/// @file
/// A plug-in's settings tabs on Vellum views, and the settings editor built from
/// them. ViewProcessor<> (view_processor.hpp) serves this as its
/// EditorRole::settings editor; it builds in any SDK.

#include <memory>
#include <string>
#include <vector>

namespace vellum::view {
class View;
}

namespace pulp::vellum {

/// One settings tab a plug-in contributes.
struct ViewSettingsSection {
    std::string title;
    std::unique_ptr<::vellum::view::View> view;
};

/// The settings editor for `sections`: null when none has a view, the one
/// section's view alone, or a tab panel of them, in order.
std::unique_ptr<::vellum::view::View> make_settings_view(std::vector<ViewSettingsSection> sections);

} // namespace pulp::vellum
