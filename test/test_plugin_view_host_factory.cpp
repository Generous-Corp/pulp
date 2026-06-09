// Non-Apple PluginViewHost factory registration + headless capture proof
// (#3329 Win/Linux parity). On Windows/Linux the platform host registers itself
// via register_platform_plugin_view_host(), so PluginViewHost::create() returns
// a real host without the caller installing a factory. Even with no display
// server (headless CI VM), the host degrades to a capture-only mode whose
// capture_back_buffer_png() still produces a valid frame via Skia raster.
//
// This test is built only on a non-Apple build that has a native platform host
// (PULP_TEST_HAS_PLATFORM_PLUGIN_VIEW_HOST, set from CMake). Apple has built-in
// NSView/UIView hosts and a different capture contract, so it is excluded.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/drag_drop.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#if defined(PULP_TEST_HAS_X11)
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#endif

using namespace pulp::view;

TEST_CASE("register_platform_plugin_view_host installs a factory",
          "[view][plugin-view-host][factory]") {
    register_platform_plugin_view_host();
    REQUIRE(PluginViewHost::has_factory());
}

TEST_CASE("PluginViewHost::create returns a native host with headless capture",
          "[view][plugin-view-host][create]") {
    View root;
    root.set_theme(Theme::dark());
    auto knob = std::make_unique<Knob>();
    knob->set_bounds({8, 8, 48, 48});
    knob->set_value(0.5f);
    root.add_child(std::move(knob));

    PluginViewHost::Options opts;
    opts.size = {64, 64};
    opts.use_gpu = true;  // host falls back to raster capture if GPU/display absent
    auto host = PluginViewHost::create(root, opts);
    REQUIRE(host != nullptr);

    auto size = host->get_size();
    REQUIRE(size.width == 64);
    REQUIRE(size.height == 64);

    // Headless capture must yield a valid PNG even with no display server /
    // no GPU — the deterministic frame the foreign-host embed smoke relies on.
    auto png = host->capture_back_buffer_png();
    REQUIRE_FALSE(png.empty());
    REQUIRE(png.size() > 8);
    REQUIRE(png[0] == 0x89);  // PNG magic
    REQUIRE(png[1] == 'P');
}

#if defined(PULP_TEST_HAS_X11)

// ── XDND synthetic-source handshake (Linux X11 target, #3645) ────────────────
//
// The X11 plugin host advertises an XDND target on its child window and runs the
// XdndEnter / XdndPosition / XdndDrop protocol in pump_x_events(), routing the
// resolved payload into the shared dispatch core (dispatch_drop). This test acts
// as an XDND *source* from a second X connection: it owns the XdndSelection with
// a `text/uri-list` payload, sends the protocol ClientMessages to the host's
// child window, services the host's XConvertSelection (SelectionRequest), and
// asserts the host fired dispatch_drop with the expected path AND sent back an
// XdndFinished. Requires a display server (xvfb-run); skips cleanly otherwise.

namespace {

// A DropReceiver that records the last accepted drop so the test can assert the
// host routed the XDND payload through the dispatch core.
class RecordingDropZone : public pulp::view::View, public pulp::view::DropReceiver {
public:
    bool accept_drop(const pulp::view::DropData& data, pulp::view::Point) override {
        last = data;
        drops += 1;
        return true;  // consume
    }
    pulp::view::DropData last;
    int drops = 0;
};

void send_client_message(Display* dpy, Window target, Window source, Atom type,
                         long d0, long d1, long d2, long d3, long d4) {
    XEvent ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.display = dpy;
    ev.xclient.window = target;
    ev.xclient.message_type = type;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = source ? static_cast<long>(source) : d0;
    ev.xclient.data.l[1] = d1;
    ev.xclient.data.l[2] = d2;
    ev.xclient.data.l[3] = d3;
    ev.xclient.data.l[4] = d4;
    XSendEvent(dpy, target, False, NoEventMask, &ev);
    XFlush(dpy);
}

}  // namespace

