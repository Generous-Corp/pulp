// window_host_mac_geometry.mm — geometry, coordinate, and event-
// translation helpers extracted from window_host_mac.mm.
//
// R2-5 refactor: these are free functions / former file-local statics
// that do NOT touch PulpView ivars or any Obj-C instance state — pure
// coordinate math, NSEvent→Pulp event translation, View tree walks, and
// NSWindow / NSView configuration. The PulpView @implementation block,
// its ivars, and its category/delegate methods stay in
// window_host_mac.mm.
//
// Declarations: window_host_mac_internal.hpp (private; not in any
// public include tree). Only consumed by window_host_mac.mm.

#include "window_host_mac_internal.hpp"

#include <TargetConditionals.h>
#if TARGET_OS_OSX

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/modal.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/window_manager.hpp>

#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace pulp::view::mac_geometry {

void request_app_close(NSWindow* window) {
    NSWindow* target = window != nil ? window : [NSApp keyWindow];
    if (target == nil) target = [NSApp mainWindow];
    if (target != nil) {
        [target performClose:nil];
    } else {
        [NSApp stop:nil];
    }
}

pulp::view::ModalOverlay* find_topmost_modal(pulp::view::View* root) {
    if (!root || !root->visible()) return nullptr;

    for (size_t i = root->child_count(); i > 0; --i) {
        if (auto* modal = find_topmost_modal(root->child_at(i - 1)))
            return modal;
    }

    return dynamic_cast<pulp::view::ModalOverlay*>(root);
}

uint16_t modifiers_from_ns_flags(NSEventModifierFlags flags) {
    uint16_t m = 0;
    if (flags & NSEventModifierFlagShift)   m |= pulp::view::kModShift;
    if (flags & NSEventModifierFlagControl) m |= pulp::view::kModCtrl;
    if (flags & NSEventModifierFlagOption)  m |= pulp::view::kModAlt;
    if (flags & NSEventModifierFlagCommand) m |= pulp::view::kModCmd;
    return m;
}

pulp::view::Point to_local(pulp::view::Point pos, pulp::view::View* target, pulp::view::View* root) {
    // pulp-internal #69 — convert window-space `pos` into `target`'s
    // local-pre-scale coordinates, accounting for any ancestor's
    // set_scale transform. The forward render chain is:
    //   visual_pos = sum over ancestors A of (A.bounds * scale_chain_above_A)
    //              + (target_local * scale_chain_above_target)
    // Inverse: walk root→target, peel off each ancestor's offset
    // (scaled by the chain above it), then divide the residual by the
    // final scale_chain to get target-local coords.
    std::vector<pulp::view::View*> chain;
    for (auto* v = target; v && v != root; v = v->parent()) chain.push_back(v);
    // chain is target..root_child. Reverse to root_child..target.
    std::reverse(chain.begin(), chain.end());
    auto local = pos;
    float scale_chain = 1.0f;  // accumulated scale from root down to current ancestor
    for (auto* v : chain) {
        local.x -= v->bounds().x * scale_chain;
        local.y -= v->bounds().y * scale_chain;
        scale_chain *= v->scale();
    }
    if (scale_chain != 0.0f && scale_chain != 1.0f) {
        local.x /= scale_chain;
        local.y /= scale_chain;
    }
    return local;
}

// pulp #992 — confirm a cached View* is still attached to the live tree
// before any dereference. Walks `root` and compares pointers only, so it's
// safe to call with `needle` pointing into freed memory. Returns false if
// `needle` was unparented or destroyed between the cached capture (e.g. at
// mouseDown) and the next event (e.g. mouseUp). Does NOT detect the ABA
// case where memory was freed and reused at the same address — see #992
// follow-up issue if profiling shows that's hit; for now a tree walk
// catches the React-unmount-during-click pattern that actually crashes.
bool view_is_in_tree(pulp::view::View* needle, pulp::view::View* root) {
    if (!needle || !root) return false;
    if (needle == root) return true;
    for (size_t i = 0; i < root->child_count(); ++i) {
        if (view_is_in_tree(needle, root->child_at(i))) return true;
    }
    return false;
}

