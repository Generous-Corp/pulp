// GlSurface on X11 (GLX): the child window's lifecycle under a host window,
// following the host window's size, the client-thread context rule and request
// selection against a software renderer, X error containment, two-phase
// teardown, and input from synthesized X events and the X test extension.
//
// Runs under a virtual X server (xvfb-run). With no display the cases skip,
// unless PULP_TEST_REQUIRE_X11_DISPLAY is set, in which case they fail so a CI
// lane cannot pass by not testing.

#include <pulp/native/gl_surface.hpp>
#include <pulp/native/input.hpp>
#include <pulp/native/surface.hpp>
#include <pulp/platform/binary_unique_name.hpp>

#include <catch2/catch_test_macros.hpp>

#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#if defined(PULP_TEST_HAS_XTEST)
#include <X11/extensions/XTest.h>
#endif

#include <dirent.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace pulp::native;

namespace {

// Software GL, so the context a test gets does not depend on the machine's
// GPU: set before the first context is made.
const bool g_software_gl = [] {
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 0);
    return true;
}();

bool env_set(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0';
}

// The test's own connection to the server: the "host" side. Skips the test
// when there is no display, or fails it when one is required.
struct HostConnection {
    Display* display = nullptr;
    HostConnection() {
        display = XOpenDisplay(nullptr);
        if (display == nullptr) {
            if (env_set("PULP_TEST_REQUIRE_X11_DISPLAY"))
                FAIL("no X display, and PULP_TEST_REQUIRE_X11_DISPLAY is set");
            SKIP("no X display (run under xvfb-run)");
        }
    }
    ~HostConnection() {
        if (display != nullptr)
            XCloseDisplay(display);
    }
    HostConnection(const HostConnection&) = delete;
    HostConnection& operator=(const HostConnection&) = delete;
};

// A top-level window standing in for the host's editor window.
struct HostWindow {
    Display* display;
    Window window = 0;
    explicit HostWindow(Display* d, unsigned w = 320, unsigned h = 240) : display(d) {
        window = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, w, h, 0, 0, 0);
        XSelectInput(d, window,
                     StructureNotifyMask | KeyPressMask | KeyReleaseMask | ButtonPressMask);
        XMapWindow(d, window);
        XSync(d, False);
    }
    ~HostWindow() {
        if (window != 0)
            XDestroyWindow(display, window);
        XSync(display, False);
    }
    void* handle() const {
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(window));
    }
    unsigned children() const {
        Window root = 0;
        Window parent_of = 0;
        Window* list = nullptr;
        unsigned count = 0;
        XSync(display, False);
        if (XQueryTree(display, window, &root, &parent_of, &list, &count) == 0)
            return 0;
        if (list != nullptr)
            XFree(list);
        return count;
    }
    // The size of window `w` as the server has it, or false when it is gone.
    static bool size_of(Display* d, Window w, unsigned& width, unsigned& height) {
        XWindowAttributes attributes{};
        XSync(d, False);
        // Errors for a vanished window must not end the process.
        XErrorHandler previous = XSetErrorHandler([](Display*, XErrorEvent*) { return 0; });
        const Status ok = XGetWindowAttributes(d, w, &attributes);
        XSync(d, False);
        XSetErrorHandler(previous);
        width = static_cast<unsigned>(attributes.width);
        height = static_cast<unsigned>(attributes.height);
        return ok != 0;
    }
};

// The list every machine can satisfy somewhere.
GlSurfaceOptions any_driver() {
    GlSurfaceOptions options;
    options.requests = {
        {3, 3, GlProfile::core, 0}, {2, 1, GlProfile::any, 0}, {1, 1, GlProfile::any, 0}};
    return options;
}

// Run `fn` on a thread of its own, as a client's render thread would.
void on_render_thread(const std::function<void()>& fn) {
    std::thread t(fn);
    t.join();
}

// Whether a context can be made at all on this machine: GLX on the virtual
// server, with a software driver behind it. Skips the case when it cannot,
// unless the lane requires a display (a virtual server with a software driver
// always has GLX).
void require_glx(GlSurface& surface) {
    bool made = false;
    on_render_thread([&] {
        made = surface.context().make_current();
        if (made)
            surface.context().release_current();
    });
    if (!made) {
        if (env_set("PULP_TEST_REQUIRE_X11_DISPLAY"))
            FAIL("no GLX context, and PULP_TEST_REQUIRE_X11_DISPLAY is set");
        SKIP("no GLX context on this X server");
    }
}