TEST_CASE("X11 plugin host runs the XDND target handshake into dispatch_drop",
          "[view][plugin-view-host][xdnd][linux]") {
    Display* src = XOpenDisplay(nullptr);
    if (!src) {
        WARN("no X display (run under xvfb-run) — XDND handshake skipped");
        return;
    }

    // Build the host with a recording drop zone covering the whole surface.
    pulp::view::View root;
    root.set_bounds({0, 0, 200, 150});
    auto zone_owned = std::make_unique<RecordingDropZone>();
    RecordingDropZone* zone = zone_owned.get();
    zone->set_bounds({0, 0, 200, 150});
    root.add_child(std::move(zone_owned));

    pulp::view::register_platform_plugin_view_host();
    pulp::view::PluginViewHost::Options opts;
    opts.size = {200, 150};
    opts.use_gpu = false;
    auto host = pulp::view::PluginViewHost::create(root, opts);
    REQUIRE(host != nullptr);

    // Parent the child onto the X11 root so it has a real on-screen origin the
    // source can target. After this, child-local == root coords (origin 0,0).
    Window x_root = DefaultRootWindow(src);
    host->attach_to_parent(reinterpret_cast<void*>(static_cast<uintptr_t>(x_root)));
    Window child = static_cast<Window>(reinterpret_cast<uintptr_t>(host->native_handle()));
    REQUIRE(child != 0);

    // Verify the host advertised XdndAware on the child window.
    Atom xa_aware = XInternAtom(src, "XdndAware", False);
    {
        Atom actual = 0; int fmt = 0; unsigned long n = 0, after = 0;
        unsigned char* prop = nullptr;
        // Poll briefly: the host sets the property on its own connection.
        int version = 0;
        for (int i = 0; i < 50 && version == 0; ++i) {
            if (XGetWindowProperty(src, child, xa_aware, 0, 1, False, XA_ATOM,
                                   &actual, &fmt, &n, &after, &prop) == Success &&
                prop && n >= 1) {
                version = static_cast<int>(reinterpret_cast<unsigned long*>(prop)[0]);
            }
            if (prop) { XFree(prop); prop = nullptr; }
            if (version == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(version >= 5);
    }

    // Source window that owns the XdndSelection and carries the uri-list.
    Window source = XCreateSimpleWindow(src, x_root, 0, 0, 1, 1, 0, 0, 0);
    Atom xa_selection = XInternAtom(src, "XdndSelection", False);
    Atom xa_uri_list = XInternAtom(src, "text/uri-list", False);
    Atom xa_enter = XInternAtom(src, "XdndEnter", False);
    Atom xa_position = XInternAtom(src, "XdndPosition", False);
    Atom xa_drop = XInternAtom(src, "XdndDrop", False);
    Atom xa_status = XInternAtom(src, "XdndStatus", False);
    Atom xa_finished = XInternAtom(src, "XdndFinished", False);
    const std::string payload = "file:///tmp/dropped%20sample.wav\r\n";

    XSetSelectionOwner(src, xa_selection, source, CurrentTime);
    REQUIRE(XGetSelectionOwner(src, xa_selection) == source);

    // XdndEnter: version 5 in the top byte of data.l[1], single inline type.
    send_client_message(src, child, source, xa_enter,
                        0, (5L << 24), static_cast<long>(xa_uri_list), 0, 0);
    // XdndPosition: pointer at root (40,30); packed (x<<16)|y in data.l[2].
    send_client_message(src, child, source, xa_position,
                        0, 0, (40L << 16) | 30L, CurrentTime,
                        static_cast<long>(XInternAtom(src, "XdndActionCopy", False)));

    // Drive the host until it replies XdndStatus (accept) for the position.
    bool got_status = false;
    auto pump_both = [&](int rounds) {
        for (int i = 0; i < rounds; ++i) {
            host->pump_x_events();
            while (XPending(src) > 0) {
                XEvent ev; XNextEvent(src, &ev);
                if (ev.type == ClientMessage && ev.xclient.message_type == xa_status &&
                    (ev.xclient.data.l[1] & 1)) {
                    got_status = true;
                } else if (ev.type == ClientMessage &&
                           ev.xclient.message_type == xa_finished) {
                    // recorded below
                } else if (ev.type == SelectionRequest) {
                    // Service the host's XConvertSelection: write the payload to
                    // the requested property, then notify.
                    XSelectionRequestEvent& rq = ev.xselectionrequest;
                    XChangeProperty(src, rq.requestor, rq.property, rq.target, 8,
                                    PropModeReplace,
                                    reinterpret_cast<const unsigned char*>(payload.data()),
                                    static_cast<int>(payload.size()));
                    XEvent note;
                    std::memset(&note, 0, sizeof(note));
                    note.xselection.type = SelectionNotify;
                    note.xselection.requestor = rq.requestor;
                    note.xselection.selection = rq.selection;
                    note.xselection.target = rq.target;
                    note.xselection.property = rq.property;
                    note.xselection.time = rq.time;
                    XSendEvent(src, rq.requestor, False, NoEventMask, &note);
                    XFlush(src);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    pump_both(40);
    CHECK(got_status);

    // XdndDrop: triggers the host's XConvertSelection. data.l[2] is the time.
    bool got_finished = false;
    send_client_message(src, child, source, xa_drop, 0, 0, CurrentTime, 0, 0);
    for (int i = 0; i < 60 && zone->drops == 0; ++i) {
        host->pump_x_events();
        while (XPending(src) > 0) {
            XEvent ev; XNextEvent(src, &ev);
            if (ev.type == SelectionRequest) {
                XSelectionRequestEvent& rq = ev.xselectionrequest;
                XChangeProperty(src, rq.requestor, rq.property, rq.target, 8,
                                PropModeReplace,
                                reinterpret_cast<const unsigned char*>(payload.data()),
                                static_cast<int>(payload.size()));
                XEvent note;
                std::memset(&note, 0, sizeof(note));
                note.xselection.type = SelectionNotify;
                note.xselection.requestor = rq.requestor;
                note.xselection.selection = rq.selection;
                note.xselection.target = rq.target;
                note.xselection.property = rq.property;
                note.xselection.time = rq.time;
                XSendEvent(src, rq.requestor, False, NoEventMask, &note);
                XFlush(src);
            } else if (ev.type == ClientMessage &&
                       ev.xclient.message_type == xa_finished) {
                got_finished = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Drain any trailing XdndFinished.
    for (int i = 0; i < 20 && !got_finished; ++i) {
        host->pump_x_events();
        while (XPending(src) > 0) {
            XEvent ev; XNextEvent(src, &ev);
            if (ev.type == ClientMessage && ev.xclient.message_type == xa_finished)
                got_finished = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(zone->drops == 1);
    REQUIRE(zone->last.type == pulp::view::DropData::Type::files);
    REQUIRE(zone->last.file_paths.size() == 1);
    CHECK(zone->last.file_paths[0] == "/tmp/dropped sample.wav");
    CHECK(got_finished);

    XDestroyWindow(src, source);
    XCloseDisplay(src);
}

#endif  // PULP_TEST_HAS_X11

