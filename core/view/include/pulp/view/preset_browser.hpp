#pragma once

/// @file preset_browser.hpp
/// Preset browser UI component with categories, search, and navigation.

#include <pulp/view/view.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/state/preset_manager.hpp>
#include <pulp/canvas/canvas.hpp>
#include <functional>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

namespace pulp::view {

/// Preset browser widget with list display, search filtering,
/// category grouping, and prev/next navigation.
///
/// Wraps a PresetManager and displays its presets in a scrollable list
/// with optional search filtering and folder-based categories.
///
/// @code
/// PresetBrowser browser(preset_manager);
/// browser.set_bounds({0, 0, 300, 400});
/// browser.on_preset_selected = [&](const state::PresetInfo& p) {
///     preset_manager.load(p);
/// };
/// root.add_child(std::move(browser_ptr));
/// @endcode
class PresetBrowser : public View {
public:
    explicit PresetBrowser(state::PresetManager& manager)
        : manager_(manager)
    {
        set_focusable(true);
        refresh();
    }

    /// Called when a preset is selected (single click).
    std::function<void(const state::PresetInfo&)> on_preset_selected;

    /// Called when a preset is activated (double click or Enter).
    std::function<void(const state::PresetInfo&)> on_preset_activated;

    /// Refresh the preset list from the PresetManager.
    void refresh() {
        all_presets_ = manager_.all_presets();
        apply_filter();
    }

    /// Set search filter text. Empty string shows all presets.
    void set_filter(const std::string& text) {
        filter_text_ = text;
        apply_filter();
    }

    const std::string& filter_text() const { return filter_text_; }

    /// Show only factory presets, only user presets, or all.
    enum class ShowMode { all, factory_only, user_only };
    void set_show_mode(ShowMode mode) {
        show_mode_ = mode;
        apply_filter();
    }

    /// Select next preset in the filtered list.
    void select_next() {
        if (filtered_.empty()) return;
        selected_index_ = (selected_index_ + 1) % static_cast<int>(filtered_.size());
        notify_selection();
    }

    /// Select previous preset in the filtered list.
    void select_previous() {
        if (filtered_.empty()) return;
        if (selected_index_ < 0) {
            selected_index_ = static_cast<int>(filtered_.size()) - 1;
            notify_selection();
            return;
        }
        selected_index_ = (selected_index_ - 1 + static_cast<int>(filtered_.size()))
                          % static_cast<int>(filtered_.size());
        notify_selection();
    }

    /// Get the currently selected preset index (-1 if none).
    int selected_index() const { return selected_index_; }

    /// Get the currently selected preset (nullptr if none).
    const state::PresetInfo* selected_preset() const {
        if (selected_index_ >= 0 && selected_index_ < static_cast<int>(filtered_.size()))
            return &filtered_[static_cast<size_t>(selected_index_)];
        return nullptr;
    }

    /// Number of presets currently shown (after filtering).
    int visible_count() const { return static_cast<int>(filtered_.size()); }

    float row_height() const { return row_height_; }
    void set_row_height(float h) { row_height_ = h; }

    void paint(canvas::Canvas& canvas) override {
        auto b = local_bounds();

        // Background
        canvas.set_fill_color(resolve_color("preset_browser_bg",
            canvas::Color::hex(0x16213e)));
        canvas.fill_rounded_rect(b.x, b.y, b.width, b.height, 6);

        // Header: current preset name + nav arrows
        float header_h = 32.0f;
        canvas.set_fill_color(resolve_color("surface",
            canvas::Color::hex(0x1a1a2e)));
        canvas.fill_rect(b.x, b.y, b.width, header_h);

        canvas.set_font("system", 13);
        canvas.set_fill_color(resolve_color("text",
            canvas::Color::hex(0xe0e0e0)));

        std::string current = manager_.current_preset_name();
        if (current.empty()) current = "(No preset)";
        if (manager_.has_unsaved_changes()) current += " *";
        canvas.fill_text(current, b.x + 30, b.y + header_h / 2 + 4);

        // Nav arrows
        canvas.set_fill_color(resolve_color("text_muted",
            canvas::Color::hex(0x808090)));
        canvas.fill_text("\xe2\x97\x80", b.x + 8, b.y + header_h / 2 + 4);  // ◀
        canvas.fill_text("\xe2\x96\xb6", b.x + b.width - 18, b.y + header_h / 2 + 4);  // ▶

        // Search filter display
        if (!filter_text_.empty()) {
            canvas.set_font("system", 11);
            canvas.set_fill_color(resolve_color("accent",
                canvas::Color::hex(0xe94560)));
            canvas.fill_text("Filter: " + filter_text_,
                             b.x + 8, b.y + header_h + 14);
        }

        // Preset list
        float list_y = b.y + header_h + (filter_text_.empty() ? 4.0f : 22.0f);
        float list_h = b.y + b.height - list_y;

        canvas.save();
        canvas.clip_rect(b.x, list_y, b.width, list_h);

        int visible_start = static_cast<int>(scroll_offset_ / row_height_);
        int visible_count = static_cast<int>(list_h / row_height_) + 2;

        for (int i = visible_start;
             i < std::min(visible_start + visible_count, static_cast<int>(filtered_.size()));
             ++i)
        {
            float y = list_y + static_cast<float>(i) * row_height_ - scroll_offset_;
            const auto& preset = filtered_[static_cast<size_t>(i)];

            // Selection highlight
            if (i == selected_index_) {
                canvas.set_fill_color(resolve_color("list_selected_bg",
                    canvas::Color::hex(0x0f3460)));
                canvas.fill_rect(b.x + 2, y, b.width - 4, row_height_);
            }

            // Factory/user indicator
            canvas.set_font("system", 10);
            if (preset.is_factory) {
                canvas.set_fill_color(resolve_color("text_muted",
                    canvas::Color::hex(0x606070)));
                canvas.fill_text("F", b.x + 6, y + row_height_ * 0.65f);
            }

            // Preset name
            canvas.set_font("system", 13);
            canvas.set_fill_color(resolve_color("text",
                canvas::Color::hex(0xe0e0e0)));
            canvas.fill_text(preset.name, b.x + 22, y + row_height_ * 0.65f);

            // Folder path (if any)
            if (!preset.folder.empty()) {
                canvas.set_font("system", 10);
                canvas.set_fill_color(resolve_color("text_muted",
                    canvas::Color::hex(0x808090)));
                canvas.fill_text(preset.folder,
                                 b.x + b.width - 80, y + row_height_ * 0.65f);
            }
        }

        canvas.restore();
    }

