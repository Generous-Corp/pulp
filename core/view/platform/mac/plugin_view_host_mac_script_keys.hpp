#pragma once

#import <Cocoa/Cocoa.h>

#include "window_host_mac_internal.hpp"
#include <pulp/view/script_event_dispatch.hpp>
#include <pulp/view/view.hpp>

namespace pulp::view {

// AppKit may offer the same press as a key equivalent and then as keyDown.
// Keep its identity alive until the next press so an unconsumed JS event is
// delivered once while both native entry points still return it to the DAW.
class PluginScriptKeys {
  public:
    PluginScriptKeys() = default;
    PluginScriptKeys(const PluginScriptKeys&) = delete;
    PluginScriptKeys& operator=(const PluginScriptKeys&) = delete;
    ~PluginScriptKeys() {
        [event_ release];
    }

    bool dispatch(View* root, NSEvent* event) {
        if (!root || !event)
            return false;
        if (event_ == event)
            return handled_;
        [event_ release];
        event_ = [event retain];
        handled_ = false;
        try {
            handled_ = script_events::dispatch_key_for_root(
                *root, static_cast<int>(mac_geometry::key_code_from_ns(event.keyCode)),
                mac_geometry::modifiers_from_ns_flags(event.modifierFlags), true);
            if (handled_)
                root->request_repaint();
        } catch (...) {
            // A failed script must not leave the host's keyboard captured.
            handled_ = false;
        }
        return handled_;
    }

  private:
    NSEvent* event_ = nil;
    bool handled_ = false;
};

} // namespace pulp::view
