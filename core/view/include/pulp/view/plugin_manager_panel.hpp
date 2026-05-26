#pragma once

/// @file plugin_manager_panel.hpp
/// Host-side "manage plugins" UI widget built on top of the existing
/// scanner / scan_cache / scan_blacklist backend.
///
/// The panel shows three buckets — scanned, failed, blacklisted — with the
/// plugin's format, path, and last-scan timestamp. It wraps a non-blocking
/// rescan, per-format search-path editors, a right-click menu, a
/// search/filter box, and screen-reader labels. The widget itself is a
/// pure-View subclass; host applications provide the scan backend via a
/// `PluginManagerModel` so the widget can be driven by an in-process scanner
/// in production and a mock scanner in tests.

#include <pulp/view/view.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/canvas/canvas.hpp>

#include <pulp/host/scanner.hpp>
#include <pulp/host/scan_blacklist.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::view {

/// A single row in the manager's three-bucket display.
struct PluginManagerRow {
    pulp::host::PluginFormat format = pulp::host::PluginFormat::CLAP;
    std::string name;                 ///< Plugin display name (may be empty for failed scans)
    std::string path;                 ///< Absolute path to the plugin bundle/binary
    std::string reason;               ///< For failed/blacklisted rows: why
    std::int64_t last_scan_unix = 0;  ///< Seconds-since-epoch; 0 = "never"

    // ── Item 4.3 — drag-add identity ─────────────────────────────────────
    // The fields below are populated by the model so a drag-add into a
    // SignalGraph can synthesize the PluginInfo without having to re-scan
    // or look the row up by path. They are intentionally optional — older
    // models that pre-date item 4.3 leave them at defaults and the
    // resulting drop produces an "unresolved" graph node (PluginSlot::load
    // is still attempted via `to_plugin_info`).
    std::string manufacturer;
    std::string version;
    std::string unique_id;
    int num_inputs = 2;
    int num_outputs = 2;
    bool is_instrument = false;
    bool is_effect = true;

    /// Convert this row back into a `pulp::host::PluginInfo` suitable for
    /// SignalGraph::add_plugin_node(). Lossy by construction — only the
    /// fields the manager panel tracks survive.
    pulp::host::PluginInfo to_plugin_info() const {
        pulp::host::PluginInfo info;
        info.name = name;
        info.manufacturer = manufacturer;
        info.version = version;
        info.path = path;
        info.unique_id = unique_id;
        info.format = format;
        info.is_instrument = is_instrument;
        info.is_effect = is_effect;
        info.num_inputs = num_inputs;
        info.num_outputs = num_outputs;
        return info;
    }
};

/// Which bucket a row belongs to.
enum class PluginManagerBucket : std::uint8_t {
    scanned = 0,
    failed = 1,
    blacklisted = 2,
};

/// Model interface the widget calls into. Hosts plug their scanner /
/// cache / blacklist backing store behind this, which lets the widget stay
/// unit-testable against mocks.
///
/// Methods marked "UI thread" are called from the widget paint / input
/// handlers. `start_rescan` MUST be non-blocking — real implementations
/// dispatch to a worker thread or the out-of-process `pulp-scan-worker`.
class PluginManagerModel {
public:
    virtual ~PluginManagerModel() = default;

    /// Snapshot the current bucket contents. Called frequently while the
    /// panel is visible; implementations should back this with an atomic
    /// swap rather than re-scanning.
    virtual std::vector<PluginManagerRow> rows(PluginManagerBucket bucket) const = 0;

    /// Format-specific search paths the user has configured. The widget's
    /// per-format editor mutates these via add/remove calls below.
    virtual std::vector<std::string> search_paths(pulp::host::PluginFormat fmt) const = 0;
    virtual void add_search_path(pulp::host::PluginFormat fmt, std::string path) = 0;
    virtual void remove_search_path(pulp::host::PluginFormat fmt, const std::string& path) = 0;

    /// Kick off a non-blocking rescan. Returns immediately. Progress is
    /// reported through `progress_fraction()` and `is_scanning()` below.
    virtual void start_rescan() = 0;

