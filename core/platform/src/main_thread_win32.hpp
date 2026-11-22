#pragma once

// Windows fallback for MainThreadScope and MainThreadTimer when no host run
// loop is registered: a message-only window on the thread that first needs one,
// with SetTimer for timers and PostMessage for posts. It is a HostRunLoop, so
// the scope and timer code in main_thread_host.cpp drives it exactly as it
// drives a host's loop; only the wake-up for posts differs (a posted message
// instead of a descriptor).
//
// Internal to pulp-platform.

#include <pulp/platform/main_thread.hpp>

#include <functional>

namespace pulp::platform::detail {

/// The fallback loop. A process-wide object that is never destroyed; its window
/// exists only while at least one scope uses it.
HostRunLoop& win32_fallback_loop() noexcept;

/// Make the fallback usable from the calling thread, creating its window here
/// when none exists. False when another live thread owns the window, or the
/// window cannot be created. UI thread.
bool win32_fallback_acquire();

/// Destroy the window and unregister its class, once no scope uses it. Safe to
/// call when none exists. UI thread.
void win32_fallback_release();

/// Whether the calling thread owns the live window.
bool win32_fallback_on_ui_thread() noexcept;

/// Ask the owner thread to run `drain` from its message loop. Any thread. Posts
/// from one burst share one window message. Returns false when no window
/// exists.
bool win32_fallback_request_drain(std::function<void()> drain);

} // namespace pulp::platform::detail
