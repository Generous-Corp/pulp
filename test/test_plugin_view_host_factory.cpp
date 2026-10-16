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

#include <catch2/catch_message.hpp>
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

#if defined(_WIN32)
// WIN32_LEAN_AND_MEAN + NOMINMAX guarded before <windows.h> (SendMessageW,
// WM_SIZE, MAKELPARAM, GetClientRect).
#include <pulp/platform/win32_sane.hpp>
#include <pulp/view/frame_clock.hpp>
#include <cstdint>
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
    // The host re-runs a Yoga layout pass on every paint (attach_to_parent ->
    // repaint -> paint_scene -> layout_children), so a manually-set bounds on a
    // child does NOT survive — a default flex child with no sizing collapses to
    // zero on the main axis. Give the zone flex_grow:1 so Yoga sizes it to fill
    // the root's 200x150; otherwise the drop point hit-tests the (zero-height)
    // zone, misses it, and dispatch_drop never reaches the receiver.
    pulp::view::View root;
    root.set_bounds({0, 0, 200, 150});
    auto zone_owned = std::make_unique<RecordingDropZone>();
    RecordingDropZone* zone = zone_owned.get();
    zone->set_bounds({0, 0, 200, 150});
    zone->flex().flex_grow = 1.0f;  // fill the root's column main axis after layout
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

#if defined(_WIN32)

// ── Windows editor WM_SIZE reconcile (host-driven resize) ────────────────────
//
// A DAW resizes the editor container, which resizes our child HWND and posts
// WM_SIZE. The host must convert the physical client size to logical units,
// update get_size(), resize its surfaces, and fire the resize callback — the
// Windows analogue of the macOS -setFrameSize: -> on_native_frame_changed path.
// This test drives WM_SIZE directly via SendMessage (a synchronous, same-thread
// wndproc call — no DAW needed) and asserts the reconcile happened, was
// deduplicated, and did NOT recurse into a SetWindowPos on our own HWND.
TEST_CASE("Windows plugin host reconciles a WM_SIZE host resize",
          "[view][plugin-view-host][resize][windows]") {
    View root;
    root.set_theme(Theme::dark());
    auto knob = std::make_unique<Knob>();
    knob->set_bounds({8, 8, 48, 48});
    root.add_child(std::move(knob));

    register_platform_plugin_view_host();
    PluginViewHost::Options opts;
    opts.size = {64, 64};
    opts.use_gpu = true;  // may degrade to raster-only on a GPU-less runner
    auto host = PluginViewHost::create(root, opts);
    REQUIRE(host != nullptr);
    REQUIRE(host->get_size().width == 64);
    REQUIRE(host->get_size().height == 64);

    auto hwnd = static_cast<HWND>(host->native_handle());
    REQUIRE(hwnd != nullptr);

    // Client rect of the freshly-created (unparented) window. handle_wm_size()
    // must never SetWindowPos, so this must stay unchanged after the reconcile —
    // the proof that the reconcile path doesn't recurse through set_size().
    RECT rc_before{};
    REQUIRE(GetClientRect(hwnd, &rc_before));

    // Capture the resize callback: assert it fires once with the reconciled
    // LOGICAL size.
    uint32_t cb_w = 0, cb_h = 0;
    int cb_count = 0;
    host->set_resize_callback([&](uint32_t w, uint32_t h) {
        cb_w = w; cb_h = h; ++cb_count;
    });

    const float scale = host->scale_factor();
    REQUIRE(scale > 0.0f);

    // Target a LOGICAL size distinct from the initial 64x64, and send the
    // matching PHYSICAL client size the way a host-driven resize would. Deriving
    // physical from the live scale keeps the test correct at 100% / 125% / 150% /
    // 200% DPI runners alike.
    const uint32_t target_logical_w = 200;
    const uint32_t target_logical_h = 150;
    const auto phys_w =
        static_cast<uint32_t>(target_logical_w * scale + 0.5f);
    const auto phys_h =
        static_cast<uint32_t>(target_logical_h * scale + 0.5f);

    // SIZE_RESTORED (0) is a normal (non-minimize) resize; lParam packs width in
    // the low word and height in the high word.
    SendMessageW(hwnd, WM_SIZE, SIZE_RESTORED,
                 static_cast<LPARAM>(MAKELPARAM(phys_w, phys_h)));

    auto sz = host->get_size();
    CHECK(sz.width == target_logical_w);
    CHECK(sz.height == target_logical_h);
    CHECK(cb_count == 1);
    CHECK(cb_w == sz.width);
    CHECK(cb_h == sz.height);

    // A redundant WM_SIZE at the SAME size is a no-op (no second callback).
    SendMessageW(hwnd, WM_SIZE, SIZE_RESTORED,
                 static_cast<LPARAM>(MAKELPARAM(phys_w, phys_h)));
    CHECK(cb_count == 1);

    // A minimize (SIZE_MINIMIZED / 0x0 client) is ignored, size unchanged.
    SendMessageW(hwnd, WM_SIZE, SIZE_MINIMIZED, 0);
    CHECK(cb_count == 1);
    CHECK(host->get_size().width == target_logical_w);

    // The reconcile did NOT resize our own HWND (no SetWindowPos recursion):
    // the client rect is exactly what it was before the synthetic WM_SIZE.
    RECT rc_after{};
    REQUIRE(GetClientRect(hwnd, &rc_after));
    CHECK((rc_after.right - rc_after.left) == (rc_before.right - rc_before.left));
    CHECK((rc_after.bottom - rc_after.top) == (rc_before.bottom - rc_before.top));

    // GPU portion — soft-skip when no Dawn adapter/device is available (a
    // headless CI runner often lacks a usable D3D device). When the GPU path is
    // live, the swapchain was reconfigured through the same set_size surface
    // path; a non-null surface handle is the compile-safe assertion here (the
    // exact dims live behind pulp::render, a PRIVATE dep not on this target's
    // include path).
    if (host->is_gpu_backed()) {
        CHECK(host->gpu_surface() != nullptr);
    } else {
        WARN("no GPU adapter/device (headless CI) — GPU swapchain check skipped");
    }
}