    /// Kick off a non-blocking rescan for exactly one plugin path. Used by
    /// the "Rescan just this plugin" context menu item.
    virtual void start_rescan(const std::string& path) = 0;

    /// Rescan progress, in the range [0, 1]. Always 1.0 when not scanning.
    virtual float progress_fraction() const = 0;

    /// True while a rescan is in flight.
    virtual bool is_scanning() const = 0;

    /// Blacklist toggle. Calling `set_blacklisted(path, true)` must persist
    /// across sessions via the underlying `pulp::host::ScanBlacklist`.
    virtual bool is_blacklisted(const std::string& path) const = 0;
    virtual void set_blacklisted(const std::string& path, bool blacklisted,
                                 const std::string& reason = "user") = 0;

    /// Reveal a plugin bundle in the OS file manager ("Show in Finder" on
    /// macOS, "Show in Explorer" on Windows, xdg-open on Linux). Optional
    /// — default is a no-op so tests don't have to stub it.
    virtual void reveal_in_file_manager(const std::string& /*path*/) {}
};

// ── Mock / in-memory model for tests and examples ────────────────────────

/// A simple in-memory model that just hands out pre-populated rows. Used
/// by the headless test and the demo example. Real hosts subclass
/// `PluginManagerModel` directly and wire it to a live `PluginScanner`.
class InMemoryPluginManagerModel : public PluginManagerModel {
public:
    std::vector<PluginManagerRow> scanned_rows;
    std::vector<PluginManagerRow> failed_rows;
    std::unordered_map<int, std::vector<std::string>> paths_by_format;
    pulp::host::ScanBlacklist blacklist;

    // Externally observable progress state — tests can tick this by hand.
    std::atomic<float> progress{1.0f};
    std::atomic<bool> scanning{false};

    /// Count of start_rescan() calls — lets tests verify the button wired
    /// through without having to stand up a real worker.
    int rescan_count = 0;
    int single_rescan_count = 0;
    std::string last_single_rescan_path;
    int reveal_count = 0;
    std::string last_reveal_path;

    std::vector<PluginManagerRow> rows(PluginManagerBucket bucket) const override {
        switch (bucket) {
            case PluginManagerBucket::scanned:
                return filter_scanned();
            case PluginManagerBucket::failed:
                return failed_rows;
            case PluginManagerBucket::blacklisted:
                return blacklist_rows();
        }
        return {};
    }

    std::vector<std::string> search_paths(pulp::host::PluginFormat fmt) const override {
        auto it = paths_by_format.find(static_cast<int>(fmt));
        if (it == paths_by_format.end()) return {};
        return it->second;
    }
    void add_search_path(pulp::host::PluginFormat fmt, std::string path) override {
        auto& v = paths_by_format[static_cast<int>(fmt)];
        if (std::find(v.begin(), v.end(), path) == v.end())
            v.push_back(std::move(path));
    }
    void remove_search_path(pulp::host::PluginFormat fmt, const std::string& path) override {
        auto& v = paths_by_format[static_cast<int>(fmt)];
        v.erase(std::remove(v.begin(), v.end(), path), v.end());
    }

    void start_rescan() override { ++rescan_count; }
    void start_rescan(const std::string& path) override {
        ++single_rescan_count;
        last_single_rescan_path = path;
    }
    float progress_fraction() const override { return progress.load(); }
    bool is_scanning() const override { return scanning.load(); }

    bool is_blacklisted(const std::string& path) const override {
        return blacklist.is_blacklisted(path);
    }
    void set_blacklisted(const std::string& path, bool on,
                         const std::string& reason) override {
        if (on) blacklist.blacklist(path, reason.empty() ? "user" : reason);
        else    blacklist.clear(path);
    }
    void reveal_in_file_manager(const std::string& path) override {
        ++reveal_count;
        last_reveal_path = path;
    }

private:
    std::vector<PluginManagerRow> filter_scanned() const {
        // Scanned bucket excludes anything the user has blacklisted.
        std::vector<PluginManagerRow> out;
        out.reserve(scanned_rows.size());
        for (const auto& r : scanned_rows) {
            if (!blacklist.is_blacklisted(r.path)) out.push_back(r);
        }
        return out;
    }
    std::vector<PluginManagerRow> blacklist_rows() const {
        std::vector<PluginManagerRow> out;
        for (const auto& [path, entry] : blacklist.entries()) {
            PluginManagerRow row;
            row.path = path;
            row.reason = entry.reason;
            row.last_scan_unix = entry.mtime;
            // Pull format / name from the scanned list if we remember it.
            for (const auto& s : scanned_rows) {
                if (s.path == path) {
                    row.format = s.format;
                    row.name = s.name;
                    break;
                }
            }
            out.push_back(std::move(row));
        }
        return out;
    }
};

