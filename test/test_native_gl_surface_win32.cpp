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

// What the host window receives: input the surface sent on to it.
int g_host_keys = 0;
int g_host_wheels = 0;

LRESULT CALLBACK host_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_KEYDOWN)
        ++g_host_keys;
    else if (message == WM_MOUSEWHEEL)
        ++g_host_wheels;
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

// Drain the messages the surface posted to the host window.
void pump_messages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// A hidden top-level window standing in for the host's editor window.
struct HostWindow {
    HWND hwnd = nullptr;
    HostWindow() {
        static const wchar_t* kClass = L"PulpGlSurfaceTestHost";
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            wc.lpfnWndProc = host_proc;
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
            hwnd,
            [](HWND, LPARAM p) -> BOOL {
                ++*reinterpret_cast<int*>(p);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&count));
        return count;
    }
};

// The list every machine can satisfy somewhere: a hosted runner without a GPU
// driver has only Microsoft's OpenGL 1.1 software renderer.
GlSurfaceOptions any_driver() {
    GlSurfaceOptions options;
    options.requests = {
        {3, 3, GlProfile::core, 0}, {2, 1, GlProfile::any, 0}, {1, 1, GlProfile::any, 0}};
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
    REQUIRE_FALSE(surface->attach(host.hwnd)); // already attached
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

TEST_CASE("a multisampled request is refused rather than given a single-sampled context",
          "[native][gl-surface][win32]") {
    HostWindow host;
    GlSurfaceOptions sampled_only;
    sampled_only.requests = {{1, 1, GlProfile::any, 4}};
    auto refused = GlSurface::create(sampled_only);
    REQUIRE(refused->attach(host.hwnd));
    bool made = true;
    std::thread first([&] { made = refused->context().make_current(); });
    first.join();
    REQUIRE_FALSE(made);
    refused->detach();

    GlSurfaceOptions fallback;
    fallback.requests = {{1, 1, GlProfile::any, 4}, {1, 1, GlProfile::any, 0}};
    auto surface = GlSurface::create(fallback);
    REQUIRE(surface->attach(host.hwnd));
    std::size_t index = 99;
    std::thread second([&] {
        GlContext& gl = surface->context();
        if (gl.make_current()) {
            index = gl.report().request_index;
            gl.release_current();
        }
    });
    second.join();
    REQUIRE(index == 1u);
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
    host.reset(); // destroys the host window and, with it, the child
    surface->invalidate();
    surface->invalidate(); // idempotent
    REQUIRE(releases == 1);
    REQUIRE_FALSE(surface->attached());
    REQUIRE_FALSE(surface->attach(reinterpret_cast<void*>(1))); // invalidated for good
}

// ── Input, cursor and focus ─────────────────────────────────────────────────
//
// Messages are sent straight to the child window, the way the system delivers
// them, so the window procedure's translation is what is under test.

namespace {

HWND child_of(GlSurface& surface) {
    return static_cast<HWND>(surface.native_window());
}

LPARAM at(int x, int y) {
    return MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y));
}

} // namespace

TEST_CASE("pointer messages reach the handler in logical points", "[native][gl-surface][win32]") {
    HostWindow host;
    GlSurfaceOptions options = any_driver();
    options.scale = 2.0;
    auto surface = GlSurface::create(options);
    REQUIRE(surface->attach(host.hwnd));
    std::vector<PointerEvent> pointer;
    InputHandler handler;
    handler.pointer = [&](const PointerEvent& e) { pointer.push_back(e); };
    surface->set_input_handler(handler);
    HWND child = child_of(*surface);

    SendMessageW(child, WM_LBUTTONDOWN, MK_LBUTTON, at(40, 60));
    SendMessageW(child, WM_LBUTTONUP, 0, at(40, 60));
    SendMessageW(child, WM_RBUTTONDOWN, MK_RBUTTON, at(10, 10));
    SendMessageW(child, WM_RBUTTONUP, 0, at(10, 10));
    SendMessageW(child, WM_MBUTTONDOWN, MK_MBUTTON, at(10, 10));
    SendMessageW(child, WM_MBUTTONUP, 0, at(10, 10));
    // The first move of a pointer that was outside is an enter.
    SendMessageW(child, WM_MOUSEMOVE, 0, at(100, 50));
    SendMessageW(child, WM_MOUSEMOVE, MK_LBUTTON, at(110, 50));

    REQUIRE(pointer.size() >= 10);
    REQUIRE(pointer[0].kind == PointerKind::down);
    REQUIRE(pointer[0].x == 20.0);
    REQUIRE(pointer[0].y == 30.0);
    REQUIRE(pointer[0].button == PointerButton::left);
    REQUIRE(pointer[0].button_number == 0);
    REQUIRE(pointer[0].buttons_held == 1u);
    REQUIRE(pointer[0].click_count == 1);
    REQUIRE(pointer[1].kind == PointerKind::up);
    REQUIRE(pointer[2].button == PointerButton::right);
    REQUIRE(pointer[2].button_number == 1);
    REQUIRE(pointer[4].button == PointerButton::middle);
    REQUIRE(pointer[4].button_number == 2);
    bool saw_enter = false;
    bool saw_move = false;
    bool saw_drag = false;
    for (const auto& e : pointer) {
        saw_enter = saw_enter || e.kind == PointerKind::enter;
        saw_move = saw_move || (e.kind == PointerKind::move && e.x == 50.0);
        saw_drag = saw_drag ||
                   (e.kind == PointerKind::drag && e.button == PointerButton::left && e.x == 55.0);
    }
    REQUIRE(saw_enter);
    REQUIRE(saw_move);
    REQUIRE(saw_drag);

    // A second press in the same place straight away is a double click.
    pointer.clear();
    SendMessageW(child, WM_LBUTTONDOWN, MK_LBUTTON, at(40, 60));
    SendMessageW(child, WM_LBUTTONUP, 0, at(40, 60));
    SendMessageW(child, WM_LBUTTONDOWN, MK_LBUTTON, at(41, 60));
    SendMessageW(child, WM_LBUTTONUP, 0, at(41, 60));
    REQUIRE(pointer.size() == 4);
    REQUIRE(pointer[2].click_count == 2);
    surface->detach();
}

