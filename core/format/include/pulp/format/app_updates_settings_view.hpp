#pragma once

// A drop-in "Updates" Settings group for native Pulp editors:
//
//   [toggle] Automatically check for updates
//   [Check for Updates…]
//   Version 1.0.7 · Last checked: 2026-10-04 14:03
//   Updates download from Spectr's GitHub releases. Installing an update …
//   [View Releases]
//
// It reads and drives the process-wide AppUpdateService
// (pulp/format/app_updates.hpp). The standalone Settings panel adds it as an
// "Updates" tab automatically; a native editor can add it to its own
// settings with
//
//   if (pulp::format::app_update_status().available)
//       panel.add_child(std::make_unique<pulp::format::AppUpdatesSettingsView>());
//
// In a plug-in no service is installed, so the guard keeps it out; the view
// itself also hides its controls when the service disappears.

#include <pulp/format/app_updates.hpp>
#include <pulp/view/view.hpp>

namespace pulp::view {
class Label;
class TextButton;
class Toggle;
} // namespace pulp::view

namespace pulp::format {

class AppUpdatesSettingsView : public view::View {
  public:
    AppUpdatesSettingsView();

    /// Re-read the service status into the controls. Cheap; call it from the
    /// editor's poll/idle (the standalone Settings panel does).
    void refresh();

    [[nodiscard]] view::Toggle* automatic_checks_toggle() const {
        return automatic_toggle_;
    }
    [[nodiscard]] view::TextButton* check_button() const {
        return check_button_;
    }
    /// Opens the declared releases page; hidden when the app declares none.
    [[nodiscard]] view::TextButton* releases_button() const {
        return releases_button_;
    }
    [[nodiscard]] view::Label* status_label() const {
        return status_label_;
    }
    [[nodiscard]] view::Label* note_label() const {
        return note_label_;
    }

  private:
    view::Toggle* automatic_toggle_ = nullptr;
    view::TextButton* check_button_ = nullptr;
    view::TextButton* releases_button_ = nullptr;
    view::Label* status_label_ = nullptr;
    view::Label* note_label_ = nullptr;
};

} // namespace pulp::format
