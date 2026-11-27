#pragma once

/// @file
/// The "Updates" Settings group on Vellum views, the port of
/// `pulp::format::AppUpdatesSettingsView`:
///
///   [toggle] Automatically check for updates
///   [Check for Updates…]
///   Version 1.0.7 · Last checked: 2026-10-04 14:03
///   Updates download from Spectr's GitHub releases. Installing an update …
///   [View Releases]
///
/// It reads and drives the process-wide AppUpdateService
/// (pulp/format/app_updates.hpp). A ViewProcessor adds it to its settings
/// with
///
///   if (pulp::format::app_update_status().available)
///       sections.push_back({"Updates",
///                           std::make_unique<pulp::vellum::AppUpdatesSettingsView>()});
///
/// In a plug-in no service is installed, so the guard keeps it out; the view
/// itself also hides its controls when the service disappears. Main thread.

#include <pulp/format/app_updates.hpp>
#include <pulp/vellum/view_editor.hpp>

#include <vellum/view/view.hpp>

namespace vellum::view {
class Label;
class TextButton;
class Toggle;
} // namespace vellum::view

namespace pulp::vellum {

class AppUpdatesSettingsView : public ::vellum::view::View, public EditorIdleTarget {
  public:
    AppUpdatesSettingsView();

    /// Re-read the service status into the controls. Cheap; call it from the
    /// editor's idle.
    void refresh();

    /// Calls refresh() on each editor idle tick, wherever the view sits in a
    /// ViewEditorFactory editor's tree (a settings tab included), so a check
    /// that finishes in the background shows without a click.
    void editor_idle() override {
        refresh();
    }

    [[nodiscard]] ::vellum::view::Toggle* automatic_checks_toggle() const {
        return automatic_toggle_;
    }
    [[nodiscard]] ::vellum::view::TextButton* check_button() const {
        return check_button_;
    }
    /// Opens the declared releases page; hidden when the app declares none.
    [[nodiscard]] ::vellum::view::TextButton* releases_button() const {
        return releases_button_;
    }
    [[nodiscard]] ::vellum::view::Label* status_label() const {
        return status_label_;
    }
    [[nodiscard]] ::vellum::view::Label* note_label() const {
        return note_label_;
    }

  private:
    ::vellum::view::Toggle* automatic_toggle_ = nullptr;
    ::vellum::view::TextButton* check_button_ = nullptr;
    ::vellum::view::TextButton* releases_button_ = nullptr;
    ::vellum::view::Label* status_label_ = nullptr;
    ::vellum::view::Label* note_label_ = nullptr;
};

} // namespace pulp::vellum
