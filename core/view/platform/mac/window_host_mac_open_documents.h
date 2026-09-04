// window_host_mac_open_documents.h — private forward declarations for the
// NSApplication delegate that receives files the OS asks the app to open.
//
// The implementations live in window_host_mac_open_documents.mm. This header is
// only consumed by window_host_mac.mm and is not part of the public SDK
// surface.
//
// Opening a document has two halves, and each is inert alone. Declaring the
// type in Info.plist (pulp_declare_standalone_document_type() in CMake) is what
// makes Launch Services route a double-click to this app; the delegate behind
// this header is what makes the app hear about it. Declaring without handling
// gives the worst outcome: the right icon, a successful launch, and a file that
// is silently dropped.
//
// The delegate is process-global rather than per-window, which is why it lives
// here and not in the window host.

#pragma once

#include <functional>
#include <string>
#include <vector>

#ifdef __OBJC__
// Per-binary-unique ObjC class names (renames PulpAppDelegate when a shipped
// binary defines PULP_VIEW_OBJC_SUFFIX); ObjC class names are process-global,
// so every separately loaded Pulp binary needs its own.
#include "pulp_mac_objc_names.h"

namespace pulp::view::mac_open_documents {

// Install the app delegate, once, without ever displacing one that is already
// there.
//
// MUST be called before [NSApp run]. A launch-time open is delivered around
// applicationDidFinishLaunching, which is inside the run loop — so a delegate
// installed after the loop starts misses precisely the file that launched the
// app, which is the common case.
//
// A Pulp binary loaded into somebody else's app (a plug-in in a DAW) shares
// that host's NSApplication and must not take over its delegate, so an existing
// delegate is left alone. Hosted binaries then never receive open events, which
// is correct: the host owns its documents.
void install_app_delegate();

// Route files the OS asks this app to open to `handler`, installing the
// delegate as a side effect. Paths that arrived before any handler existed are
// flushed to it immediately, so a launch-time open is never lost.
void set_open_files_handler(
    std::function<void(const std::vector<std::string>&)> handler);

}  // namespace pulp::view::mac_open_documents

#endif  // __OBJC__