pulp::view::KeyCode key_code_from_ns(unsigned short code) {
    using KC = pulp::view::KeyCode;
    switch (code) {
        case 0: return KC::a; case 1: return KC::s; case 2: return KC::d;
        case 3: return KC::f; case 4: return KC::h; case 5: return KC::g;
        case 6: return KC::z; case 7: return KC::x; case 8: return KC::c;
        case 9: return KC::v; case 11: return KC::b; case 12: return KC::q;
        case 13: return KC::w; case 14: return KC::e; case 15: return KC::r;
        case 16: return KC::y; case 17: return KC::t; case 31: return KC::o;
        case 32: return KC::u; case 34: return KC::i; case 35: return KC::p;
        case 37: return KC::l; case 38: return KC::j; case 40: return KC::k;
        case 45: return KC::n; case 46: return KC::m;
        case 36: return KC::enter; case 53: return KC::escape;
        case 48: return KC::tab; case 51: return KC::backspace;
        case 117: return KC::delete_;
        case 123: return KC::left; case 124: return KC::right;
        case 125: return KC::down; case 126: return KC::up;
        case 115: return KC::home; case 119: return KC::end_;
        case 49: return KC::space;
        default: return KC::unknown;
    }
}

void configure_window_type(NSWindow* window, const pulp::view::WindowOptions& options) {
    if (!options.window_type) return;

    using pulp::view::WindowType;
    WindowType type = *options.window_type;

    switch (type) {
        case WindowType::palette:
            // Floating palette: stays above main windows, no taskbar entry
            [window setLevel:NSFloatingWindowLevel];
            [window setHidesOnDeactivate:YES];
            [window setCollectionBehavior:
                NSWindowCollectionBehaviorTransient |
                NSWindowCollectionBehaviorFullScreenAuxiliary];
            break;

        case WindowType::inspector:
            // Inspector: floating, resizable, auxiliary panel
            [window setLevel:NSFloatingWindowLevel];
            [window setHidesOnDeactivate:YES];
            [window setCollectionBehavior:
                NSWindowCollectionBehaviorFullScreenAuxiliary];
            break;

        case WindowType::popup:
            // Popup: above everything, auto-dismiss expected by caller
            [window setLevel:NSPopUpMenuWindowLevel];
            [window setHidesOnDeactivate:YES];
            [window setCollectionBehavior:NSWindowCollectionBehaviorTransient];
            break;

        case WindowType::dialog:
            // Dialog: modal window level, centered
            [window setLevel:NSModalPanelWindowLevel];
            [window center];
            break;

        case WindowType::main:
            // Main: default behavior, no special configuration
            break;
    }

    // Set parent-child relationship if a parent handle was provided
    if (options.parent_native_handle) {
        NSWindow* parent = (__bridge NSWindow*)options.parent_native_handle;
        [parent addChildWindow:window ordered:NSWindowAbove];
    }
}

NSRect child_view_frame_in_host(NSView* container,
                                float x,
                                float y,
                                float width,
                                float height) {
    if (!container) {
        return NSZeroRect;
    }

    const auto bounds = container.bounds;
    const CGFloat clipped_width = std::max<CGFloat>(0.0, width);
    const CGFloat clipped_height = std::max<CGFloat>(0.0, height);
    const CGFloat cocoa_y = container.isFlipped
        ? y
        : NSHeight(bounds) - y - clipped_height;
    return NSMakeRect(x, cocoa_y, clipped_width, clipped_height);
}

bool attach_child_view_to_host(NSView* container,
                               void* child_view_handle,
                               float x,
                               float y,
                               float width,
                               float height) {
    if (!container || !child_view_handle) {
        return false;
    }

    NSView* child = (__bridge NSView*) child_view_handle;
    if (!child) {
        return false;
    }

    if (child.superview && child.superview != container) {
        [child removeFromSuperview];
    }

    [child setFrame:child_view_frame_in_host(container, x, y, width, height)];

    if (child.superview != container) {
        [container addSubview:child];
    }

    [child setHidden:NO];
    return true;
}

bool set_child_view_bounds_in_host(NSView* container,
                                   void* child_view_handle,
                                   float x,
                                   float y,
                                   float width,
                                   float height) {
    if (!container || !child_view_handle) {
        return false;
    }

    NSView* child = (__bridge NSView*) child_view_handle;
    if (!child || child.superview != container) {
        return false;
    }

    [child setFrame:child_view_frame_in_host(container, x, y, width, height)];
    return true;
}

void detach_child_view_from_host(NSView* container, void* child_view_handle) {
    if (!container || !child_view_handle) {
        return;
    }

    NSView* child = (__bridge NSView*) child_view_handle;
    if (child && child.superview == container) {
        [child removeFromSuperview];
    }
}

}  // namespace pulp::view::mac_geometry

#endif  // TARGET_OS_OSX