// Dispatch until `done` or the deadline. The server round trips in
// `host.display` flush the test's own requests first.
bool pump(GlSurface& surface, Display* host, const std::function<bool()>& done,
          int timeout_ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    do {
        XSync(host, False);
        surface.dispatch_events();
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return done();
}

std::size_t open_descriptors() {
    std::size_t count = 0;
    if (DIR* dir = opendir("/proc/self/fd")) {
        while (readdir(dir) != nullptr)
            ++count;
        closedir(dir);
    }
    return count;
}

// A synthesized event for the child window, delivered to the client that made
// it (the surface), as the server would deliver a real one.
void send_to_client(Display* host, Window target, XEvent& event) {
    XSendEvent(host, target, False, 0, &event);
    XSync(host, False);
}

XEvent button_event(Display* host, Window target, int type, unsigned button, int x, int y,
                    Time time, unsigned state = 0) {
    XEvent e{};
    e.xbutton.type = type;
    e.xbutton.display = host;
    e.xbutton.window = target;
    e.xbutton.root = DefaultRootWindow(host);
    e.xbutton.time = time;
    e.xbutton.x = x;
    e.xbutton.y = y;
    e.xbutton.state = state;
    e.xbutton.button = button;
    e.xbutton.same_screen = True;
    return e;
}

XEvent key_event(Display* host, Window target, int type, KeySym symbol, unsigned state = 0,
                 Time time = 1) {
    XEvent e{};
    e.xkey.type = type;
    e.xkey.display = host;
    e.xkey.window = target;
    e.xkey.root = DefaultRootWindow(host);
    e.xkey.time = time;
    e.xkey.state = state;
    e.xkey.keycode = XKeysymToKeycode(host, symbol);
    e.xkey.same_screen = True;
    return e;
}

Window child_of(const GlSurface& surface) {
    return static_cast<Window>(reinterpret_cast<std::uintptr_t>(surface.native_window()));
}

} // namespace

TEST_CASE("the window class name is unique to this binary", "[native][gl-surface][x11]") {
    const std::string name = GlSurface::window_class_name();
    REQUIRE(name == pulp::platform::binary_unique_name("PulpGlSurface"));
    REQUIRE(name != "PulpGlSurface");
    REQUIRE(name.size() > std::string("PulpGlSurface_").size());
}

TEST_CASE("attach makes a child that fills the host window and detach removes it",
          "[native][gl-surface][x11]") {
    HostConnection conn;
    HostWindow host(conn.display);
    auto surface = GlSurface::create(any_driver());
    REQUIRE(surface != nullptr);
    REQUIRE_FALSE(surface->offscreen());
    REQUIRE_FALSE(surface->attached());
    REQUIRE(surface->native_window() == nullptr);
    REQUIRE(surface->event_fd() == -1);

    REQUIRE(surface->attach(host.handle()));
    REQUIRE(surface->attached());
    REQUIRE(surface->event_fd() >= 0);
    REQUIRE(host.children() == 1);
    unsigned w = 0;
    unsigned h = 0;
    REQUIRE(HostWindow::size_of(conn.display, child_of(*surface), w, h));
    REQUIRE(w == 320);
    REQUIRE(h == 240);
    const SurfaceGeometry geometry = surface->geometry();
    REQUIRE(geometry.drawable_width == 320);
    REQUIRE(geometry.drawable_height == 240);
    REQUIRE(geometry.in_window);
    REQUIRE(geometry.scale > 0.0);

    // A second attach is refused while attached.
    REQUIRE_FALSE(surface->attach(host.handle()));
    REQUIRE(host.children() == 1);

    const Window child = child_of(*surface);
    surface->detach();
    REQUIRE_FALSE(surface->attached());
    REQUIRE(surface->native_window() == nullptr);
    REQUIRE(surface->event_fd() == -1);
    REQUIRE(host.children() == 0);
    REQUIRE_FALSE(HostWindow::size_of(conn.display, child, w, h));
    const SurfaceGeometry detached = surface->geometry();
    REQUIRE_FALSE(detached.in_window);
    REQUIRE(detached.width == 0.0);
    REQUIRE(detached.height == 0.0);
    REQUIRE(detached.scale == 0.0);
    REQUIRE(detached.drawable_width == 0);
    REQUIRE(detached.drawable_height == 0);
}

TEST_CASE("mapping and unmapping updates the on-screen state", "[native][gl-surface][x11]") {
    HostConnection conn;
    HostWindow host(conn.display);
    auto surface = GlSurface::create(any_driver());
    REQUIRE(surface->attach(host.handle()));
    std::vector<SurfaceEvent> events;
    surface->set_listener([&](const SurfaceEvent& e) { events.push_back(e); });

    XUnmapWindow(conn.display, host.window);
    XSync(conn.display, False);
    REQUIRE(pump(*surface, conn.display, [&] {
        return !events.empty() && events.back().kind == SurfaceEventKind::visibility_changed &&
               !events.back().visibility.window_visible;
    }));
    REQUIRE(events.back().visibility.minimized);

    XMapWindow(conn.display, host.window);
    XSync(conn.display, False);
    REQUIRE(pump(*surface, conn.display, [&] {
        return events.back().kind == SurfaceEventKind::visibility_changed &&
               events.back().visibility.window_visible;
    }));
    REQUIRE(events.back().visibility.in_window);
}