// ── The widget itself ────────────────────────────────────────────────────

/// Three-column "manage plugins" panel.
///
/// The widget is format-agnostic and drives everything through the model.
/// The layout is:
///
///   [ search box ][ Rescan ][ progress bar ]
///   ┌──────────────┬──────────────┬──────────────┐
///   │  Scanned     │  Failed      │  Blacklisted │
///   ├──────────────┼──────────────┼──────────────┤
///   │  rows …      │  rows …      │  rows …      │
///   └──────────────┴──────────────┴──────────────┘
///
/// Right-clicking a row shows a context menu (tracked on the widget as
/// `context_menu_path()` + `context_menu_items()`; hosts render their own
/// native popup when available).
class PluginManagerPanel : public View {
public:
    explicit PluginManagerPanel(PluginManagerModel& model) : model_(&model) {
        set_focusable(true);
        set_access_role(AccessRole::group);
        set_access_label("Plugin manager");
        refresh();
    }

    // ── Observable state (public for tests) ──────────────────────────────

    /// Refresh all three buckets from the model. Call after the model's
    /// backing data changes (e.g. a rescan completed).
    void refresh() {
        scanned_     = model_->rows(PluginManagerBucket::scanned);
        failed_      = model_->rows(PluginManagerBucket::failed);
        blacklisted_ = model_->rows(PluginManagerBucket::blacklisted);
        apply_filter();
    }

    /// Number of rows currently visible in a bucket (after filter).
    int visible_count(PluginManagerBucket b) const {
        return static_cast<int>(visible(b).size());
    }
    const std::vector<PluginManagerRow>& rows(PluginManagerBucket b) const {
        return visible(b);
    }

    /// Set / read the search filter. Empty string shows everything.
    void set_filter(std::string text) {
        filter_ = std::move(text);
        apply_filter();
    }
    const std::string& filter() const { return filter_; }

    /// Read-only accessor for the model — exposed so example apps can drive
    /// the same object they handed the widget.
    PluginManagerModel& model() { return *model_; }
    const PluginManagerModel& model() const { return *model_; }

    // ── Rescan button ────────────────────────────────────────────────────

    /// Trigger a full non-blocking rescan via the model. Exposed as a
    /// public method so tests can invoke it without synthesising mouse
    /// events.
    void trigger_rescan() { model_->start_rescan(); }

    /// Trigger a rescan for exactly one plugin path.
    void trigger_rescan(const std::string& path) { model_->start_rescan(path); }

    /// Toggle blacklist on a specific path; persists via model.
    void toggle_blacklist(const std::string& path) {
        const bool now = !model_->is_blacklisted(path);
        model_->set_blacklisted(path, now, "user");
        refresh();
    }

    /// Ask the model to reveal the row in the OS file manager.
    void reveal_in_file_manager(const std::string& path) {
        model_->reveal_in_file_manager(path);
    }

    // ── Per-format search paths ──────────────────────────────────────────

    std::vector<std::string> search_paths(pulp::host::PluginFormat fmt) const {
        return model_->search_paths(fmt);
    }
    void add_search_path(pulp::host::PluginFormat fmt, std::string path) {
        model_->add_search_path(fmt, std::move(path));
    }
    void remove_search_path(pulp::host::PluginFormat fmt, const std::string& path) {
        model_->remove_search_path(fmt, path);
    }

    // ── Context menu (right-click) ───────────────────────────────────────

    /// Path of the row the user most recently right-clicked, or empty.
    const std::string& context_menu_path() const { return context_menu_path_; }
    PluginManagerBucket context_menu_bucket() const { return context_menu_bucket_; }

