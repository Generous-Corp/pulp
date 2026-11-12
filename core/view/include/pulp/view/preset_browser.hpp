#pragma once

/// @file preset_browser.hpp
/// Preset browser UI component with categories, search, and navigation.

#include <pulp/view/context_menu.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/state/preset_library.hpp>
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
///
/// With a `state::PresetLibrary` attached (`set_library`) the browser lists the
/// library's query results instead: each row shows a favourite star, the
/// display name (a rename never touches the file), tags and the MIDI program
/// number when the library has a program map. The search text uses the
/// library's grammar (every word must match). Without a library the browser
/// behaves exactly as before.
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

    /// Refresh the preset list from the PresetManager (or the library).
    void refresh() {
        if (library_) {
            apply_library_query();
            return;
        }
        all_presets_ = manager_.all_presets();
        apply_filter();
    }

    // ── Context menu (library mode) ─────────────────────────────────────

    /// What a context-menu item does. The ids are the `ContextMenu::Item::id`s
    /// `context_menu_items()` returns.
    enum class MenuAction : int {
        toggle_favourite = 1,
        rename,
        add_tag,
        remove_tag_base = 100, ///< remove_tag_base + i removes the i-th tag
        assign_program = 50,
        clear_program,
    };

    /// Text the host should ask the user for. The browser has no text field of
    /// its own: it asks, the host shows a dialog, and the answer comes back
    /// through `complete_text_request`.
    enum class TextRequestKind { rename, add_tag, assign_program };
    struct TextRequest {
        TextRequestKind kind = TextRequestKind::rename;
        std::string title;   ///< "Rename", "Add tag", "MIDI program (bank:program)"
        std::string initial; ///< Current value to prefill
    };

    /// Set to receive a right-click on a row: the row is selected, and the host
    /// shows the items (for example with `ContextMenu::show`) and passes the
    /// chosen id back to `perform_menu_action`.
    std::function<void(Point position, std::vector<ContextMenu::Item> items)> on_context_menu;
    /// Set to receive requests for text. Without it those menu actions do nothing.
    std::function<void(const TextRequest&)> on_text_request;

    /// The menu for the selected row; empty without a library or a selection.
    std::vector<ContextMenu::Item> context_menu_items() const {
        std::vector<ContextMenu::Item> items;
        const auto* s = selected_summary();
        if (!s)
            return items;
        items.push_back(ContextMenu::Item::make_header(s->display_name));
        {
            ContextMenu::Item fav;
            fav.id = static_cast<int>(MenuAction::toggle_favourite);
            fav.label = s->favourite ? "Remove from favourites" : "Add to favourites";
            items.push_back(std::move(fav));
        }
        items.push_back({static_cast<int>(MenuAction::rename), "Rename..."});
        items.push_back({static_cast<int>(MenuAction::add_tag), "Add tag..."});
        for (size_t i = 0; i < s->tags.size(); ++i)
            items.push_back({static_cast<int>(MenuAction::remove_tag_base) + static_cast<int>(i),
                             "Remove tag #" + s->tags[i]});
        items.push_back(ContextMenu::Item::make_separator());
        items.push_back({static_cast<int>(MenuAction::assign_program),
                         s->program
                             ? "MIDI program " + state::format_bank_program(*s->program) + "..."
                             : std::string("Assign MIDI program...")});
        ContextMenu::Item clear;
        clear.id = static_cast<int>(MenuAction::clear_program);
        clear.label = "Clear MIDI program";
        clear.enabled = s->program.has_value();
        items.push_back(std::move(clear));
        return items;
    }

    /// Run a menu item chosen from `context_menu_items()`. Returns false for an
    /// unknown id, a disabled action or no selection.
    bool perform_menu_action(int id) {
        const auto* s = selected_summary();
        if (!s)
            return false;
        const auto action = static_cast<MenuAction>(id);
        if (id >= static_cast<int>(MenuAction::remove_tag_base)) {
            const size_t i =
                static_cast<size_t>(id - static_cast<int>(MenuAction::remove_tag_base));
            if (i >= s->tags.size())
                return false;
            return remove_tag_from_selected(std::string(s->tags[i]));
        }
        switch (action) {
        case MenuAction::toggle_favourite:
            return toggle_favourite_selected();
        case MenuAction::rename:
            return request_text({TextRequestKind::rename, "Rename", s->display_name});
        case MenuAction::add_tag:
            return request_text({TextRequestKind::add_tag, "Add tag", ""});
        case MenuAction::assign_program:
            return request_text({TextRequestKind::assign_program, "MIDI program (bank:program)",
                                 s->program ? state::format_bank_program(*s->program) : ""});
        case MenuAction::clear_program: {
            if (!s->program)
                return false;
            return edit_selected_program(
                [&](const std::string&) { return library_->clear_program(*s->program); });
        }
        default:
            return false;
        }
    }

    /// The host's answer to a `TextRequest`. An empty rename clears the
    /// override, an empty tag is refused, and a program that is not
    /// "bank:program" in range is refused. Returns whether the edit applied.
    bool complete_text_request(TextRequestKind kind, const std::string& text) {
        switch (kind) {
        case TextRequestKind::rename:
            return rename_selected(text);
        case TextRequestKind::add_tag:
            return !text.empty() && add_tag_to_selected(text);
        case TextRequestKind::assign_program: {
            const auto position = state::parse_bank_program(text);
            if (!position)
                return false;
            return edit_selected_program(
                [&](const std::string& key) { return library_->assign_program(*position, key); });
        }
        }
        return false;
    }

    // ── Library mode ────────────────────────────────────────────────────

    /// Attach a library (not owned; it must outlive the browser) or detach with
    /// nullptr. Switches the list to the library's query results.
    void set_library(state::PresetLibrary* library) {
        library_ = library;
        refresh();
    }
    bool has_library() const {
        return library_ != nullptr;
    }

    /// Only favourites. No effect without a library.
    void set_favourites_only(bool only) {
        favourites_only_ = only;
        refresh();
    }
    /// Only presets carrying this tag (empty clears). No effect without a library.
    void set_tag_filter(const std::string& tag) {
        tag_filter_ = tag;
        refresh();
    }

    /// The library summary behind the selected row, or nullptr.
    const state::PresetSummary* selected_summary() const {
        if (!library_ || selected_index_ < 0 ||
            selected_index_ >= static_cast<int>(summaries_.size()))
            return nullptr;
        return &summaries_[static_cast<size_t>(selected_index_)];
    }
    const state::PresetSummary* summary_at(int index) const {
        if (!library_ || index < 0 || index >= static_cast<int>(summaries_.size()))
            return nullptr;
        return &summaries_[static_cast<size_t>(index)];
    }

    /// Edits on the selected row. Each returns false without a library or a
    /// selection, and re-queries so the list reflects the change.
    bool toggle_favourite_selected() {
        const auto* s = selected_summary();
        if (!s)
            return false;
        return edit_selected(
            [&](const std::string& key) { return library_->set_favourite(key, !s->favourite); });
    }
    bool add_tag_to_selected(const std::string& tag) {
        return edit_selected([&](const std::string& key) { return library_->add_tag(key, tag); });
    }
    bool remove_tag_from_selected(const std::string& tag) {
        return edit_selected(
            [&](const std::string& key) { return library_->remove_tag(key, tag); });
    }
    /// An empty name clears the override and shows the file name again.
    bool rename_selected(const std::string& display_name) {
        return edit_selected(
            [&](const std::string& key) { return library_->set_display_name(key, display_name); });
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

            const state::PresetSummary* summary = summary_at(i);

            // Factory/user indicator, or the favourite star in library mode
            canvas.set_font("system", 10);
            if (summary) {
                canvas.set_fill_color(
                    summary->favourite ? resolve_color("accent", canvas::Color::hex(0xe94560))
                                       : resolve_color("text_muted", canvas::Color::hex(0x606070)));
                canvas.fill_text(summary->favourite ? "\xe2\x98\x85" : "\xe2\x98\x86", // ★ ☆
                                 b.x + 6, y + row_height_ * 0.65f);
            } else if (preset.is_factory) {
                canvas.set_fill_color(resolve_color("text_muted",
                    canvas::Color::hex(0x606070)));
                canvas.fill_text("F", b.x + 6, y + row_height_ * 0.65f);
            }

            // Preset name
            canvas.set_font("system", 13);
            canvas.set_fill_color(resolve_color("text",
                canvas::Color::hex(0xe0e0e0)));
            canvas.fill_text(summary ? summary->display_name : preset.name, b.x + 22,
                             y + row_height_ * 0.65f);

            if (summary) {
                std::string extra;
                for (const auto& t : summary->tags)
                    extra += (extra.empty() ? "" : " ") + ("#" + t);
                if (summary->program)
                    extra += (extra.empty() ? "" : "  ") + std::to_string(summary->program->bank) +
                             ":" + std::to_string(summary->program->program);
                if (!extra.empty()) {
                    canvas.set_font("system", 10);
                    canvas.set_fill_color(
                        resolve_color("text_muted", canvas::Color::hex(0x808090)));
                    canvas.fill_text(extra, b.x + b.width - 130, y + row_height_ * 0.65f);
                }
            }

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

        // Right-click on a row selects it and offers the library menu.
        if (library_ && event.button == MouseButton::right && on_context_menu) {
            const float menu_list_y = 32.0f + (filter_text_.empty() ? 4.0f : 22.0f);
            if (event.position.y >= menu_list_y) {
                const int row = static_cast<int>((event.position.y - menu_list_y + scroll_offset_) /
                                                 row_height_);
                if (row >= 0 && row < static_cast<int>(filtered_.size())) {
                    selected_index_ = row;
                    notify_selection();
                    on_context_menu(event.position, context_menu_items());
                }
            }
            return;
        }

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
            // The star column toggles the favourite instead of selecting.
            if (library_ && event.position.x < 20.0f) {
                toggle_favourite_selected();
                return;
            }
            notify_selection();

            if (event.click_count == 2 && on_preset_activated) {
                on_preset_activated(filtered_[static_cast<size_t>(index)]);
            }
        }
    }

    bool on_key_event(const KeyEvent& event) override {
        if (!event.is_down) return false;

        if (library_ && event.key == KeyCode::f && !event.isMainModifier() && selected_summary()) {
            toggle_favourite_selected();
            return true;
        }
        if (event.key == KeyCode::up) { select_previous(); return true; }
        if (event.key == KeyCode::down) { select_next(); return true; }
        if (event.key == KeyCode::enter && selected_preset()) {
            if (on_preset_activated) on_preset_activated(*selected_preset());
            return true;
        }
        return false;
    }

    bool wants_mouse_input() const override { return true; }