TEST_CASE("a parent that does not exist is refused without ending the process",
          "[native][gl-surface][x11]") {
    HostConnection conn;
    auto surface = GlSurface::create(any_driver());
    // An id no client ever made: the server answers BadWindow, which Xlib's
    // default handler would turn into exit().
    REQUIRE_FALSE(surface->attach(reinterpret_cast<void*>(0x7ffffff0)));
    REQUIRE_FALSE(surface->attached());
    REQUIRE_FALSE(surface->attach(nullptr));

    HostWindow host(conn.display);
    REQUIRE(surface->attach(host.handle()));
    surface->detach();
}

TEST_CASE("a hundred attach and detach cycles leave no windows and no connections behind",
          "[native][gl-surface][x11]") {
    HostConnection conn;
    HostWindow host(conn.display);
    {
        // Warm up lazily opened descriptors (libGL, the display) first.
        auto warm = GlSurface::create(any_driver());
        REQUIRE(warm->attach(host.handle()));
        warm->detach();
    }
    const std::size_t before = open_descriptors();
    for (int i = 0; i < 100; ++i) {
        auto surface = GlSurface::create(any_driver());
        REQUIRE(surface->attach(host.handle()));
        surface->detach();
    }
    REQUIRE(host.children() == 0);
    REQUIRE(open_descriptors() == before);
}

TEST_CASE("creating and tearing down contexts leaks no connection or window",
          "[native][gl-surface][x11]") {
    HostConnection conn;
    HostWindow host(conn.display);
    {
        auto warm = GlSurface::create(any_driver());
        REQUIRE(warm->attach(host.handle()));
        require_glx(*warm);
        warm->detach();
    }
    const std::size_t before = open_descriptors();
    for (int i = 0; i < 20; ++i) {
        auto surface = GlSurface::create(any_driver());
        REQUIRE(surface->attach(host.handle()));
        on_render_thread([&] {
            GlContext& gl = surface->context();
            if (gl.make_current()) {
                gl.swap_buffers();
                gl.release_current();
            }
        });
        // Alternate the two ways a surface ends.
        if (i % 2 == 0)
            surface->detach();
    }
    REQUIRE(host.children() == 0);
    REQUIRE(open_descriptors() == before);
}

TEST_CASE("the child follows the host window's size", "[native][gl-surface][x11]") {
    HostConnection conn;
    HostWindow host(conn.display);
    auto surface = GlSurface::create(any_driver());
    REQUIRE(surface->attach(host.handle()));
    std::vector<SurfaceEvent> events;
    surface->set_listener([&](const SurfaceEvent& e) { events.push_back(e); });
    const std::uint64_t first_generation = surface->geometry().generation;

    // Nothing follows until something pumps.
    XResizeWindow(conn.display, host.window, 480, 360);
    XSync(conn.display, False);
    unsigned w = 0;
    unsigned h = 0;
    HostWindow::size_of(conn.display, child_of(*surface), w, h);
    REQUIRE(w == 320);

    REQUIRE(
        pump(*surface, conn.display, [&] { return surface->geometry().drawable_width == 480; }));
    REQUIRE(surface->geometry().drawable_height == 360);
    REQUIRE(surface->geometry().generation > first_generation);
    REQUIRE(pump(*surface, conn.display, [&] {
        return HostWindow::size_of(conn.display, child_of(*surface), w, h) && w == 480 && h == 360;
    }));
    REQUIRE_FALSE(events.empty());
    REQUIRE(events.front().kind == SurfaceEventKind::geometry_changed);
    REQUIRE(events.back().geometry.drawable_width == 480);

    // The size following the child itself does not echo as another change.
    const std::size_t settled = events.size();
    surface->dispatch_events();
    REQUIRE(events.size() == settled);

    // Shrinking follows too.
    XResizeWindow(conn.display, host.window, 200, 100);
    REQUIRE(
        pump(*surface, conn.display, [&] { return surface->geometry().drawable_width == 200; }));
    REQUIRE(surface->geometry().drawable_height == 100);
}