TEST_CASE("Windows attached editor fails closed when frame timer install fails",
          "[view][plugin-view-host][repaint][windows]") {
    struct TimerFailureSeamReset {
        ~TimerFailureSeamReset() {
            SetEnvironmentVariableW(L"PULP_TEST_FORCE_WIN_FRAME_TIMER_FAILURE", nullptr);
        }
    } reset_seam;
    HWND parent =
        CreateWindowExW(0, L"STATIC", L"Pulp timer failure regression", WS_OVERLAPPEDWINDOW, 0, 0,
                        128, 128, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    REQUIRE(parent != nullptr);
    struct ParentWindowCleanup {
        HWND hwnd;
        ~ParentWindowCleanup() { DestroyWindow(hwnd); }
    } cleanup_parent{parent};
    ShowWindow(parent, SW_SHOWNOACTIVATE);
    REQUIRE(IsWindowVisible(parent));

    View root;
    root.set_bounds({0, 0, 64, 64});
    register_platform_plugin_view_host();
    PluginViewHost::Options opts;
    opts.size = {64, 64};
    opts.use_gpu = false;
    auto host = PluginViewHost::create(root, opts);
    REQUIRE(host != nullptr);
    const HWND child = static_cast<HWND>(host->native_handle());
    REQUIRE(child != nullptr);

    SetEnvironmentVariableW(L"PULP_TEST_FORCE_WIN_FRAME_TIMER_FAILURE", L"1");
    REQUIRE_FALSE(host->try_attach_to_parent(parent));
    CHECK_FALSE(host->is_attached());
    CHECK(GetParent(child) == nullptr);
    CHECK((GetWindowLongPtrW(child, GWL_STYLE) & WS_POPUP) != 0);
    CHECK_FALSE(IsWindowVisible(child));

    // Clearing the seam permits a retry. On the CPU path a timer dispatch
    // invalidates the HWND; the subsequent WM_PAINT performs the idle work.
    SetEnvironmentVariableW(L"PULP_TEST_FORCE_WIN_FRAME_TIMER_FAILURE", nullptr);
    REQUIRE(host->try_attach_to_parent(parent));
    REQUIRE(IsWindowVisible(child));
    int idle_calls = 0;
    host->set_idle_callback([&] { ++idle_calls; });
    REQUIRE(ValidateRect(child, nullptr)); // discard attach-time invalidation
    // Receive the installed timer through the real message queue. Keep the
    // fixture visible so CPU paint invalidation is observable.
    MSG frame_timer{};
    bool timer_received = false;
    const auto timer_deadline = GetTickCount64() + 2000;
    while (!timer_received && GetTickCount64() < timer_deadline) {
        if (PeekMessageW(&frame_timer, child, WM_TIMER, WM_TIMER, PM_REMOVE) &&
            frame_timer.wParam == 1) {
            timer_received = true;
            DispatchMessageW(&frame_timer);
        } else {
            MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_TIMER, MWMO_INPUTAVAILABLE);
        }
    }
    REQUIRE(timer_received);
    CHECK(idle_calls == 0);
    REQUIRE(GetUpdateRect(child, nullptr, FALSE));
    REQUIRE(UpdateWindow(child));
    CHECK(idle_calls == 1);
    CHECK_FALSE(GetUpdateRect(child, nullptr, FALSE));
    // A detached window is hidden, so its update region alone cannot prove
    // that a stale timer did no work. Arm animation and watch the frame clock.
    REQUIRE(root.frame_clock() != nullptr);
    root.set_continuous_repaint(true);
    host->detach();
    REQUIRE(ValidateRect(child, nullptr)); // discard native detach invalidation
    const auto frames_before_stale_timer = root.frame_clock()->frame();
    SendMessageW(child, WM_TIMER, 1, 0); // stale queued timer must be inert
    CHECK(root.frame_clock()->frame() == frames_before_stale_timer);
    CHECK_FALSE(GetUpdateRect(child, nullptr, FALSE));
    CHECK(idle_calls == 1);
    host->set_idle_callback({});
}

