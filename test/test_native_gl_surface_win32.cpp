// GlSurface on Windows: the child window's lifecycle under a host window, the
// per-binary window class, the client-thread context rule, request selection
// against whatever driver the machine has, and two-phase teardown.

#include <pulp/native/gl_surface.hpp>
#include <pulp/platform/binary_unique_name.hpp>
#include <pulp/platform/win32_sane.hpp>

#include <catch2/catch_test_macros.hpp>

#include <GL/gl.h>

#include <string>
#include <thread>
#include <vector>

using namespace pulp::native;

namespace {

// A hidden top-level window standing in for the host's editor window.
struct HostWindow {
    HWND hwnd = nullptr;
    HostWindow() {
        static const wchar_t* kClass = L"PulpGlSurfaceTestHost";
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = kClass;
            RegisterClassExW(&wc);
            registered = true;
        }
        hwnd = CreateWindowExW(0, kClass, L"host", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr,
                               nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    ~HostWindow() {
        if (hwnd != nullptr && IsWindow(hwnd))
            DestroyWindow(hwnd);
    }
    int children() const {
        int count = 0;
        EnumChildWindows(
            hwnd, [](HWND, LPARAM p) -> BOOL { ++*reinterpret_cast<int*>(p); return TRUE; },
            reinterpret_cast<LPARAM>(&count));
        return count;
    }
};

// The list every machine can satisfy somewhere: a hosted runner without a GPU
// driver has only Microsoft's OpenGL 1.1 software renderer.
GlSurfaceOptions any_driver() {
    GlSurfaceOptions options;
    options.requests = {{3, 3, GlProfile::core, 0}, {2, 1, GlProfile::any, 0}, {1, 1, GlProfile::any, 0}};
    return options;
}

} // namespace

TEST_CASE("the window class name is unique to this binary", "[native][gl-surface][win32]") {
    const std::string name = GlSurface::window_class_name();
    REQUIRE(name == pulp::platform::binary_unique_name("PulpGlSurface"));
    REQUIRE(name != "PulpGlSurface");
    REQUIRE(name.size() > std::string("PulpGlSurface_").size());
}

TEST_CASE("attach makes a child of the host window and detach removes it",
          "[native][gl-surface][win32]") {
    HostWindow host;
    REQUIRE(host.hwnd != nullptr);
    auto surface = GlSurface::create(any_driver());
    REQUIRE(surface != nullptr);
    REQUIRE_FALSE(surface->offscreen());
    REQUIRE_FALSE(surface->attach(nullptr));
    REQUIRE(surface->attach(host.hwnd));
    REQUIRE_FALSE(surface->attach(host.hwnd));  // already attached
    HWND child = static_cast<HWND>(surface->native_window());
    REQUIRE(IsWindow(child));
    REQUIRE(GetParent(child) == host.hwnd);
    REQUIRE(host.children() == 1);
    const auto g = surface->geometry();
    REQUIRE(g.in_window);
    REQUIRE(g.drawable_width > 0);
    REQUIRE(g.scale > 0.0);

    surface->detach();
    REQUIRE_FALSE(surface->attached());
    REQUIRE_FALSE(IsWindow(child));
    REQUIRE(host.children() == 0);
}

TEST_CASE("a hundred attach and detach cycles leave no windows behind",
          "[native][gl-surface][win32]") {
    HostWindow host;
    for (int i = 0; i < 100; ++i) {
        auto surface = GlSurface::create(any_driver());
        REQUIRE(surface->attach(host.hwnd));
        surface->detach();
    }
    REQUIRE(host.children() == 0);
    // Every surface is gone, so the class is unregistered again.
    WNDCLASSEXW info{};
    info.cbSize = sizeof info;
    const std::string narrow = GlSurface::window_class_name();
    const std::wstring wide(narrow.begin(), narrow.end());
    // pulp-native is linked statically into this executable, so the class
    // belongs to the executable's module.
    REQUIRE_FALSE(GetClassInfoExW(GetModuleHandleW(nullptr), wide.c_str(), &info));
}

TEST_CASE("the context is created on the client thread and honours a request",
          "[native][gl-surface][win32]") {
    HostWindow host;
    auto surface = GlSurface::create(any_driver());
    REQUIRE(surface->attach(host.hwnd));
    REQUIRE(wglGetCurrentContext() == nullptr);

    bool made = false;
    bool swapped = false;
    HGLRC before = reinterpret_cast<HGLRC>(1);
    GlContextReport report;
    std::thread render([&] {
        before = wglGetCurrentContext();
        GlContext& gl = surface->context();
        made = gl.make_current();
        if (made) {
            report = gl.report();
            glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            swapped = gl.swap_buffers();
            gl.release_current();
        }
    });
    render.join();
    INFO("GL_VERSION " << report.version << ", GL_RENDERER " << report.renderer);
    REQUIRE(before == nullptr);
    REQUIRE(made);
    REQUIRE(swapped);
    // The report agrees with the request list: the entry it names is the
    // first one a context of this version and profile honours.
    REQUIRE(first_honoured_request(any_driver().requests, report.major, report.minor,
                                   report.profile) == report.request_index);
    REQUIRE(report.max_texture_size > 0);
    // The UI thread never had the context current.
    REQUIRE(wglGetCurrentContext() == nullptr);
    surface->detach();
}

TEST_CASE("detach runs the release handler while the child window still exists",
          "[native][gl-surface][win32]") {
    HostWindow host;
    auto surface = GlSurface::create(any_driver());
    REQUIRE(surface->attach(host.hwnd));
    HWND child = static_cast<HWND>(surface->native_window());
    int releases = 0;
    bool window_alive_in_handler = false;
    bool refused_in_handler = false;
    GlSurface* s = surface.get();
    surface->set_release_handler([&] {
        ++releases;
        window_alive_in_handler = IsWindow(child) == TRUE;
        std::thread probe([&] { refused_in_handler = !s->context().make_current(); });
        probe.join();
    });
    surface->detach();
    REQUIRE(releases == 1);
    REQUIRE(window_alive_in_handler);
    REQUIRE(refused_in_handler);
    REQUIRE_FALSE(IsWindow(child));
}

TEST_CASE("invalidate after the host window is gone touches nothing",
          "[native][gl-surface][win32]") {
    auto host = std::make_unique<HostWindow>();
    auto surface = GlSurface::create(any_driver());
    REQUIRE(surface->attach(host->hwnd));
    int releases = 0;
    surface->set_release_handler([&] { ++releases; });
    host.reset();  // destroys the host window and, with it, the child
    surface->invalidate();
    surface->invalidate();  // idempotent
    REQUIRE(releases == 1);
    REQUIRE_FALSE(surface->attached());
    REQUIRE_FALSE(surface->attach(reinterpret_cast<void*>(1)));  // invalidated for good
}