    /// Menu item strings in display order. Stable enum avoids string
    /// matching in tests.
    enum class ContextItem : std::uint8_t {
        rescan_this = 0,
        toggle_blacklist = 1,
        reveal_in_file_manager = 2,
    };
    static std::vector<ContextItem> context_menu_items() {
        return {ContextItem::rescan_this,
                ContextItem::toggle_blacklist,
                ContextItem::reveal_in_file_manager};
    }

    /// Menu label for a `ContextItem`. The blacklist label flips depending
    /// on the current state of `context_menu_path()`.
    std::string context_menu_label(ContextItem item) const {
        switch (item) {
            case ContextItem::rescan_this:
                return "Rescan just this plugin";
            case ContextItem::toggle_blacklist:
                if (!context_menu_path_.empty() && model_->is_blacklisted(context_menu_path_))
                    return "Remove from blacklist";
                return "Add to blacklist";
            case ContextItem::reveal_in_file_manager:
#if defined(_WIN32)
                return "Show in Explorer";
#elif defined(__APPLE__)
                return "Show in Finder";
#else
                return "Show in file manager";
#endif
        }
        return {};
    }

    /// Activate a menu item against `context_menu_path()`. No-op when the
    /// context path is empty. Public so tests and hosts can drive menu
    /// selection without faking a platform popup.
    void activate_context_item(ContextItem item) {
        if (context_menu_path_.empty()) return;
        switch (item) {
            case ContextItem::rescan_this:
                trigger_rescan(context_menu_path_);
                break;
            case ContextItem::toggle_blacklist:
                toggle_blacklist(context_menu_path_);
                break;
            case ContextItem::reveal_in_file_manager:
                reveal_in_file_manager(context_menu_path_);
                break;
        }
    }

    /// Open the context menu against a specific row — used by tests and
    /// platform right-click handlers alike.
    void open_context_menu(PluginManagerBucket b, const std::string& path) {
        context_menu_bucket_ = b;
        context_menu_path_ = path;
    }
    void close_context_menu() { context_menu_path_.clear(); }

    // ── Accessibility labels for the three columns ──────────────────────
    //
    // Exposed so external a11y checkers and tests can verify that each
    // column carries a meaningful screen-reader label independent of the
    // visual rendering. Values deliberately include the row counts.
    std::string column_access_label(PluginManagerBucket b) const {
        const auto& v = visible(b);
        const char* name =
            b == PluginManagerBucket::scanned ? "Scanned plugins"
          : b == PluginManagerBucket::failed  ? "Failed plugins"
          :                                     "Blacklisted plugins";
        return std::string(name) + " — " + std::to_string(v.size()) +
               (v.size() == 1 ? " entry" : " entries");
    }

    // ── View overrides ──────────────────────────────────────────────────

    void paint(canvas::Canvas& canvas) override {
        const auto b = local_bounds();

        canvas.set_fill_color(resolve_color("plugin_manager_bg",
            canvas::Color::hex(0x16213e)));
        canvas.fill_rounded_rect(b.x, b.y, b.width, b.height, 6);

        // ── Toolbar: search + rescan + progress bar ──────────────────────
        const float toolbar_h = 28.0f;
        canvas.set_fill_color(resolve_color("surface",
            canvas::Color::hex(0x1a1a2e)));
        canvas.fill_rect(b.x, b.y, b.width, toolbar_h);

        canvas.set_font("system", 12);
        canvas.set_fill_color(resolve_color("text",
            canvas::Color::hex(0xe0e0e0)));
        const std::string filter_label =
            filter_.empty() ? "Search: (type to filter)" : "Search: " + filter_;
        canvas.fill_text(filter_label, b.x + 8, b.y + 18);

        const float button_w = 96.0f;
        const float button_x = b.x + b.width - button_w - 8.0f;
        canvas.set_fill_color(resolve_color("accent",
            canvas::Color::hex(0xe94560)));
        canvas.fill_rounded_rect(button_x, b.y + 4, button_w, toolbar_h - 8, 4);
        canvas.set_fill_color(canvas::Color::hex(0xffffff));
        canvas.fill_text(model_->is_scanning() ? "Scanning…" : "Rescan",
                          button_x + 12, b.y + 18);

        if (model_->is_scanning()) {
            const float pb_x = b.x + b.width - button_w - 8.0f - 120.0f;
            const float pb_w = 110.0f;
            canvas.set_fill_color(resolve_color("progress_track",
                canvas::Color::hex(0x0f3460)));
            canvas.fill_rect(pb_x, b.y + 10, pb_w, 8);
            canvas.set_fill_color(resolve_color("progress_fill",
                canvas::Color::hex(0x4ade80)));
            const float f = std::clamp(model_->progress_fraction(), 0.0f, 1.0f);
            canvas.fill_rect(pb_x, b.y + 10, pb_w * f, 8);
        }

        // ── Three columns ───────────────────────────────────────────────
        const float col_w = (b.width - 16.0f) / 3.0f;
        const float col_y = b.y + toolbar_h + 4.0f;
        const float col_h = b.height - toolbar_h - 8.0f;

        paint_column(canvas, b.x + 4.0f,                col_y, col_w, col_h,
                     PluginManagerBucket::scanned,     "Scanned");
        paint_column(canvas, b.x + 4.0f + col_w,       col_y, col_w, col_h,
                     PluginManagerBucket::failed,      "Failed");
        paint_column(canvas, b.x + 4.0f + col_w * 2.0f, col_y, col_w, col_h,
                     PluginManagerBucket::blacklisted, "Blacklisted");
    }

