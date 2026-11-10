// Internal to pulp-native: the input half of MetalSurface's view. Objective-C++.

#pragma once

#include <pulp/native/metal_surface.hpp>

#import <AppKit/AppKit.h>

#include <string>

namespace pulp::native::detail {

/// Input state one surface keeps for its view. Main thread.
struct SurfaceInput {
    InputHandler handler;
    Cursor cursor = Cursor::arrow;
    bool text_focus = false;
    bool pointer_inside = false;
    bool drop_accepted = false;
    std::string marked;
    NSTrackingArea* tracking = nil;
    // The host's first responder when the client took text focus, restored
    // when the client gives it back.
    __weak NSResponder* prior_responder = nil;

    void emit_focus(bool focused, FocusReason reason) {
        if (handler.focus) {
            auto callback = handler.focus;
            callback(FocusEvent{focused, reason});
        }
    }
};

/// The input state behind a surface view, or null once the surface is gone.
/// Defined with MetalSurface.
SurfaceInput* surface_input_of(id view);
/// The registered surface view class. Defined with MetalSurface.
Class surface_view_class();
/// Adds the responder, tracking, text-input and dragging methods. Called while
/// the class is being configured, before it is registered.
void add_input_methods(Class cls);

} // namespace pulp::native::detail