TEST_CASE("wheel messages reach the handler, or go on to the host", "[native][gl-surface][win32]") {
    HostWindow host;
    auto surface = GlSurface::create(any_driver());
    REQUIRE(surface->attach(host.hwnd));
    HWND child = child_of(*surface);
    // Wheel messages carry screen coordinates.
    POINT origin{20, 20};
    ClientToScreen(child, &origin);
    const LPARAM screen = at(origin.x, origin.y);

    // No wheel handler: the message is posted to the host window.
    g_host_wheels = 0;
    SendMessageW(child, WM_MOUSEWHEEL, MAKEWPARAM(0, 120), screen);
    pump_messages();
    REQUIRE(g_host_wheels == 1);

    std::vector<WheelEvent> wheel;
    InputHandler handler;
    handler.wheel = [&](const WheelEvent& e) { wheel.push_back(e); };
    surface->set_input_handler(handler);
    SendMessageW(child, WM_MOUSEWHEEL, MAKEWPARAM(0, 120), screen);
    SendMessageW(child, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-120)), screen);
    SendMessageW(child, WM_MOUSEHWHEEL, MAKEWPARAM(0, 120), screen);
    pump_messages();
    REQUIRE(g_host_wheels == 1); // the handler took them
    REQUIRE(wheel.size() == 3);
    REQUIRE(wheel[0].dy > 0.0);
    REQUIRE(wheel[0].dx == 0.0);
    REQUIRE_FALSE(wheel[0].precise);
    REQUIRE(wheel[0].x == 20.0);
    REQUIRE(wheel[1].dy < 0.0);
    REQUIRE(wheel[2].dx < 0.0); // a right tilt moves content left
    surface->detach();
}