TEST_CASE("the scale is detected, passed by the host and replaced later",
          "[native][gl-surface][x11]") {
    HostConnection conn;
    HostWindow host(conn.display);

    GlSurfaceOptions options = any_driver();
    options.scale = 2.0;
    auto surface = GlSurface::create(options);
    REQUIRE(surface->attach(host.handle()));
    REQUIRE(surface->geometry().scale == 2.0);
    REQUIRE(surface->geometry().width == 160.0);
    REQUIRE(surface->geometry().height == 120.0);
    REQUIRE(surface->geometry().drawable_width == 320);

    std::vector<SurfaceEvent> events;
    surface->set_listener([&](const SurfaceEvent& e) { events.push_back(e); });
    surface->set_scale(4.0);
    REQUIRE(surface->geometry().scale == 4.0);
    REQUIRE(surface->geometry().width == 80.0);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].kind == SurfaceEventKind::geometry_changed);
    // Back to the options' value.
    surface->set_scale(0.0);
    REQUIRE(surface->geometry().scale == 2.0);

    // With nothing passed, the scale is detected from the server and is
    // positive.
    auto detected = GlSurface::create(any_driver());
    REQUIRE(detected->attach(host.handle()));
    REQUIRE(detected->geometry().scale > 0.0);
    REQUIRE(detected->geometry().drawable_width == 320);
}

TEST_CASE("the context is made on the client thread and honours a request",
          "[native][gl-surface][x11]") {
    HostConnection conn;
    HostWindow host(conn.display);
    auto surface = GlSurface::create(any_driver());
    REQUIRE(surface->attach(host.handle()));
    require_glx(*surface);

    bool made = false;
    bool swapped = false;
    GLXDrawable drawable = 0;
    unsigned drawable_config = 0;
    int context_config = 0;
    int query_status = -1;
    GlContextReport report;
    std::array<unsigned char, 4> pixel{0, 0, 0, 0};
    on_render_thread([&] {
        GlContext& gl = surface->context();
        made = gl.make_current();
        if (!made)
            return;
        drawable = glXGetCurrentDrawable();
        Display* render_display = glXGetCurrentDisplay();
        glXQueryDrawable(render_display, drawable, GLX_FBCONFIG_ID, &drawable_config);
        query_status = glXQueryContext(render_display, glXGetCurrentContext(), GLX_FBCONFIG_ID,
                                      &context_config);
        report = gl.report();
        glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        swapped = gl.swap_buffers();
        gl.release_current();
    });
    INFO("GL_VERSION " << report.version << ", GL_RENDERER " << report.renderer);
    REQUIRE(made);
    REQUIRE(swapped);
    // A GLXWindow is a separate server resource, not the UI thread's raw X
    // child. Its FBConfig must match the context used for rendering/readback.
    REQUIRE(drawable != 0);
    REQUIRE(drawable != child_of(*surface));
    REQUIRE(query_status == Success);
    REQUIRE(drawable_config != 0);
    REQUIRE(drawable_config == static_cast<unsigned>(context_config));
    REQUIRE(first_honoured_request(any_driver().requests, report.major, report.minor,
                                   report.profile) == report.request_index);
    REQUIRE(report.max_texture_size > 0);
    REQUIRE(pixel[0] >= 49);
    REQUIRE(pixel[0] <= 53);
    REQUIRE(pixel[1] >= 100);
    REQUIRE(pixel[1] <= 104);
    REQUIRE(pixel[2] >= 151);
    REQUIRE(pixel[2] <= 155);
    REQUIRE(surface->context().framebuffer() == 0);
    // The renderer string names a software rasterizer exactly when the report
    // says so; with software GL forced it must.
    REQUIRE(report.software == is_software_renderer(report.renderer));
    if (env_set("LIBGL_ALWAYS_SOFTWARE"))
        REQUIRE(report.software);
    surface->detach();
}

TEST_CASE("a request the server cannot honour falls through to the next",
          "[native][gl-surface][x11]") {
    HostConnection conn;
    HostWindow host(conn.display);

    GlSurfaceOptions options;
    options.requests = {{9, 9, GlProfile::core, 0}, {2, 1, GlProfile::any, 0}};
    auto surface = GlSurface::create(options);
    REQUIRE(surface->attach(host.handle()));
    require_glx(*surface);
    std::size_t index = 99;
    on_render_thread([&] {
        GlContext& gl = surface->context();
        if (gl.make_current()) {
            index = gl.report().request_index;
            gl.release_current();
        }
    });
    REQUIRE(index == 1u);
    surface->detach();

    // No request at all can be honoured: make_current answers false, and the
    // process is still alive to say so.
    GlSurfaceOptions impossible;
    impossible.requests = {{9, 9, GlProfile::core, 0}, {8, 0, GlProfile::compatibility, 0}};
    auto refused = GlSurface::create(impossible);
    REQUIRE(refused->attach(host.handle()));
    bool made = true;
    on_render_thread([&] { made = refused->context().make_current(); });
    REQUIRE_FALSE(made);
    refused->detach();
}

