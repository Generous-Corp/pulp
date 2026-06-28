#pragma once

#include <pulp/events/event_loop.hpp>
#include <cstdint>
#include <functional>

namespace pulp::events {

// Process-wide bridge to the native UI/main message loop.
//
// Platform owners register a backend while their native event loop is alive.
// Callers can then marshal work to that loop without depending on AppKit,
// UIKit, SDL, or another platform-specific queue API.
//
// unregister_backend() waits for any in-flight backend callbacks selected by
// this dispatcher to finish before returning. If a backend unregisters itself
// from inside one of its callbacks, the registration is removed immediately
// and unregister waits for any other threads already inside that backend's
// callbacks before allowing the current callback to finish. A backend that
// accepts work is still responsible for owning whatever state is needed until
// that work runs.
class MainThreadDispatcher {
public:
    using Token = std::uint64_t;

    struct Backend {
        std::function<bool(Task)> post;
        std::function<bool()> is_main_thread;
        // Optional: post `task` to the main thread after `delay_ms`
        // milliseconds. Lets callers run a paced main-thread poll without
        // busy-reposting. A backend that cannot schedule may leave this empty;
        // `call_async_after` then falls back to an immediate `post`.
        std::function<bool(Task, int /*delay_ms*/)> post_after;
    };

    // Registers a backend and makes it active. If another live backend was
    // already active, it is restored when the newer token unregisters.
    static Token register_backend(Backend backend);

    // Removes a live registration. Returns false for unknown or zero tokens.
    static bool unregister_backend(Token token);

    static bool has_backend();
    static bool is_main_thread();

    // Posts work to the active main-thread backend. Returns false if no backend
    // accepts the task. Task exceptions are swallowed so they cannot escape into
    // native event queues.
    static bool call_async(Task task);

    // Posts work to the active main-thread backend after `delay_ms`
    // milliseconds. Falls back to an immediate `call_async` if the backend has
    // no `post_after`. Returns false if no backend accepts the task. Task
    // exceptions are swallowed. Used to drive a paced (non-busy) main-thread
    // poll loop.
    static bool call_async_after(Task task, int delay_ms);

    // Runs work on the active main thread, blocking the caller when needed.
    // Returns false if no backend accepts the task. Task exceptions propagate
    // back to the caller.
    static bool call_sync(Task task);
};

} // namespace pulp::events