    void on_mouse_event(const MouseEvent& event) override {
        if (!event.is_down) return;

        const auto b = local_bounds();
        const float toolbar_h = 28.0f;

        // Rescan button
        const float button_w = 96.0f;
        const float button_x = b.x + b.width - button_w - 8.0f;
        if (event.position.y < toolbar_h &&
            event.position.x >= button_x && event.position.x <= button_x + button_w) {
            trigger_rescan();
            return;
        }

        // Column hit test
        const float col_w = (b.width - 16.0f) / 3.0f;
        const int col_idx = static_cast<int>((event.position.x - 4.0f) / col_w);
        const PluginManagerBucket bucket = col_idx <= 0
            ? PluginManagerBucket::scanned
            : col_idx == 1
                ? PluginManagerBucket::failed
                : PluginManagerBucket::blacklisted;

        const float col_y = toolbar_h + 4.0f;
        const float header_h = 20.0f;
        const float row_y = event.position.y - col_y - header_h;
        if (row_y < 0) return;

        const auto& rows_vec = visible(bucket);
        const int row = static_cast<int>(row_y / row_height_);
        if (row < 0 || row >= static_cast<int>(rows_vec.size())) {
            close_context_menu();
            return;
        }

        const auto& hit = rows_vec[static_cast<size_t>(row)];
        if (event.button == MouseButton::right) {
            open_context_menu(bucket, hit.path);
        } else {
            selected_path_ = hit.path;
            selected_bucket_ = bucket;
            if (on_row_selected) on_row_selected(bucket, hit);
        }
    }

    // ── Drag-add into a SignalGraph (item 4.3) ───────────────────────────
    //
    // The panel itself is rendering-only — it does not know what surface
    // (graph editor, mixer column, etc.) the user is dropping onto. We
    // emit a `on_row_drag_start` callback when the user presses on a
    // scanned row and drags past `drag_threshold_px_`. The host is
    // responsible for taking the row's `PluginInfo` and adding the node
    // to whichever `SignalGraph` the drop landed on (see
    // `pulp::host::add_plugin_node_from_row`).
    //
    // The drag callback fires only for rows in the `scanned` bucket —
    // dragging a failed or blacklisted entry into the graph would create
    // a node that can never load, so the panel suppresses it.

    void on_mouse_down(Point pos) override {
        drag_origin_ = pos;
        drag_origin_set_ = true;
        drag_started_ = false;
        drag_row_ = identify_row_at(pos);
    }
    void on_mouse_drag(Point pos) override {
        if (drag_origin_set_ && !drag_started_ && drag_row_.has_value()) {
            const float dx = pos.x - drag_origin_.x;
            const float dy = pos.y - drag_origin_.y;
            if (dx * dx + dy * dy >= drag_threshold_px_ * drag_threshold_px_) {
                drag_started_ = true;
                if (on_row_drag_start && drag_row_->first == PluginManagerBucket::scanned) {
                    on_row_drag_start(drag_row_->first, drag_row_->second, pos);
                }
            }
        }
    }
    void on_mouse_up(Point pos) override {
        if (drag_started_ && drag_row_.has_value() && on_row_drag_end) {
            on_row_drag_end(drag_row_->first, drag_row_->second, pos);
        }
        drag_origin_set_ = false;
        drag_started_ = false;
        drag_row_.reset();
    }

