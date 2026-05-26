// macOS Cocoa backend registration for MainThreadDispatcher in plugin mode
// (item 6.4b macOS plan).
//
// Adapter code (AU v3 / VST3 / CLAP on macOS) calls register_plugin_backend()
// at instance construction time and unregister_plugin_backend() at teardown.
// The first registration installs a backend that posts onto
// `dispatch_get_main_queue()`; the last unregister removes it. Reference-
// counted so multiple plugin instances sharing the process behave correctly.

#include <pulp/events/plugin_main_thread.hpp>

#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>

#include <atomic>
#include <memory>
#include <mutex>

namespace pulp::events {

namespace {

struct PluginBackendRegistry {
    std::mutex mu;
    int refcount = 0;
    MainThreadDispatcher::Token token = 0;
    std::shared_ptr<std::atomic<bool>> alive;
};

PluginBackendRegistry& registry() {
    static PluginBackendRegistry r;
    return r;
}

MainThreadDispatcher::Backend make_backend(std::shared_ptr<std::atomic<bool>> alive) {
    return {
        [alive](Task task) -> bool {
            if (!task) return false;
            if (!alive || !alive->load(std::memory_order_acquire)) return false;
            // dispatch_async copies the block; the captured heap_task is owned
            // by the block via a unique_ptr re-binding inside it.
            auto* heap_task = new Task(std::move(task));
            dispatch_async(dispatch_get_main_queue(), ^{
                std::unique_ptr<Task> owned(heap_task);
                if (*owned) (*owned)();
            });
            return true;
        },
        [alive] {
            if (!alive || !alive->load(std::memory_order_acquire)) return false;
            return static_cast<bool>([NSThread isMainThread]);
        },
    };
}

} // namespace

MainThreadDispatcher::Token register_plugin_backend() {
    auto& r = registry();
    std::lock_guard lock(r.mu);
    r.refcount++;
    if (r.refcount == 1) {
        // First plugin instance — install the backend.
        r.alive = std::make_shared<std::atomic<bool>>(true);
        r.token = MainThreadDispatcher::register_backend(make_backend(r.alive));
        if (r.token == 0) {
            // Registration failed. Roll back so unregister_plugin_backend()
            // doesn't underflow.
            r.refcount = 0;
            r.alive.reset();
        }
    }
    return r.token;
}

bool unregister_plugin_backend(MainThreadDispatcher::Token token) {
    auto& r = registry();
    std::lock_guard lock(r.mu);
    if (token == 0 || token != r.token || r.refcount == 0) {
        return false;
    }
    r.refcount--;
    if (r.refcount == 0) {
        if (r.alive) r.alive->store(false, std::memory_order_release);
        const bool ok = MainThreadDispatcher::unregister_backend(r.token);
        r.token = 0;
        r.alive.reset();
        return ok;
    }
    return true;
}

bool plugin_backend_available() {
    return true;
}

} // namespace pulp::events