TEST_CASE("keys go to the host unless the client holds the keyboard",
          "[native][gl-surface][win32]") {
    HostWindow host;
    auto surface = GlSurface::create(any_driver());
    REQUIRE(surface->attach(host.hwnd));
    HWND child = child_of(*surface);
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

    // No keyboard focus: the client is not offered the key; the host gets it.
    g_host_keys = 0;
    SendMessageW(child, WM_KEYDOWN, VK_SPACE, 0);
    SendMessageW(child, WM_KEYUP, VK_SPACE, 0xC0000001);
    pump_messages();
    REQUIRE(keys.empty());
    REQUIRE(g_host_keys == 1);

    // Keys held: offered first; a consumed key does not reach the host.
    surface->set_keyboard_focus(KeyboardFocus::keys);
    g_host_keys = 0;
    SendMessageW(child, WM_KEYDOWN, 'A', 0);
    SendMessageW(child, WM_KEYUP, 'A', 0xC0000001);
    pump_messages();
    REQUIRE(keys.size() == 2);
    REQUIRE(keys[0].kind == KeyKind::down);
    REQUIRE(keys[0].key == Key::a);
    REQUIRE_FALSE(keys[0].repeat);
    REQUIRE(keys[1].kind == KeyKind::up);
    REQUIRE(g_host_keys == 0);

    // A declined key goes on to the host.
    keys.clear();
    consume = false;
    SendMessageW(child, WM_KEYDOWN, VK_SPACE, 0);
    SendMessageW(child, WM_KEYUP, VK_SPACE, 0xC0000001);
    pump_messages();
    REQUIRE(keys.size() == 2);
    REQUIRE(g_host_keys == 1);

    // A held key repeats; the repeat is the same key, marked as one.
    keys.clear();
    consume = true;
    SendMessageW(child, WM_KEYDOWN, 'B', 0);
    SendMessageW(child, WM_KEYDOWN, 'B', 0x40000001);
    REQUIRE(keys.size() == 2);
    REQUIRE_FALSE(keys[0].repeat);
    REQUIRE(keys[1].repeat);
    SendMessageW(child, WM_KEYUP, 'B', 0xC0000001);

    // A key the client took is not also typed; an untaken one types text.
    surface->set_keyboard_focus(KeyboardFocus::text);
    texts.clear();
    SendMessageW(child, WM_KEYDOWN, 'C', 0);
    SendMessageW(child, WM_CHAR, 'c', 0);
    SendMessageW(child, WM_KEYUP, 'C', 0xC0000001);
    REQUIRE(texts.empty());
    consume = false;
    SendMessageW(child, WM_KEYDOWN, 'D', 0);
    SendMessageW(child, WM_CHAR, 'd', 0);
    SendMessageW(child, WM_KEYUP, 'D', 0xC0000001);
    REQUIRE(texts.size() == 1);
    REQUIRE(texts[0] == "d");
    // Text is UTF-8: a euro sign and a surrogate pair.
    SendMessageW(child, WM_CHAR, 0x20AC, 0);
    SendMessageW(child, WM_CHAR, 0xD83D, 0);
    SendMessageW(child, WM_CHAR, 0xDE00, 0);
    REQUIRE(texts.size() == 3);
    REQUIRE(texts[1] == "\xE2\x82\xAC");
    REQUIRE(texts[2] == "\xF0\x9F\x98\x80");
    // Control characters are not text.
    SendMessageW(child, WM_CHAR, 0x08, 0);
    REQUIRE(texts.size() == 3);
    surface->detach();
}

TEST_CASE("focus messages reach the handler and set_keyboard_focus takes the window focus",
          "[native][gl-surface][win32]") {
    HostWindow host;
    ShowWindow(host.hwnd, SW_SHOWNA);
    auto surface = GlSurface::create(any_driver());
    REQUIRE(surface->attach(host.hwnd));
    HWND child = child_of(*surface);
    std::vector<bool> focus;
    InputHandler handler;
    handler.focus = [&](const FocusEvent& e) { focus.push_back(e.focused); };
    surface->set_input_handler(handler);

    SendMessageW(child, WM_SETFOCUS, 0, 0);
    SendMessageW(child, WM_KILLFOCUS, 0, 0);
    REQUIRE(focus == std::vector<bool>{true, false});

    // Holding the keyboard asks Windows for focus; on a runner with no
    // interactive desktop it can refuse, so only the round trip is required.
    surface->set_keyboard_focus(KeyboardFocus::keys);
    const bool took = GetFocus() == child;
    surface->set_keyboard_focus(KeyboardFocus::none);
    if (took)
        REQUIRE(GetFocus() != child);
    surface->detach();
}

TEST_CASE("the cursor over the surface follows set_cursor", "[native][gl-surface][win32]") {
    HostWindow host;
    auto surface = GlSurface::create(any_driver());
    surface->set_cursor(Cursor::crosshair); // before attaching: kept
    REQUIRE(surface->attach(host.hwnd));
    HWND child = child_of(*surface);

    auto shown_after_setcursor = [&] {
        SendMessageW(child, WM_SETCURSOR, reinterpret_cast<WPARAM>(child),
                     MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        return GetCursor();
    };
    const HCURSOR crosshair = LoadCursorW(nullptr, IDC_CROSS);
    const HCURSOR ibeam = LoadCursorW(nullptr, IDC_IBEAM);
    const HCURSOR hand = LoadCursorW(nullptr, IDC_HAND);
    REQUIRE(shown_after_setcursor() == crosshair);
    surface->set_cursor(Cursor::ibeam);
    REQUIRE(shown_after_setcursor() == ibeam);
    surface->set_cursor(Cursor::pointing_hand);
    REQUIRE(shown_after_setcursor() == hand);
    surface->set_cursor(Cursor::ibeam);
    REQUIRE(shown_after_setcursor() == ibeam);
    surface->detach();
}

TEST_CASE("messages after detach touch nothing", "[native][gl-surface][win32]") {
    HostWindow host;
    auto surface = GlSurface::create(any_driver());
    REQUIRE(surface->attach(host.hwnd));
    int calls = 0;
    InputHandler handler;
    handler.pointer = [&](const PointerEvent&) { ++calls; };
    surface->set_input_handler(handler);
    HWND child = child_of(*surface);
    surface->detach();
    // The window is gone; a late message to its handle is the system's problem,
    // not a call into the destroyed surface.
    SendMessageW(child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(1, 1));
    REQUIRE(calls == 0);
}