TEST_CASE("Windows attached editor requires detach before changing parent",
          "[view][plugin-view-host][repaint][windows]") {
    struct TimerFailureSeamReset {
        ~TimerFailureSeamReset() {
            SetEnvironmentVariableW(L"PULP_TEST_FORCE_WIN_FRAME_TIMER_FAILURE", nullptr);
        }
    } reset_seam;
    REQUIRE(SetEnvironmentVariableW(L"PULP_TEST_FORCE_WIN_FRAME_TIMER_FAILURE", nullptr));
    struct ParentWindow {
        HWND hwnd = CreateWindowExW(
            0, L"STATIC", L"Pulp repeated attachment", WS_OVERLAPPEDWINDOW,
            0, 0, 128, 128, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~ParentWindow() { if (hwnd) DestroyWindow(hwnd); }
    } first_parent, second_parent;
    REQUIRE(first_parent.hwnd != nullptr);
    REQUIRE(second_parent.hwnd != nullptr);
    ShowWindow(first_parent.hwnd, SW_SHOWNOACTIVATE);
    ShowWindow(second_parent.hwnd, SW_SHOWNOACTIVATE);

    View root;
    root.set_bounds({0, 0, 64, 64});
    root.set_theme(Theme::dark());
    register_platform_plugin_view_host();
    PluginViewHost::Options opts;
    opts.size = {64, 64};
    opts.use_gpu = true;
    auto host = PluginViewHost::create(root, opts);
    REQUIRE(host != nullptr);
    const HWND child = static_cast<HWND>(host->native_handle());
    REQUIRE(child != nullptr);
    REQUIRE(host->try_attach_to_parent(first_parent.hwnd));
    REQUIRE(IsWindowVisible(child));
    const auto original_style = GetWindowLongPtrW(child, GWL_STYLE);
    const auto original_surface = host->gpu_surface_status();
    const bool had_gpu = host->is_gpu_backed();
    if (had_gpu) {
        REQUIRE(original_surface.state == PluginViewHost::GpuSurfaceState::ready);
        REQUIRE(original_surface.surface != nullptr);
    } else {
        SKIP("Dawn GPU unavailable: repeated-attachment GPU lifetime checks unavailable");
    }
    int surface_notifications = 0;
    auto subscription = host->observe_gpu_surface(
        [&](const PluginViewHost::GpuSurfaceStatus&) { ++surface_notifications; });
    REQUIRE(surface_notifications == 1);
    REQUIRE(root.frame_clock() != nullptr);
    root.set_continuous_repaint(true);
    REQUIRE(ValidateRect(child, nullptr));
    MSG frame_timer{};
    while (PeekMessageW(&frame_timer, child, WM_TIMER, WM_TIMER, PM_REMOVE)) {}
    const auto frames_before_repeat = root.frame_clock()->frame();

    const auto check_preserved_attachment = [&] {
        CHECK(host->is_attached());
        CHECK(GetParent(child) == first_parent.hwnd);
        CHECK(GetWindowLongPtrW(child, GWL_STYLE) == original_style);
        CHECK(IsWindowVisible(child));
        CHECK_FALSE(GetUpdateRect(child, nullptr, FALSE));
        CHECK(root.frame_clock()->frame() == frames_before_repeat);
        CHECK(host->gpu_surface_status().state == original_surface.state);
        CHECK(host->gpu_surface_status().surface == original_surface.surface);
        CHECK(surface_notifications == 1);
    };
    for (int force_timer_failure = 0; force_timer_failure != 2; ++force_timer_failure) {
        INFO("new timer installation forced to fail: " << force_timer_failure);
        REQUIRE(SetEnvironmentVariableW(L"PULP_TEST_FORCE_WIN_FRAME_TIMER_FAILURE",
                                       force_timer_failure ? L"1" : nullptr));
        REQUIRE(host->try_attach_to_parent(first_parent.hwnd));
        check_preserved_attachment();
        host->attach_to_parent(first_parent.hwnd);
        check_preserved_attachment();
        REQUIRE_FALSE(host->try_attach_to_parent(second_parent.hwnd));
        check_preserved_attachment();
        host->attach_to_parent(second_parent.hwnd);
        check_preserved_attachment();
        REQUIRE_FALSE(host->try_attach_to_parent(nullptr));
        check_preserved_attachment();
    }

    // New timer installation is still refused. Delivery after draining the
    // queue proves that the existing attachment kept its installed timer.
    bool timer_received = false;
    const auto timer_deadline = GetTickCount64() + 2000;
    while (!timer_received && GetTickCount64() < timer_deadline) {
        if (PeekMessageW(&frame_timer, child, WM_TIMER, WM_TIMER, PM_REMOVE) &&
            frame_timer.wParam == 1) {
            timer_received = true;
            DispatchMessageW(&frame_timer);
        } else {
            MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_TIMER, MWMO_INPUTAVAILABLE);
        }
    }
    REQUIRE(timer_received);
    CHECK(root.frame_clock()->frame() == frames_before_repeat + 1);

    host->detach();
    REQUIRE_FALSE(host->is_attached());
    CHECK(GetParent(child) == nullptr);
    if (had_gpu) {
        CHECK(host->gpu_surface_state() == PluginViewHost::GpuSurfaceState::pending);
        CHECK(host->gpu_surface() == nullptr);
        CHECK(surface_notifications == 2);
    }
    // Once detached, a fresh attachment must install a new timer and may fail.
    REQUIRE_FALSE(host->try_attach_to_parent(second_parent.hwnd));
    CHECK_FALSE(host->is_attached());
    CHECK(GetParent(child) == nullptr);
    REQUIRE(SetEnvironmentVariableW(L"PULP_TEST_FORCE_WIN_FRAME_TIMER_FAILURE", nullptr));
    REQUIRE(host->try_attach_to_parent(second_parent.hwnd));
    CHECK(GetParent(child) == second_parent.hwnd);
    CHECK(IsWindowVisible(child));
    if (had_gpu) {
        CHECK(host->is_gpu_backed());
        CHECK(host->gpu_surface_state() == PluginViewHost::GpuSurfaceState::ready);
        CHECK(host->gpu_surface() != nullptr);
        CHECK(surface_notifications == 3);
    }
    host->detach();
}

