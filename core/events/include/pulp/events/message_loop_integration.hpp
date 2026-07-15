#pragma once

// Per-OS message-loop integration shims.
//
// This header exposes a thin, cross-platform API surface that callers can
// use to introspect and cooperate with the host's native main message loop
// (Cocoa NSRunLoop / Win MsgWaitForMultipleObjects / Linux GLib MainLoop
// or X11/Wayland event sources).
//
// Background:
//   `MainThreadDispatcher` is a process-wide bridge that lets any Pulp
//   subsystem post work to whatever native main-thread queue a platform
//   owner has registered. The macOS plugin-mode helper
//   (`plugin_main_thread.hpp`) adds a refcounted Cocoa backend so format
//   adapters can register at instantiation time. This header sits one
//   level up: it exposes the *kind* of native loop that's currently
//   integrated, so cross-platform code can branch on capabilities (e.g.
//   "do I have a Cocoa run loop I can attach a CFRunLoopObserver to?")
//   without including platform headers.
//
// What this header DOES:
//   - Reports the active backend kind (`MainLoopKind`).
//   - Forwards `call_async` / `call_sync` / `is_main_thread` to the
//     underlying `MainThreadDispatcher` so callers can write one piece
//     of cross-platform code.
//   - Documents the per-OS contract (callback thread, re-entrancy, etc.)
//     for the four loops Pulp targets.
//
// What this header additionally does for headless native hosts:
//   - Offers one bounded main-loop service slice on Apple platforms. This
//     lets offline tools advance plug-ins whose initialization depends on
//     XPC, timers, or main-queue callbacks without taking ownership of the
//     application's event loop.
//
// Platform coverage:
//   - macOS / iOS  — Cocoa NSRunLoop backend via the plugin-mode helper
//     in `plugin_main_thread.hpp`.
//   - Windows      — MsgWaitForMultipleObjects backend pending; the
//     dispatcher will report `MainLoopKind::None` until the standalone
//     Win32 host or VST3 editor registers one.
//   - Linux        — GLib / X11 / Wayland backends pending; same
//     `MainLoopKind::None` story until a backend lands.

#include <pulp/events/main_thread_dispatcher.hpp>

#include <chrono>
#include <cstdint>
#include <string_view>

namespace pulp::events {

/// The kind of native main loop currently integrated.
///
/// This is reported by whichever backend is active in
/// `MainThreadDispatcher`. It is `None` when no backend is registered —
/// e.g. a unit test process, a headless server, or a Pulp plugin loaded
/// into a host that hasn't called `register_plugin_backend()`.
enum class MainLoopKind : std::uint8_t {
    None        = 0,    ///< No native main loop integration.
    Cocoa       = 1,    ///< macOS / iOS NSRunLoop / CFRunLoop.
    Win32       = 2,    ///< Windows MsgWaitForMultipleObjects / GetMessage.
    GLib        = 3,    ///< Linux glib GMainLoop (GNOME / GTK hosts).
    X11         = 4,    ///< Bare X11 XNextEvent loop.
    Wayland     = 5,    ///< Wayland wl_display_dispatch loop.
    Custom      = 255,  ///< Caller-registered loop (SDL, custom dispatcher).
};

/// Outcome of a bounded native-main-loop service slice.
enum class MainLoopPumpResult : std::uint8_t {
    HandledSource = 0, ///< At least one native source was handled.
    TimedOut      = 1, ///< The bounded interval elapsed without a source.
    Stopped       = 2, ///< The native loop was explicitly stopped.
    Finished      = 3, ///< The native loop has no sources or timers.
    Unsupported   = 4, ///< This platform has no bounded pump implementation.
    WrongThread   = 5, ///< The call was not made on the process main thread.
};

/// Cross-platform introspection of the active main loop.
///
/// The introspection and dispatch methods are thread-safe. Backends register
/// themselves with the process-wide `MainThreadDispatcher` and the returned
/// `MainLoopKind` follows whatever the most-recent active registration is.
/// `pump_main_loop_for()` is deliberately main-thread-only.
class MessageLoopIntegration {
public:
    /// Returns the kind of native loop currently active, or `None`.
    static MainLoopKind active_kind();