TEST_CASE("a multisampled request is refused rather than given a single-sampled context",
          "[native][gl-surface][x11]") {
    HostConnection conn;
    HostWindow host(conn.display);

    GlSurfaceOptions fallback;
    fallback.requests = {{1, 1, GlProfile::any, 4}, {1, 1, GlProfile::any, 0}};
    auto surface = GlSurface::create(fallback);
    REQUIRE(surface->attach(host.handle()));
    require_glx(*surface);
    std::size_t index = 99;
    on_render_thread([&] {
        GlContext& gl = surface->context();
        if (gl.make_current()) {
            index = gl.report().request_index;
            gl.release_current();
        }
    });
    REQUIRE(index == 1u);
    surface->detach();

    GlSurfaceOptions sampled_only;
    sampled_only.requests = {{1, 1, GlProfile::any, 4}};
    auto refused = GlSurface::create(sampled_only);
    REQUIRE(refused->attach(host.handle()));
    bool made = true;
    on_render_thread([&] { made = refused->context().make_current(); });
    REQUIRE_FALSE(made);
    refused->detach();
}

TEST_CASE("detach runs the release handler while the window and context still exist",
          "[native][gl-surface][x11]") {
    HostConnection conn;
    HostWindow host(conn.display);
    auto surface = GlSurface::create(any_driver());
    REQUIRE(surface->attach(host.handle()));
    require_glx(*surface);

    // The client's render thread holds the context until the handler joins it.
    std::atomic<bool> stop{false};
    std::atomic<bool> have_context{false};
    std::thread render([&] {
        GlContext& gl = surface->context();
        if (!gl.make_current())
            return;
        have_context = true;
        while (!stop)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        gl.release_current();
    });
    while (!have_context)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    const Window child = child_of(*surface);
    int releases = 0;
    bool window_alive_in_handler = false;
    bool refused_in_handler = false;
    GlSurface* s = surface.get();
    surface->set_release_handler([&] {
        ++releases;
        unsigned w = 0;
        unsigned h = 0;
        window_alive_in_handler = HostWindow::size_of(conn.display, child, w, h);
        // Teardown has stopped new work: the context refuses and does not swap.
        bool swapped = true;
        std::thread probe([&] {
            refused_in_handler = !s->context().make_current();
            swapped = s->context().swap_buffers();
        });
        probe.join();
        refused_in_handler = refused_in_handler && !swapped;
        stop = true;
        render.join();
    });
    surface->detach();
    REQUIRE(releases == 1);
    REQUIRE(window_alive_in_handler);
    REQUIRE(refused_in_handler);
    REQUIRE(host.children() == 0);

    // After teardown the context is gone for good.
    bool made = true;
    on_render_thread([&] { made = surface->context().make_current(); });
    REQUIRE_FALSE(made);
    bool swapped = true;
    on_render_thread([&] { swapped = surface->context().swap_buffers(); });
    REQUIRE_FALSE(swapped);
}

TEST_CASE("destroying an attached surface runs the release handler once and frees the window",
          "[native][gl-surface][x11]") {
    HostConnection conn;
    HostWindow host(conn.display);
    int releases = 0;
    {
        auto surface = GlSurface::create(any_driver());
        REQUIRE(surface->attach(host.handle()));
        require_glx(*surface);
        on_render_thread([&] {
            if (surface->context().make_current())
                surface->context().release_current();
        });
        surface->set_release_handler([&] { ++releases; });
    }
    REQUIRE(releases == 1);
    REQUIRE(host.children() == 0);
}

TEST_CASE("invalidate after the host window is gone touches nothing", "[native][gl-surface][x11]") {
    HostConnection conn;
    auto host = std::make_unique<HostWindow>(conn.display);
    auto surface = GlSurface::create(any_driver());
    REQUIRE(surface->attach(host->handle()));
    std::vector<SurfaceEventKind> kinds;
    surface->set_listener([&](const SurfaceEvent& e) { kinds.push_back(e.kind); });
    int releases = 0;
    surface->set_release_handler([&] { ++releases; });

    // The host destroys its window first; the surface hears of it when it next
    // pumps, and reports it left the window.
    host.reset();
    REQUIRE(pump(*surface, conn.display, [&] { return !surface->geometry().in_window; }));
    REQUIRE_FALSE(kinds.empty());
    REQUIRE(kinds.back() == SurfaceEventKind::window_changed);
    const SurfaceGeometry gone = surface->geometry();
    REQUIRE(gone.width == 0.0);
    REQUIRE(gone.height == 0.0);
    REQUIRE(gone.scale == 0.0);
    REQUIRE(gone.drawable_width == 0);
    REQUIRE(gone.drawable_height == 0);

    surface->invalidate();
    REQUIRE(releases == 1);
    REQUIRE_FALSE(surface->attached());
    surface->invalidate(); // idempotent
    REQUIRE(releases == 1);
    // After invalidate nothing can attach again.
    HostWindow other(conn.display);
    REQUIRE_FALSE(surface->attach(other.handle()));
}