private:
    state::PresetManager& manager_;
    std::vector<state::PresetInfo> all_presets_;
    std::vector<state::PresetInfo> filtered_;
    std::string filter_text_;
    ShowMode show_mode_ = ShowMode::all;
    int selected_index_ = -1;
    float row_height_ = 24.0f;
    float scroll_offset_ = 0.0f;
    state::PresetLibrary* library_ = nullptr;
    std::vector<state::PresetSummary> summaries_; ///< Parallel to filtered_ in library mode
    bool favourites_only_ = false;
    std::string tag_filter_;

    bool request_text(const TextRequest& request) {
        if (!on_text_request)
            return false;
        on_text_request(request);
        return true;
    }

    // A program-map edit changes the rows' program numbers, so it re-queries
    // exactly as a metadata edit does.
    template <typename Edit> bool edit_selected_program(Edit&& edit) {
        return edit_selected(edit);
    }

    template <typename Edit> bool edit_selected(Edit&& edit) {
        const auto* s = selected_summary();
        if (!s)
            return false;
        const std::string key = s->key; // the query below invalidates s
        const bool ok = edit(key);
        apply_library_query(key);
        return ok;
    }

    void apply_library_query(const std::string& keep_key = {}) {
        std::string selected = keep_key;
        if (selected.empty())
            if (const auto* s = selected_summary())
                selected = s->key;

        state::PresetQuery q;
        q.text = filter_text_;
        if (favourites_only_)
            q.favourite = true;
        if (!tag_filter_.empty())
            q.tags_all = {tag_filter_};
        if (show_mode_ == ShowMode::factory_only)
            q.source = state::PresetSource::factory;
        // user_only: every source that is not factory; filtered below.
        auto page = library_->query(q);

        summaries_.clear();
        filtered_.clear();
        for (auto& s : page.items) {
            if (show_mode_ == ShowMode::user_only && s.is_factory)
                continue;
            state::PresetInfo info;
            info.name = s.name;
            info.path = s.path;
            info.is_factory = s.is_factory;
            info.folder = s.folder;
            info.author = s.author;
            info.tags = s.tags;
            filtered_.push_back(std::move(info));
            summaries_.push_back(std::move(s));
        }

        selected_index_ = -1;
        for (size_t i = 0; i < summaries_.size(); ++i)
            if (!selected.empty() && summaries_[i].key == selected)
                selected_index_ = static_cast<int>(i);
        if (selected_index_ < 0 && !filtered_.empty() && !selected.empty())
            selected_index_ = 0;
    }

    void apply_filter() {
        if (library_) {
            apply_library_query();
            return;
        }
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