    /// Human-readable name for diagnostics ("cocoa", "win32", "glib",
    /// "x11", "wayland", "custom", "none"). Stable across releases —
    /// the value is documented in `docs/reference/`.
    static std::string_view active_name();

    /// Convenience: any non-None backend is registered. Equivalent to
    /// `active_kind() != MainLoopKind::None` and forwards to the
    /// `MainThreadDispatcher::has_backend()` check the underlying
    /// implementation uses.
    static bool available();

    /// Forwards to `MainThreadDispatcher::is_main_thread()`.
    static bool is_main_thread();

    /// Forwards to `MainThreadDispatcher::call_async()`.
    /// Returns false if no backend is registered.
    static bool post(Task task);

    /// Forwards to `MainThreadDispatcher::call_sync()`.
    /// Returns false if no backend is registered.
    static bool call_sync(Task task);

    /// Service the native process main loop for at most `max_duration`.
    ///
    /// Apple hosts use this to advance dispatch-main, CFRunLoop, Cocoa, XPC,
    /// and timer work while rendering headlessly. It must be called from the
    /// process main/control thread, never from an audio callback. A zero or
    /// negative duration performs a non-blocking poll. Other platforms return
    /// `Unsupported` until they grow an equivalent bounded implementation.
    ///
    /// This reports event-loop progress only. It is not a plug-in readiness or
    /// licensing signal; callers remain responsible for their own warm-up and
    /// settle policy.
    static MainLoopPumpResult
    pump_main_loop_for(std::chrono::milliseconds max_duration);

    /// Register the *kind* tag for a custom backend. Call this AFTER
    /// `MainThreadDispatcher::register_backend()` returns a token, so
    /// the introspection layer reports the correct value. Pulp's own
    /// platform backends (Cocoa, Win32, GLib, ...) call this internally
    /// — third-party SDL / Tauri-style integrations should call it
    /// directly with `MainLoopKind::Custom`.
    ///
    /// `token` is the value returned by `MainThreadDispatcher::register_backend`.
    /// `unregister_kind(token)` removes the tag; if you forget, the tag
    /// stays until the process ends but the dispatcher's own teardown
    /// still cleans up the actual backend.
    static void register_kind(MainThreadDispatcher::Token token, MainLoopKind kind);

    /// Clear the kind tag for a token registered via `register_kind`.
    /// Safe to call with an unknown token (no-op).
    static void unregister_kind(MainThreadDispatcher::Token token);
};

// ─── Per-OS contract notes (documentation surface) ──────────────────────
//
// macOS / iOS — Cocoa (MainLoopKind::Cocoa)
//   - Callback thread: the AppKit / UIKit main thread.
//   - Re-entrancy: `call_sync()` from the main thread runs inline; from
//     a worker thread it dispatches via `dispatch_get_main_queue()` and
//     blocks until the block runs.
//   - Cooperation with hosts: the plugin-mode backend in
//     `plugin_main_thread.hpp` is refcounted; safe to call register /
//     unregister once per loaded plugin instance.
//
// Windows — Win32 (MainLoopKind::Win32) — backend pending
//   - Expected callback thread: the thread that owns the message pump
//     (the thread that called `GetMessage` / `MsgWaitForMultipleObjects`).
//   - Re-entrancy: `call_sync()` from the pump thread should run inline;
//     from a worker thread it should `PostMessage` to a hidden window
//     and block on an event.
//
// Linux — GLib / X11 / Wayland — backends pending
//   - GLib hosts: register via `g_main_context_invoke` for `call_async`
//     and a `GMutex` + `GCond` for `call_sync`.
//   - X11 hosts: post to a self-pipe that the main thread reads under
//     `select()`; the X11 event loop ferries the wakeup.
//   - Wayland hosts: use `wl_display_dispatch_pending()` plus a self-pipe
//     wakeup; otherwise indistinguishable from X11 from the caller's POV.

} // namespace pulp::events
