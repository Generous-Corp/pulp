#pragma once

// Per-OS message-loop integration shims.
//
// This header exposes a thin, cross-platform API surface that callers can
// use to introspect and cooperate with the host's native main message loop
// (Cocoa NSRunLoop / Win MsgWaitForMultipleObjects / Linux GLib MainLoop
// or X11/Wayland event sources).
//
// Background:
//   PR #2825 shipped `MainThreadDispatcher` — a process-wide bridge that
//   lets any Pulp subsystem post work to whatever native main-thread queue
//   a platform owner has registered. The macOS plugin-mode helper
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
// What this header does NOT do:
//   - Wrap CFRunLoop / HWND / GMainLoop primitives — adapters that need
//     those should still include the platform-specific header from
//     `core/events/src/<platform>/`. This header is the *cross-platform
//     introspection* surface, not a re-implementation of every native
//     loop API.
//
// Platform coverage (as of 2026-05-26):
//   - macOS / iOS  — Cocoa NSRunLoop backend (shipped via #2825 + plugin
//     helper in `plugin_main_thread.hpp`).
//   - Windows      — MsgWaitForMultipleObjects backend pending; the
//     dispatcher will report `MainLoopKind::None` until the standalone
//     Win32 host or VST3 editor registers one.
//   - Linux        — GLib / X11 / Wayland backends pending; same
//     `MainLoopKind::None` story until a backend lands.
//
// The gap-doc rows that track per-OS backend implementations live in
// `planning/2026-05-24-reference-framework-gap-analysis.md`. This header
// is the long-promised *cross-platform API surface* — callers can adopt
// it today and the OS-side backends light up as they ship.

#include <pulp/events/main_thread_dispatcher.hpp>

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

/// Cross-platform introspection of the active main loop.
///
/// All methods are thread-safe. Backends register themselves with the
/// process-wide `MainThreadDispatcher` and the returned `MainLoopKind`
/// follows whatever the most-recent active registration is.
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
