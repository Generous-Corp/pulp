#pragma once

// Self-contained XDND drag-SOURCE handshake for the standalone window-host
// outbound path (drag_drop_linux.cpp). Given a Display* and an XDND-aware
// `source` window, it owns the XdndSelection, grabs the pointer, drives
// XdndEnter/Position/Drop toward whatever XdndAware window is under the pointer,
// serves the `text/uri-list` payload on request, and returns true if a target
// accepted the drop. Bounded by a wall-clock deadline so an unresponsive target
// can never wedge the grabbed pointer. X11-only (compiled where Xlib links).
//
// NOTE: the X11 plugin view host (plugin_view_host_linux.cpp) carries a parallel
// member-function implementation (run_xdnd_source) bound to its own window. The
// two are intentionally kept separate for now so the standalone path can't
// regress the merged plugin-host path; a future refactor should have the host
// delegate here. Keep the two in sync if the protocol changes.

#include "xdnd_parse.hpp"  // build_uri_list

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <chrono>
#include <string>
#include <vector>

#include <unistd.h>  // usleep

namespace pulp::view::xdnd {

inline bool run_drag_source(Display* display, int screen, Window source,
                            const std::vector<std::string>& paths,
                            std::chrono::milliseconds timeout) {
    if (!display || !source || paths.empty()) return false;
    const std::string uri_list = build_uri_list(paths);
    if (uri_list.empty()) return false;

    // X11 atoms are a server-global namespace, so interning on this connection
    // yields the same ids any target uses.
    const Atom xa_XdndAware = XInternAtom(display, "XdndAware", False);
    const Atom xa_XdndEnter = XInternAtom(display, "XdndEnter", False);
    const Atom xa_XdndPosition = XInternAtom(display, "XdndPosition", False);
    const Atom xa_XdndStatus = XInternAtom(display, "XdndStatus", False);
    const Atom xa_XdndLeave = XInternAtom(display, "XdndLeave", False);
    const Atom xa_XdndDrop = XInternAtom(display, "XdndDrop", False);
    const Atom xa_XdndFinished = XInternAtom(display, "XdndFinished", False);
    const Atom xa_XdndSelection = XInternAtom(display, "XdndSelection", False);
    const Atom xa_XdndActionCopy = XInternAtom(display, "XdndActionCopy", False);
    const Atom xa_text_uri_list = XInternAtom(display, "text/uri-list", False);

    auto aware_version = [&](Window w) -> int {
        Atom type = None; int fmt = 0; unsigned long n = 0, after = 0;
        unsigned char* data = nullptr; int ver = 0;
        if (XGetWindowProperty(display, w, xa_XdndAware, 0, 1, False, AnyPropertyType,
                               &type, &fmt, &n, &after, &data) == Success) {
            if (data && type != None && fmt == 32 && n >= 1)
                ver = static_cast<int>(reinterpret_cast<long*>(data)[0]);
        }
        if (data) XFree(data);
        return ver;
    };
    // The XdndAware window under a root-space pointer: descend to the deepest
    // window, then walk up to the first XdndAware ancestor.
    auto target_at = [&](int rx, int ry, int* version_out) -> Window {
        Window root = RootWindow(display, screen);
        Window cur = root, child = root; int wx = 0, wy = 0;
        while (child != None &&
               XTranslateCoordinates(display, root, cur, rx, ry, &wx, &wy, &child)) {
            if (child == None) break;
            cur = child;
        }
        for (Window w = cur; w != None;) {
            int ver = aware_version(w);
            if (ver > 0) { if (version_out) *version_out = ver < 5 ? ver : 5; return w; }
            Window r = 0, parent = 0, *kids = nullptr; unsigned int nk = 0;
            if (!XQueryTree(display, w, &r, &parent, &kids, &nk)) break;
            if (kids) XFree(kids);
            if (w == r || parent == None) break;
            w = parent;
        }
        return None;
    };
    auto send = [&](Window target, Atom message, long d0, long d1, long d2,
                    long d3, long d4) {
        XEvent e{};
        e.xclient.type = ClientMessage;
        e.xclient.display = display;
        e.xclient.window = target;
        e.xclient.message_type = message;
        e.xclient.format = 32;
        e.xclient.data.l[0] = d0; e.xclient.data.l[1] = d1; e.xclient.data.l[2] = d2;
        e.xclient.data.l[3] = d3; e.xclient.data.l[4] = d4;
        XSendEvent(display, target, False, NoEventMask, &e);
    };
    auto serve = [&](const XSelectionRequestEvent& req) {
        XEvent note{};
        note.xselection.type = SelectionNotify;
        note.xselection.display = req.display;
        note.xselection.requestor = req.requestor;
        note.xselection.selection = req.selection;
        note.xselection.target = req.target;
        note.xselection.time = req.time;
        note.xselection.property = None;
        if (req.target == xa_text_uri_list && req.property != None) {
            XChangeProperty(display, req.requestor, req.property, req.target, 8,
                            PropModeReplace,
                            reinterpret_cast<const unsigned char*>(uri_list.data()),
                            static_cast<int>(uri_list.size()));
            note.xselection.property = req.property;
        }
        XSendEvent(display, req.requestor, False, NoEventMask, &note);
    };

    XSetSelectionOwner(display, xa_XdndSelection, source, CurrentTime);
    if (XGetSelectionOwner(display, xa_XdndSelection) != source) return false;
    if (XGrabPointer(display, source, True, ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, None,
                     CurrentTime) != GrabSuccess) {
        return false;
    }

    Window target = None;
    int target_ver = 0;
    bool accepted = false;  // last XdndStatus accept bit
    bool dropped = false;   // we have sent XdndDrop
    bool result = false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    XEvent ev;
    while (std::chrono::steady_clock::now() < deadline) {
        if (XPending(display) == 0) { usleep(2000); continue; }
        XNextEvent(display, &ev);

        if (ev.type == MotionNotify && !dropped) {
            const int rx = ev.xmotion.x_root, ry = ev.xmotion.y_root;
            int ver = 0;
            const Window t = target_at(rx, ry, &ver);
            if (t != target) {
                if (target != None)
                    send(target, xa_XdndLeave, static_cast<long>(source), 0, 0, 0, 0);
                target = t; target_ver = ver; accepted = false;
                if (target != None)
                    send(target, xa_XdndEnter, static_cast<long>(source),
                         (static_cast<long>(target_ver) << 24),
                         static_cast<long>(xa_text_uri_list), 0, 0);
            }
            if (target != None)
                send(target, xa_XdndPosition, static_cast<long>(source), 0,
                     (rx << 16) | (ry & 0xFFFF),
                     static_cast<long>(ev.xmotion.time),
                     static_cast<long>(xa_XdndActionCopy));
        } else if (ev.type == ClientMessage) {
            if (ev.xclient.message_type == xa_XdndStatus) {
                accepted = (ev.xclient.data.l[1] & 1L) != 0;
            } else if (ev.xclient.message_type == xa_XdndFinished) {
                result = dropped && (ev.xclient.data.l[1] & 1L) != 0;
                break;
            }
        } else if (ev.type == SelectionRequest) {
            if (ev.xselectionrequest.selection == xa_XdndSelection)
                serve(ev.xselectionrequest);
        } else if (ev.type == ButtonRelease && ev.xbutton.button == Button1) {
            if (target != None && accepted) {
                send(target, xa_XdndDrop, static_cast<long>(source), 0,
                     static_cast<long>(ev.xbutton.time), 0, 0);
                dropped = true;  // keep pumping for the serve + XdndFinished
            } else {
                if (target != None)
                    send(target, xa_XdndLeave, static_cast<long>(source), 0, 0, 0, 0);
                break;
            }
        }
    }

    XUngrabPointer(display, CurrentTime);
    // Relinquish XdndSelection ownership now the drag is over (mirrors the plugin
    // host's hardening — the timeout/cancel paths must not leak ownership).
    XSetSelectionOwner(display, xa_XdndSelection, None, CurrentTime);
    XFlush(display);
    return result;
}

}  // namespace pulp::view::xdnd