    /// Programmatic drag — bypasses the per-frame mouse plumbing so a
    /// host or test can fire the drag-add callback for a specific row
    /// without synthesising motion events. The row is looked up by its
    /// path. Returns true when a matching row was found and a non-empty
    /// `on_row_drag_start` callback fired; false otherwise.
    bool simulate_row_drag(PluginManagerBucket bucket,
                           const std::string& path,
                           Point drop_position = {0, 0})
    {
        const auto& rows_vec = visible(bucket);
        for (const auto& r : rows_vec) {
            if (r.path == path) {
                if (bucket == PluginManagerBucket::scanned && on_row_drag_start) {
                    on_row_drag_start(bucket, r, drop_position);
                }
                if (on_row_drag_end) {
                    on_row_drag_end(bucket, r, drop_position);
                }
                return true;
            }
        }
        return false;
    }

    /// Pixel threshold a drag must exceed before `on_row_drag_start` fires.
    /// Defaults to 4 px to match common desktop conventions.
    float drag_threshold_px() const { return drag_threshold_px_; }
    void set_drag_threshold_px(float px) { drag_threshold_px_ = px; }

    /// Fired once per drag, when the user has moved past
    /// `drag_threshold_px()` after pressing on a scanned row.
    std::function<void(PluginManagerBucket, const PluginManagerRow&, Point)> on_row_drag_start;

    /// Fired on mouse-up after a drag started. Hosts that build a live
    /// drop-target preview tear it down here.
    std::function<void(PluginManagerBucket, const PluginManagerRow&, Point)> on_row_drag_end;

    bool on_key_event(const KeyEvent& event) override {
        if (!event.is_down) return false;
        // Type-to-filter: printable keys land in `on_text_input` on most
        // backends, but we also support backspace here so focus-only flows
        // still work in tests.
        if (event.key == KeyCode::backspace && !filter_.empty()) {
            filter_.pop_back();
            apply_filter();
            return true;
        }
        return false;
    }
    void on_text_input(const TextInputEvent& event) override {
        filter_ += event.text;
        apply_filter();
    }

    // Callback fired when a row is left-clicked. Optional.
    std::function<void(PluginManagerBucket, const PluginManagerRow&)> on_row_selected;

    // Row height is exposed so tests can synthesize click coordinates.
    float row_height() const { return row_height_; }
    void set_row_height(float h) { row_height_ = h; }

private:
    PluginManagerModel* model_;

    std::vector<PluginManagerRow> scanned_, failed_, blacklisted_;
    std::vector<PluginManagerRow> scanned_view_, failed_view_, blacklisted_view_;

    std::string filter_;
    std::string selected_path_;
    PluginManagerBucket selected_bucket_ = PluginManagerBucket::scanned;

    std::string context_menu_path_;
    PluginManagerBucket context_menu_bucket_ = PluginManagerBucket::scanned;

    float row_height_ = 22.0f;

    // Drag-add state (item 4.3)
    Point drag_origin_{0, 0};
    bool drag_origin_set_ = false;
    bool drag_started_ = false;
    std::optional<std::pair<PluginManagerBucket, PluginManagerRow>> drag_row_;
    float drag_threshold_px_ = 4.0f;