TEST_CASE("detach after the host window is gone does not end the process",
          "[native][gl-surface][x11]") {
    HostConnection conn;
    auto host = std::make_unique<HostWindow>(conn.display);
    auto surface = GlSurface::create(any_driver());
    REQUIRE(surface->attach(host->handle()));
    require_glx(*surface);
    on_render_thread([&] {
        GlContext& gl = surface->context();
        if (gl.make_current()) {
            gl.swap_buffers();
            gl.release_current();
        }
    });
    host.reset();
    XSync(conn.display, False);
    // Both the window the surface made and the one it hangs from are gone: the
    // requests it now sends name vanished windows, and the server answers each
    // with an error.
    surface->detach();
    REQUIRE_FALSE(surface->attached());
}

TEST_CASE("pointer events from the child window reach the handler", "[native][gl-surface][x11]") {
    HostConnection conn;
    HostWindow host(conn.display);
    GlSurfaceOptions options = any_driver();
    options.scale = 2.0;
    auto surface = GlSurface::create(options);
    REQUIRE(surface->attach(host.handle()));
    const Window child = child_of(*surface);

    std::vector<PointerEvent> pointer;
    std::vector<WheelEvent> wheel;
    InputHandler handler;
    handler.pointer = [&](const PointerEvent& e) { pointer.push_back(e); };
    handler.wheel = [&](const WheelEvent& e) { wheel.push_back(e); };
    surface->set_input_handler(handler);

    XEvent press = button_event(conn.display, child, ButtonPress, 1, 40, 60, 1000, ShiftMask);
    send_to_client(conn.display, child, press);
    XEvent release =
        button_event(conn.display, child, ButtonRelease, 1, 40, 60, 1050, ShiftMask | Button1Mask);
    send_to_client(conn.display, child, release);
    XEvent again = button_event(conn.display, child, ButtonPress, 1, 42, 61, 1200);
    send_to_client(conn.display, child, again);
    XEvent right = button_event(conn.display, child, ButtonPress, 3, 10, 10, 5000);
    send_to_client(conn.display, child, right);
    XEvent middle = button_event(conn.display, child, ButtonPress, 2, 10, 10, 9000);
    send_to_client(conn.display, child, middle);
    REQUIRE(pump(*surface, conn.display, [&] { return pointer.size() >= 5; }));

    // Positions are logical points: pixels over the scale of 2.
    REQUIRE(pointer[0].kind == PointerKind::down);
    REQUIRE(pointer[0].x == 20.0);
    REQUIRE(pointer[0].y == 30.0);
    REQUIRE(pointer[0].button == PointerButton::left);
    REQUIRE(pointer[0].button_number == 0);
    REQUIRE((pointer[0].modifiers & modifier_shift) != 0);
    REQUIRE(pointer[0].buttons_held == 1u);
    REQUIRE(pointer[0].click_count == 1);
    REQUIRE(pointer[0].timestamp == 1.0);
    REQUIRE(pointer[1].kind == PointerKind::up);
    REQUIRE(pointer[1].buttons_held == 0u);
    // A second press close in time and place is a double click; a far one is
    // a new click.
    REQUIRE(pointer[2].kind == PointerKind::down);
    REQUIRE(pointer[2].click_count == 2);
    REQUIRE(pointer[3].button == PointerButton::right);
    REQUIRE(pointer[3].button_number == 1);
    REQUIRE(pointer[3].click_count == 1);
    REQUIRE(pointer[4].button == PointerButton::middle);
    REQUIRE(pointer[4].button_number == 2);

    // Motion with and without a button.
    pointer.clear();
    XEvent move{};
    move.xmotion.type = MotionNotify;
    move.xmotion.display = conn.display;
    move.xmotion.window = child;
    move.xmotion.x = 100;
    move.xmotion.y = 50;
    move.xmotion.time = 2;
    send_to_client(conn.display, child, move);
    move.xmotion.state = Button1Mask;
    send_to_client(conn.display, child, move);
    REQUIRE(pump(*surface, conn.display, [&] { return pointer.size() >= 2; }));
    REQUIRE(pointer[0].kind == PointerKind::move);
    REQUIRE(pointer[0].x == 50.0);
    REQUIRE(pointer[1].kind == PointerKind::drag);
    REQUIRE(pointer[1].button == PointerButton::left);

    // Wheel buttons are wheel events, not clicks.
    pointer.clear();
    XEvent up = button_event(conn.display, child, ButtonPress, 4, 20, 20, 3);
    send_to_client(conn.display, child, up);
    XEvent down = button_event(conn.display, child, ButtonPress, 5, 20, 20, 4);
    send_to_client(conn.display, child, down);
    XEvent right_tick = button_event(conn.display, child, ButtonPress, 7, 20, 20, 5);
    send_to_client(conn.display, child, right_tick);
    XEvent wheel_release = button_event(conn.display, child, ButtonRelease, 4, 20, 20, 6);
    send_to_client(conn.display, child, wheel_release);
    REQUIRE(pump(*surface, conn.display, [&] { return wheel.size() >= 3; }));
    surface->dispatch_events();
    REQUIRE(pointer.empty());
    REQUIRE(wheel.size() == 3);
    REQUIRE(wheel[0].dy > 0.0);
    REQUIRE(wheel[0].dx == 0.0);
    REQUIRE_FALSE(wheel[0].precise);
    REQUIRE(wheel[0].x == 10.0);
    REQUIRE(wheel[1].dy < 0.0);
    REQUIRE(wheel[2].dx < 0.0);
}