    void on_mouse_event(const MouseEvent& event) override {
        if (!event.is_down) return;
        auto b = local_bounds();

        float header_h = 32.0f;

        // Click in header area — nav arrows
        if (event.position.y < header_h) {
            if (event.position.x < 25) {
                select_previous();
                if (on_preset_activated && selected_preset())
                    on_preset_activated(*selected_preset());
            } else if (event.position.x > b.width - 25) {
                select_next();
                if (on_preset_activated && selected_preset())
                    on_preset_activated(*selected_preset());
            }
            return;
        }

        // Click in list area
        float list_y = header_h + (filter_text_.empty() ? 4.0f : 22.0f);
        if (event.position.y < list_y)
            return;
        float click_y = event.position.y - list_y + scroll_offset_;
        int index = static_cast<int>(click_y / row_height_);

        if (index >= 0 && index < static_cast<int>(filtered_.size())) {
            selected_index_ = index;
            notify_selection();

            if (event.click_count == 2 && on_preset_activated) {
                on_preset_activated(filtered_[static_cast<size_t>(index)]);
            }
        }
    }

    bool on_key_event(const KeyEvent& event) override {
        if (!event.is_down) return false;

        if (event.key == KeyCode::up) { select_previous(); return true; }
        if (event.key == KeyCode::down) { select_next(); return true; }
        if (event.key == KeyCode::enter && selected_preset()) {
            if (on_preset_activated) on_preset_activated(*selected_preset());
            return true;
        }
        return false;
    }

private:
    state::PresetManager& manager_;
    std::vector<state::PresetInfo> all_presets_;
    std::vector<state::PresetInfo> filtered_;
    std::string filter_text_;
    ShowMode show_mode_ = ShowMode::all;
    int selected_index_ = -1;
    float row_height_ = 24.0f;
    float scroll_offset_ = 0.0f;

    void apply_filter() {
        filtered_.clear();
        for (const auto& p : all_presets_) {
            if (show_mode_ == ShowMode::factory_only && !p.is_factory) continue;
            if (show_mode_ == ShowMode::user_only && p.is_factory) continue;

            if (!filter_text_.empty()) {
                // Case-insensitive search in name and folder
                std::string lower_name = p.name;
                std::string lower_folder = p.folder;
                std::string lower_filter = filter_text_;
                std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                    [](unsigned char c) { return std::tolower(c); });
                std::transform(lower_folder.begin(), lower_folder.end(), lower_folder.begin(),
                    [](unsigned char c) { return std::tolower(c); });
                std::transform(lower_filter.begin(), lower_filter.end(), lower_filter.begin(),
                    [](unsigned char c) { return std::tolower(c); });

                if (lower_name.find(lower_filter) == std::string::npos &&
                    lower_folder.find(lower_filter) == std::string::npos) continue;
            }

            filtered_.push_back(p);
        }

        if (selected_index_ >= static_cast<int>(filtered_.size())) {
            selected_index_ = filtered_.empty() ? -1 : 0;
        }
    }

    void notify_selection() {
        if (on_preset_selected && selected_preset()) {
            on_preset_selected(*selected_preset());
        }
    }
};

} // namespace pulp::view
