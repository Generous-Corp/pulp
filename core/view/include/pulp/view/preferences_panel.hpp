#pragma once

/// @file preferences_panel.hpp
/// PreferencesPanel — sidebar-of-pages container for app preferences
/// (closes the gap-doc P3 row "PreferencesPanel").
///
/// `PreferencesPanel` is a `View` subclass that hosts a sidebar of named
/// "pages". Each page is a (title, icon, content) tuple; the panel
/// shows the sidebar entries on the left and the active page's content
/// view filling the rest. Setting the active page is one of:
///   - `set_active_page(index)` — programmatic switch.
///   - `set_active_page(title)` — switch by title (no-op if not found).
///   - clicking a sidebar row (drives the same switch via the host's
///     mouse routing; tests can call `simulate_select(index)` instead).
///
/// **Persistence**: a `PreferencesPanel` can be bound to a
/// `pulp::state::PropertiesFile` (typically the user lane of
/// `ApplicationProperties`) so the last-selected page survives
/// across runs. The persistence key defaults to
/// `"preferences.active_page"` and is configurable via
/// `set_persistence_key`. `bind_persistence()` does a one-shot read
/// of the stored title and selects the matching page (no-op if the
/// page is gone). Subsequent `set_active_page` calls write back.
///
/// Headless-friendly: pages register `View` content but `PreferencesPanel`
/// never paints in tests — it just owns the registry + active-index state.
///
/// License-lineage note: the name is Pulp-native per the gap-doc rule.

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <pulp/state/properties_file.hpp>
#include <pulp/view/view.hpp>

namespace pulp::view {

class PreferencesPanel : public View {
public:
    struct Page {
        std::string title;
        std::string icon;    ///< Icon glyph / asset id; opaque to the panel.
        View* content = nullptr;  ///< Non-owning — actual ownership is in
                                  ///  the View tree (children_), so the
                                  ///  page entry just remembers the slot.
    };

    PreferencesPanel() = default;

    // ── Page registry ────────────────────────────────────────────────

    /// Register a new page. The first registered page becomes active
    /// unless an explicit `set_active_page` runs first. `content` is
    /// added as a child of the panel so its layout/paint are wired
    /// through the standard View tree.
    void add_page(std::string title,
                  std::string icon,
                  std::unique_ptr<View> content) {
        auto* content_raw = content.get();
        // First page must establish active_=0; later pages stay hidden.
        if (!pages_.empty()) {
            content_raw->set_visible(false);
        }
        add_child(std::move(content));
        pages_.push_back({std::move(title), std::move(icon), content_raw});
        if (pages_.size() == 1) {
            active_ = 0;
            notify_change();
        }
    }

    size_t page_count() const { return pages_.size(); }

    /// Returns nullptr if `index` is out of range. The returned View*
    /// is owned by the panel (added as a child).
    View* page_content(size_t index) const {
        return index < pages_.size() ? pages_[index].content : nullptr;
    }
    const std::string& page_title(size_t index) const {
        return index < pages_.size() ? pages_[index].title : empty_string();
    }
    const std::string& page_icon(size_t index) const {
        return index < pages_.size() ? pages_[index].icon : empty_string();
    }

    // ── Active page ──────────────────────────────────────────────────

    int active_page() const { return active_; }
    std::string active_page_title() const {
        return (active_ >= 0 && static_cast<size_t>(active_) < pages_.size())
                   ? pages_[static_cast<size_t>(active_)].title
                   : std::string{};
    }

    /// Switch by index. No-op when out of range.
    void set_active_page(int index) {
        if (index < 0 || static_cast<size_t>(index) >= pages_.size()) return;
        if (index == active_) return;
        // Hide previous, show new.
        if (active_ >= 0 && static_cast<size_t>(active_) < pages_.size()) {
            if (auto* v = pages_[static_cast<size_t>(active_)].content) v->set_visible(false);
        }
        active_ = index;
        if (auto* v = pages_[static_cast<size_t>(active_)].content) v->set_visible(true);
        persist_active();
        notify_change();
    }

    /// Switch by title. Returns true if a matching page was found.
    bool set_active_page(std::string_view title) {
        for (size_t i = 0; i < pages_.size(); ++i) {
            if (pages_[i].title == title) {
                set_active_page(static_cast<int>(i));
                return true;
            }
        }
        return false;
    }

    /// Test helper — mirrors what a sidebar click would do.
    void simulate_select(int index) { set_active_page(index); }

    // ── Persistence ──────────────────────────────────────────────────

    /// Bind to a JSON-backed `PropertiesFile` (typically
    /// `ApplicationProperties::user_settings()`). One-shot read of the
    /// stored page title selects it now if present; subsequent
    /// selections write back under `persistence_key()`.
    void bind_persistence(pulp::state::PropertiesFile* props) {
        props_ = props;
        if (!props_) return;
        if (auto stored = props_->get_string(persistence_key_)) {
            set_active_page(*stored);
        }
    }
    /// Detach from persistence without touching the store.
    void clear_persistence() { props_ = nullptr; }
    bool has_persistence() const { return props_ != nullptr; }

    void set_persistence_key(std::string key) {
        persistence_key_ = std::move(key);
    }
    const std::string& persistence_key() const { return persistence_key_; }

    // ── Observers ────────────────────────────────────────────────────

    std::function<void(int active_index)> on_page_change;

private:
    void persist_active() const {
        if (!props_) return;
        if (active_ < 0 || static_cast<size_t>(active_) >= pages_.size()) return;
        props_->set_string(persistence_key_, pages_[static_cast<size_t>(active_)].title);
    }
    void notify_change() {
        if (on_page_change) on_page_change(active_);
    }
    static const std::string& empty_string() {
        static const std::string s;
        return s;
    }

    std::vector<Page> pages_;                 ///< title + icon + non-owning content view ptr.
    int active_ = -1;
    pulp::state::PropertiesFile* props_ = nullptr;
    std::string persistence_key_ = "preferences.active_page";
};

} // namespace pulp::view