TEST_CASE("a wheel event with no wheel handler goes on to the host window",
          "[native][gl-surface][x11]") {
    HostConnection conn;
    HostWindow host(conn.display);
    auto surface = GlSurface::create(any_driver());
    REQUIRE(surface->attach(host.handle()));
    const Window child = child_of(*surface);
    InputHandler handler;
    handler.pointer = [](const PointerEvent&) {};
    surface->set_input_handler(handler);

    XEvent tick = button_event(conn.display, child, ButtonPress, 4, 5, 5, 10);
    send_to_client(conn.display, child, tick);
    XEvent seen{};
    bool forwarded = false;
    REQUIRE(pump(*surface, conn.display, [&] {
        forwarded = XCheckTypedWindowEvent(conn.display, host.window, ButtonPress, &seen) != 0;
        return forwarded;
    }));
    REQUIRE(seen.xbutton.button == 4);
}

TEST_CASE("keys go to the host unless the client holds the keyboard", "[native][gl-surface][x11]") {
    HostConnection conn;
    HostWindow host(conn.display);
    auto surface = GlSurface::create(any_driver());
    REQUIRE(surface->attach(host.handle()));
    const Window child = child_of(*surface);

    std::vector<KeyEvent> keys;
    std::vector<std::string> texts;
    bool consume = true;
    InputHandler handler;
    handler.key = [&](const KeyEvent& e) {
        keys.push_back(e);
        return consume;
    };
    handler.text = [&](const std::string& t) { texts.push_back(t); };
    surface->set_input_handler(handler);

    auto host_keys = [&] {
        int count = 0;
        XEvent e;
        while (XCheckTypedWindowEvent(conn.display, host.window, KeyPress, &e))
            ++count;
        return count;
    };
    auto settle = [&] {
        for (int i = 0; i < 20; ++i) {
            XSync(conn.display, False);
            surface->dispatch_events();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };

    // No keyboard focus: the surface offers nothing to the client; a key that
    // reaches it anyway is the host's.
    XEvent space = key_event(conn.display, child, KeyPress, XK_space);
    send_to_client(conn.display, child, space);
    settle();
    REQUIRE(keys.empty());
    REQUIRE(host_keys() == 1);
    XEvent space_up = key_event(conn.display, child, KeyRelease, XK_space);
    send_to_client(conn.display, child, space_up);
    settle();

    // Keys held: the client is offered every key first and a consumed key does
    // not reach the host.
    surface->set_keyboard_focus(KeyboardFocus::keys);
    XEvent a_down = key_event(conn.display, child, KeyPress, XK_a);
    send_to_client(conn.display, child, a_down);
    XEvent a_up = key_event(conn.display, child, KeyRelease, XK_a);
    send_to_client(conn.display, child, a_up);
    settle();
    REQUIRE(keys.size() == 2);
    REQUIRE(keys[0].kind == KeyKind::down);
    REQUIRE(keys[0].key == Key::a);
    REQUIRE(keys[0].characters == "a");
    REQUIRE(keys[0].native_code == a_down.xkey.keycode);
    REQUIRE_FALSE(keys[0].repeat);
    REQUIRE(keys[1].kind == KeyKind::up);
    REQUIRE(host_keys() == 0);

    // A declined key goes on to the host (Space for the transport).
    keys.clear();
    consume = false;
    XEvent space2 = key_event(conn.display, child, KeyPress, XK_space);
    send_to_client(conn.display, child, space2);
    settle();
    REQUIRE(keys.size() == 1);
    REQUIRE(keys[0].key == Key::space);
    REQUIRE(host_keys() == 1);
    XEvent space2_up = key_event(conn.display, child, KeyRelease, XK_space);
    send_to_client(conn.display, child, space2_up);
    settle();

    // Text entry: a declined key becomes text, with the shifted character, and
    // does not reach the host.
    keys.clear();
    surface->set_keyboard_focus(KeyboardFocus::text);
    XEvent shifted = key_event(conn.display, child, KeyPress, XK_a, ShiftMask);
    send_to_client(conn.display, child, shifted);
    XEvent shifted_up = key_event(conn.display, child, KeyRelease, XK_a, ShiftMask);
    send_to_client(conn.display, child, shifted_up);
    settle();
    REQUIRE(texts.size() == 1);
    REQUIRE(texts[0] == "A");
    REQUIRE((keys[0].modifiers & modifier_shift) != 0);
    REQUIRE(keys[0].key == Key::a); // identity is the key, not the character
    REQUIRE(host_keys() == 0);

    // A held key repeats: each repeat is the same key, marked as a repeat.
    keys.clear();
    consume = true;
    XEvent hold = key_event(conn.display, child, KeyPress, XK_b);
    send_to_client(conn.display, child, hold);
    send_to_client(conn.display, child, hold);
    settle();
    REQUIRE(keys.size() == 2);
    REQUIRE_FALSE(keys[0].repeat);
    REQUIRE(keys[1].repeat);
    // The release goes where the press went, even though focus moved.
    surface->set_keyboard_focus(KeyboardFocus::none);
    keys.clear();
    XEvent hold_up = key_event(conn.display, child, KeyRelease, XK_b);
    send_to_client(conn.display, child, hold_up);
    settle();
    REQUIRE(keys.size() == 1);
    REQUIRE(keys[0].kind == KeyKind::up);
}

TEST_CASE("focus changes reach the handler", "[native][gl-surface][x11]") {
    HostConnection conn;
    HostWindow host(conn.display);
    auto surface = GlSurface::create(any_driver());
    REQUIRE(surface->attach(host.handle()));
    std::vector<bool> focus;
    InputHandler handler;
    handler.focus = [&](const FocusEvent& e) { focus.push_back(e.focused); };
    surface->set_input_handler(handler);

    // Holding the keyboard takes X input focus; giving it back returns it.
    surface->set_keyboard_focus(KeyboardFocus::keys);
    REQUIRE(pump(*surface, conn.display, [&] { return !focus.empty(); }));
    REQUIRE(focus.back());
    Window holder = 0;
    int revert = 0;
    XGetInputFocus(conn.display, &holder, &revert);
    REQUIRE(holder == child_of(*surface));
    surface->set_keyboard_focus(KeyboardFocus::none);
    REQUIRE(pump(*surface, conn.display, [&] { return focus.size() >= 2; }));
    REQUIRE_FALSE(focus.back());
    XGetInputFocus(conn.display, &holder, &revert);
    REQUIRE(holder != child_of(*surface));
}

#if defined(PULP_TEST_HAS_XTEST)
TEST_CASE("a drag that leaves the window is still delivered to it", "[native][gl-surface][x11]") {
    HostConnection conn;
    int event_base = 0;
    int error_base = 0;
    int major = 0;
    int minor = 0;
    if (!XTestQueryExtension(conn.display, &event_base, &error_base, &major, &minor)) {
        if (env_set("PULP_TEST_REQUIRE_X11_DISPLAY"))
            FAIL("the X test extension is missing");
        SKIP("no XTEST extension on this X server");
    }
    HostWindow host(conn.display);
    auto surface = GlSurface::create(any_driver());
    REQUIRE(surface->attach(host.handle()));
    std::vector<PointerEvent> pointer;
    InputHandler handler;
    handler.pointer = [&](const PointerEvent& e) { pointer.push_back(e); };
    surface->set_input_handler(handler);

    // The host window sits at the screen's origin, so root and window
    // coordinates agree. Press inside, move well past the 320x240 edge,
    // release there: the press grabs the pointer for the window.
    XTestFakeMotionEvent(conn.display, 0, 100, 100, 0);
    XTestFakeButtonEvent(conn.display, 1, True, 0);
    XSync(conn.display, False);
    REQUIRE(pump(*surface, conn.display, [&] {
        for (const auto& e : pointer)
            if (e.kind == PointerKind::down)
                return true;
        return false;
    }));
    XTestFakeMotionEvent(conn.display, 0, 600, 500, 0);
    XSync(conn.display, False);
    REQUIRE(pump(*surface, conn.display, [&] {
        for (const auto& e : pointer)
            if (e.kind == PointerKind::drag && e.x == 600.0 && e.y == 500.0)
                return true;
        return false;
    }));
    XTestFakeButtonEvent(conn.display, 1, False, 0);
    XSync(conn.display, False);
    REQUIRE(pump(*surface, conn.display,
                 [&] { return !pointer.empty() && pointer.back().kind == PointerKind::up; }));
    // The release lands where the pointer is, outside the surface's size.
    REQUIRE(pointer.back().x == 600.0);
    REQUIRE(pointer.back().y == 500.0);
    REQUIRE(pointer.back().buttons_held == 0u);
}
#endif