// The real HWND/Dawn host must not recurse when scripted idle work requests a
// repaint during a frame. An attached repaint is consumed by WM_TIMER; the
// nested request survives for the next timer tick. This uses the production
// host and a real GPU surface, rather than a fake invalidation counter.
TEST_CASE("Windows attached GPU editor coalesces scripted repaint requests",
          "[view][plugin-view-host][repaint][windows][gpu]") {
    constexpr UINT_PTR kTestFrameTimerId = 1;
    HWND parent = CreateWindowExW(
        0, L"STATIC", L"Pulp repaint regression", WS_OVERLAPPEDWINDOW,
        0, 0, 128, 128, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    REQUIRE(parent != nullptr);

    View root;
    root.set_bounds({0, 0, 64, 64});
    root.set_theme(Theme::dark());
    register_platform_plugin_view_host();
    PluginViewHost::Options opts;
    opts.size = {64, 64};
    opts.use_gpu = true;
    auto host = PluginViewHost::create(root, opts);
    REQUIRE(host != nullptr);
    host->attach_to_parent(parent);
    REQUIRE(host->is_attached());
    if (!host->is_gpu_backed()) {
        DestroyWindow(parent);
        SKIP("Dawn GPU unavailable: repaint re-entry requires the GPU host");
    }

    const HWND child = static_cast<HWND>(host->native_handle());
    REQUIRE(child != nullptr);

    int idle_calls = 0;
    host->set_idle_callback([&] { ++idle_calls; });
    host->repaint();
    CHECK(idle_calls == 0);
    SendMessageW(child, WM_TIMER, kTestFrameTimerId, 0);
    CHECK(idle_calls == 1);

    idle_calls = 0;
    bool requested_inside_idle = false;
    host->set_idle_callback([&] {
        ++idle_calls;
        if (!requested_inside_idle) {
            requested_inside_idle = true;
            host->repaint();
        }
    });
    host->repaint();
    CHECK(idle_calls == 0);
    SendMessageW(child, WM_TIMER, kTestFrameTimerId, 0);
    CHECK(idle_calls == 1);
    CHECK(requested_inside_idle);
    SendMessageW(child, WM_TIMER, kTestFrameTimerId, 0);
    CHECK(idle_calls == 2);

    host->set_idle_callback({});
    host->detach();
    DestroyWindow(parent);
}

#endif  // _WIN32
