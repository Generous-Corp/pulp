#include <pulp/view/window_manager.hpp>
#include <algorithm>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace pulp::view {

WindowManager::WindowManager() = default;
WindowManager::~WindowManager() = default;

// ── Window registration ─────────────────────────────────────────────────────

WindowId WindowManager::register_window(WindowHost* host, View* root_view,
                                         WindowType type, WindowId parent_id) {
    WindowId id = next_id_++;
    WindowRecord rec;
    rec.id = id;
    rec.type = type;
    rec.parent_id = parent_id;
    rec.host = host;
    rec.root_view = root_view;
    rec.alive = true;

    // Apply shared theme to the new window's root view
    if (root_view && !shared_theme_.colors.empty())
        root_view->set_theme(shared_theme_);

    windows_[id] = rec;
    return id;
}

void WindowManager::unregister_window(WindowId id) {
    auto it = windows_.find(id);
    if (it == windows_.end()) return;

    it->second.alive = false;

    // Remove any message handler
    handlers_.erase(id);

    if (on_window_closed_)
        on_window_closed_(id);

    windows_.erase(it);
}

const WindowRecord* WindowManager::window(WindowId id) const {
    auto it = windows_.find(id);
    return it != windows_.end() ? &it->second : nullptr;
}

std::vector<WindowId> WindowManager::window_ids() const {
    std::vector<WindowId> ids;
    ids.reserve(windows_.size());
    for (auto& [id, _] : windows_)
        ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::vector<WindowId> WindowManager::children_of(WindowId parent_id) const {
    std::vector<WindowId> children;
    for (auto& [id, rec] : windows_) {
        if (rec.parent_id == parent_id)
            children.push_back(id);
    }
    std::sort(children.begin(), children.end());
    return children;
}

size_t WindowManager::window_count() const {
    return windows_.size();
}

// ── Cascading close ─────────────────────────────────────────────────────────

void WindowManager::collect_descendants(WindowId id, std::vector<WindowId>& out) const {
    for (auto& [wid, rec] : windows_) {
        if (rec.parent_id == id) {
            collect_descendants(wid, out);
            out.push_back(wid);
        }
    }
}

void WindowManager::close_window(WindowId id) {
    // Collect descendants depth-first (children before parent in the list,
    // so closing order is leaves-first).
    std::vector<WindowId> to_close;
    collect_descendants(id, to_close);
    to_close.push_back(id); // Parent last

    for (WindowId wid : to_close) {
        auto it = windows_.find(wid);
        if (it == windows_.end()) continue;

        if (it->second.host)
            it->second.host->request_close();

        unregister_window(wid);
    }
}

void WindowManager::set_on_window_closed(std::function<void(WindowId)> cb) {
    on_window_closed_ = std::move(cb);
}

// ── Theme propagation ───────────────────────────────────────────────────────

void WindowManager::set_shared_theme(const Theme& theme) {
    shared_theme_ = theme;
    for (auto& [_, rec] : windows_) {
        if (rec.root_view)
            rec.root_view->set_theme(theme);
    }
}

const Theme& WindowManager::shared_theme() const {
    return shared_theme_;
}

// ── Inter-window messaging ──────────────────────────────────────────────────

void WindowManager::send_message(WindowId target, const WindowMessage& msg) {
    auto it = handlers_.find(target);
    if (it != handlers_.end())
        it->second(msg);
}

void WindowManager::broadcast_message(const WindowMessage& msg) {
    for (auto& [id, handler] : handlers_)
        handler(msg);
}

void WindowManager::set_message_handler(WindowId id, MessageHandler handler) {
    if (windows_.find(id) == windows_.end()) return;
    handlers_[id] = std::move(handler);
}

// ── Screen enumeration (platform-specific) ──────────────────────────────────

#if defined(__APPLE__) && !TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
// macOS implementation is in platform/mac/window_manager_mac.mm
#else

std::vector<ScreenInfo> WindowManager::available_screens() {
    // Fallback: report a single default screen
    ScreenInfo primary;
    primary.id = 0;
    primary.x = 0;
    primary.y = 0;
    primary.width = 1920;
    primary.height = 1080;
    primary.scale_factor = 1.0f;
    primary.is_primary = true;
    primary.name = "Default Screen";
    return {primary};
}

ScreenInfo WindowManager::primary_screen() {
    auto screens = available_screens();
    for (auto& s : screens) {
        if (s.is_primary) return s;
    }
    return screens.empty() ? ScreenInfo{} : screens[0];
}

#endif

// ── Window state save / restore ─────────────────────────────────────────────

std::unordered_map<WindowId, WindowState> WindowManager::save_all_states() const {
    std::unordered_map<WindowId, WindowState> states;
    for (auto& [id, rec] : windows_)
        states[id] = rec.state;
    return states;
}

void WindowManager::restore_state(WindowId id, const WindowState& state) {
    auto it = windows_.find(id);
    if (it != windows_.end())
        it->second.state = state;
}

// ── GPU device sharing ──────────────────────────────────────────────────────

void WindowManager::set_shared_gpu_device(void* device_handle) {
    shared_gpu_device_ = device_handle;
}

void* WindowManager::shared_gpu_device() const {
    return shared_gpu_device_;
}

// ── Multi-window capability query ───────────────────────────────────────────

bool WindowManager::is_multi_window_supported() {
#if TARGET_OS_IPHONE || TARGET_OS_SIMULATOR
    return false;  // iOS: single window only
#else
    return true;   // macOS, Windows, Linux
#endif
}

} // namespace pulp::view