    std::optional<std::pair<PluginManagerBucket, PluginManagerRow>>
    identify_row_at(Point pos) const {
        const auto b = local_bounds();
        const float toolbar_h = 28.0f;
        if (pos.y < toolbar_h) return std::nullopt;

        const float col_w = (b.width - 16.0f) / 3.0f;
        const int col_idx = static_cast<int>((pos.x - 4.0f) / col_w);
        const PluginManagerBucket bucket = col_idx <= 0
            ? PluginManagerBucket::scanned
            : col_idx == 1
                ? PluginManagerBucket::failed
                : PluginManagerBucket::blacklisted;

        const float header_h = 20.0f;
        const float row_y = pos.y - toolbar_h - 4.0f - header_h;
        if (row_y < 0) return std::nullopt;

        const auto& rows_vec = visible(bucket);
        const int row = static_cast<int>(row_y / row_height_);
        if (row < 0 || row >= static_cast<int>(rows_vec.size())) return std::nullopt;
        return std::make_pair(bucket, rows_vec[static_cast<size_t>(row)]);
    }

    const std::vector<PluginManagerRow>& visible(PluginManagerBucket b) const {
        switch (b) {
            case PluginManagerBucket::scanned:     return scanned_view_;
            case PluginManagerBucket::failed:      return failed_view_;
            case PluginManagerBucket::blacklisted: return blacklisted_view_;
        }
        return scanned_view_;
    }

    static std::string to_lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    void apply_filter() {
        scanned_view_      = apply_filter_to(scanned_);
        failed_view_       = apply_filter_to(failed_);
        blacklisted_view_  = apply_filter_to(blacklisted_);
    }
    std::vector<PluginManagerRow> apply_filter_to(const std::vector<PluginManagerRow>& in) const {
        if (filter_.empty()) return in;
        const std::string f = to_lower(filter_);
        std::vector<PluginManagerRow> out;
        out.reserve(in.size());
        for (const auto& r : in) {
            if (to_lower(r.name).find(f) != std::string::npos ||
                to_lower(r.path).find(f) != std::string::npos) {
                out.push_back(r);
            }
        }
        return out;
    }

    static const char* format_label(pulp::host::PluginFormat f) {
        switch (f) {
            case pulp::host::PluginFormat::VST3:         return "VST3";
            case pulp::host::PluginFormat::AudioUnit:    return "AU";
            case pulp::host::PluginFormat::AudioUnitV3:  return "AUv3";
            case pulp::host::PluginFormat::CLAP:         return "CLAP";
            case pulp::host::PluginFormat::LV2:          return "LV2";
        }
        return "?";
    }

    static std::string format_time(std::int64_t unix_seconds) {
        if (unix_seconds <= 0) return "never";
        std::time_t t = static_cast<std::time_t>(unix_seconds);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
        return buf;
    }

    void paint_column(canvas::Canvas& canvas,
                      float x, float y, float w, float h,
                      PluginManagerBucket bucket,
                      const char* title)
    {
        canvas.set_fill_color(resolve_color("list_bg",
            canvas::Color::hex(0x0d1b33)));
        canvas.fill_rect(x, y, w, h);

        // Column header
        const float header_h = 20.0f;
        canvas.set_fill_color(resolve_color("surface",
            canvas::Color::hex(0x1a1a2e)));
        canvas.fill_rect(x, y, w, header_h);

        canvas.set_font("system", 12);
        canvas.set_fill_color(resolve_color("text",
            canvas::Color::hex(0xe0e0e0)));
        const std::string label = std::string(title) + " (" +
            std::to_string(visible(bucket).size()) + ")";
        canvas.fill_text(label, x + 6, y + 14);

        // Rows
        canvas.save();
        canvas.clip_rect(x, y + header_h, w, h - header_h);
        float ry = y + header_h + 2.0f;
        const auto& rows_vec = visible(bucket);
        for (const auto& r : rows_vec) {
            canvas.set_font("system", 11);
            const std::string fmt_name = format_label(r.format);
            const std::string headline = fmt_name + "  " +
                (r.name.empty() ? r.path : r.name);
            canvas.set_fill_color(resolve_color("text",
                canvas::Color::hex(0xe0e0e0)));
            canvas.fill_text(headline, x + 6, ry + 12);
            canvas.set_fill_color(resolve_color("text_muted",
                canvas::Color::hex(0x808090)));
            canvas.set_font("system", 10);
            canvas.fill_text(format_time(r.last_scan_unix), x + 6, ry + 24);
            ry += row_height_;
            if (ry > y + h) break;
        }
        canvas.restore();
    }
};

} // namespace pulp::view
