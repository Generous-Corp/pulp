#include <pulp/events/message_loop_integration.hpp>

#include <mutex>
#include <unordered_map>

namespace pulp::events {

namespace {

struct KindRegistry {
    std::mutex mu;
    std::unordered_map<MainThreadDispatcher::Token, MainLoopKind> tags;
    MainLoopKind active = MainLoopKind::None;

    static KindRegistry& instance() {
        static KindRegistry r;
        return r;
    }
};

std::string_view kind_name(MainLoopKind kind) {
    switch (kind) {
        case MainLoopKind::None:    return "none";
        case MainLoopKind::Cocoa:   return "cocoa";
        case MainLoopKind::Win32:   return "win32";
        case MainLoopKind::GLib:    return "glib";
        case MainLoopKind::X11:     return "x11";
        case MainLoopKind::Wayland: return "wayland";
        case MainLoopKind::Custom:  return "custom";
    }
    return "unknown";
}

} // namespace

MainLoopKind MessageLoopIntegration::active_kind() {
    if (!MainThreadDispatcher::has_backend()) {
        return MainLoopKind::None;
    }
    auto& r = KindRegistry::instance();
    std::lock_guard<std::mutex> lock(r.mu);
    return r.active;
}

std::string_view MessageLoopIntegration::active_name() {
    return kind_name(active_kind());
}

bool MessageLoopIntegration::available() {
    return MainThreadDispatcher::has_backend();
}

bool MessageLoopIntegration::is_main_thread() {
    return MainThreadDispatcher::is_main_thread();
}

bool MessageLoopIntegration::post(Task task) {
    return MainThreadDispatcher::call_async(std::move(task));
}

bool MessageLoopIntegration::call_sync(Task task) {
    return MainThreadDispatcher::call_sync(std::move(task));
}

void MessageLoopIntegration::register_kind(MainThreadDispatcher::Token token,
                                           MainLoopKind kind) {
    if (token == 0) return;
    auto& r = KindRegistry::instance();
    std::lock_guard<std::mutex> lock(r.mu);
    r.tags[token] = kind;
    // The most-recently-registered kind is treated as active. The
    // underlying dispatcher already has push-down semantics (older
    // backends are restored on newer-token unregister), so report the
    // newest registration to match.
    r.active = kind;
}

void MessageLoopIntegration::unregister_kind(MainThreadDispatcher::Token token) {
    if (token == 0) return;
    auto& r = KindRegistry::instance();
    std::lock_guard<std::mutex> lock(r.mu);
    auto it = r.tags.find(token);
    if (it == r.tags.end()) return;
    r.tags.erase(it);

    // Restore the most-recent surviving tag, or None if nothing's left.
    // We don't track insertion order in the map — for the small (1..3)
    // set of expected registrations the simplest thing is to pick the
    // largest token (tokens are monotonically increasing in the
    // dispatcher), so the value reflects "most recent".
    MainLoopKind best = MainLoopKind::None;
    MainThreadDispatcher::Token best_tok = 0;
    for (auto& kv : r.tags) {
        if (kv.first > best_tok) {
            best_tok = kv.first;
            best = kv.second;
        }
    }
    r.active = best;
}

} // namespace pulp::events
