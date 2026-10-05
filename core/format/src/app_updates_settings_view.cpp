#include <pulp/format/app_updates_settings_view.hpp>

#include <pulp/view/buttons.hpp>
#include <pulp/view/widgets.hpp>

#include <memory>
#include <string>

namespace pulp::format {

AppUpdatesSettingsView::AppUpdatesSettingsView() {
    flex().direction = view::FlexDirection::column;
    flex().flex_grow = 1.0f;
    flex().padding = 12.0f;
    flex().gap = 8.0f;
    set_id("pulp-app-updates");

    auto toggle_row = std::make_unique<view::View>();
    toggle_row->flex().direction = view::FlexDirection::row;
    toggle_row->flex().align_items = view::FlexAlign::center;
    toggle_row->flex().gap = 8.0f;
    toggle_row->flex().preferred_height = 28.0f;
    toggle_row->flex().flex_shrink = 0.0f;
    auto toggle = std::make_unique<view::Toggle>();
    automatic_toggle_ = toggle.get();
    toggle->set_id("pulp-app-updates-automatic");
    toggle->flex().preferred_width = 48.0f;
    toggle->flex().preferred_height = 26.0f;
    toggle->flex().flex_shrink = 0.0f;
    toggle->on_toggle = [this](bool on) {
        set_app_update_automatic_checks(on);
        refresh();
    };
    toggle_row->add_child(std::move(toggle));
    auto toggle_label = std::make_unique<view::Label>();
    toggle_label->set_text("Automatically check for updates");
    toggle_label->flex().flex_grow = 1.0f;
    toggle_row->add_child(std::move(toggle_label));
    add_child(std::move(toggle_row));

    auto button = std::make_unique<view::TextButton>("Check for Updates\xE2\x80\xA6");
    check_button_ = button.get();
    button->set_id("pulp-app-updates-check");
    button->flex().preferred_width = 180.0f;
    button->flex().preferred_height = 28.0f;
    button->flex().flex_shrink = 0.0f;
    button->on_click = [this] {
        check_for_app_updates();
        refresh();
    };
    add_child(std::move(button));

    auto status = std::make_unique<view::Label>();
    status_label_ = status.get();
    status->set_id("pulp-app-updates-status");
    status->flex().preferred_height = 16.0f;
    add_child(std::move(status));

    auto note = std::make_unique<view::Label>();
    note_label_ = note.get();
    note->set_id("pulp-app-updates-note");
    note->set_multi_line(true);
    note->flex().preferred_height = 48.0f;
    add_child(std::move(note));

    auto releases = std::make_unique<view::TextButton>("View Releases");
    releases_button_ = releases.get();
    releases->set_id("pulp-app-updates-releases");
    releases->flex().preferred_width = 140.0f;
    releases->flex().preferred_height = 24.0f;
    releases->flex().flex_shrink = 0.0f;
    releases->on_click = [] { open_app_releases_page(); };
    add_child(std::move(releases));

    refresh();
}

void AppUpdatesSettingsView::refresh() {
    const auto status = app_update_status();
    if (automatic_toggle_ && automatic_toggle_->is_on() != status.automatic_checks)
        automatic_toggle_->set_on(status.automatic_checks, view::Notify::none, false);
    if (check_button_)
        check_button_->set_enabled(status.available && status.can_check_now);
    if (status_label_) {
        std::string text = app_update_version_text(status);
        if (status.available) {
            if (!text.empty())
                text += " \xC2\xB7 ";
            text += app_update_last_check_text(status);
        }
        status_label_->set_text(std::move(text));
    }
    if (note_label_)
        note_label_->set_text(app_update_note(status));
    if (releases_button_)
        releases_button_->set_visible(!status.releases_url.empty());
    set_visible(status.available);
}

} // namespace pulp::format
