#pragma once

// Plugin-mode main-thread cooperation (item 6.4b macOS plan, post-PR-#2825).
//
// PR #2825 delivered the cross-platform `MainThreadDispatcher` contract; on
// standalone macOS apps `WindowHost::create()` registers a Cocoa backend that
// posts to `dispatch_get_main_queue()`. In plugin mode (AU v3 / VST3 / CLAP
// loaded inside a DAW like Logic / Cubase / Bitwig), there is no Pulp window
// — the DAW owns the main `NSApplication`/`CFRunLoop`. Without an explicit
// registration, `MainThreadDispatcher::call_async` returns false and any
// adapter-side code that asks "please run this on the host's main thread"
// silently no-ops.
//
// The helpers in this header let format adapters and plugin instance owners
// register a process-wide Cocoa backend at instantiation time and unregister
// it at teardown. On non-macOS platforms the calls degrade to no-ops so
// adapter code can call them unconditionally; the dispatcher then simply
// stays in its "no backend" state on those platforms (Windows / Linux own
// their own message-loop integration paths — see gap-doc Phase 1).
//
// Reference-counted under the hood — call register_plugin_backend() once
// per loaded plugin instance, and `unregister_plugin_backend()` once on
// teardown. The first register installs a backend; subsequent registers bump
// the refcount; the last unregister removes the backend. Safe to call from
// any thread.

#include <pulp/events/main_thread_dispatcher.hpp>

namespace pulp::events {

/// Register a process-wide Cocoa main-thread backend for plugin-mode use.
/// Returns a token that must be passed to `unregister_plugin_backend()`.
///
/// Returns `0` on non-macOS platforms — callers should still pair the call
/// with a corresponding `unregister_plugin_backend(0)` for symmetry; the
/// unregister will no-op on a zero token.
MainThreadDispatcher::Token register_plugin_backend();

/// Tear down a backend installed via `register_plugin_backend()`. Returns
/// true if the token was live and removed (or the call was a benign no-op
/// for a zero token on a non-Apple build).
bool unregister_plugin_backend(MainThreadDispatcher::Token token);

/// Whether the host platform has a plugin-mode backend implementation.
/// True on macOS; false elsewhere. Tests and adapters can branch on this
/// to keep platform-agnostic assertions tight.
bool plugin_backend_available();

} // namespace pulp::events
