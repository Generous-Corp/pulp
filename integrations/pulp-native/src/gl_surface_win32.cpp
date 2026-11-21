// GlSurface on Windows: a child window of the host's HWND with a WGL context.
//
// The window and its pixel format are made on the UI thread. The context is
// made on the client's render thread, on its first make_current(): creating a
// modern context needs a legacy context current to look up
// wglCreateContextAttribsARB, and doing that on the render thread keeps the
// UI thread from ever binding a GL context.

#include <pulp/platform/binary_unique_name.hpp>
#include <pulp/platform/win32_sane.hpp>

#include "gl_surface_impl.h"

#include <GL/gl.h>

#include <windowsx.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

namespace pulp::native {

namespace {

// WGL_ARB_create_context and WGL_ARB_create_context_profile.
constexpr int kWglContextMajorVersion = 0x2091;
constexpr int kWglContextMinorVersion = 0x2092;
constexpr int kWglContextFlags = 0x2094;
constexpr int kWglContextProfileMask = 0x9126;
constexpr int kWglContextCoreProfileBit = 0x0001;
constexpr int kWglContextCompatibilityProfileBit = 0x0002;
constexpr int kWglContextForwardCompatibleBit = 0x0002;
constexpr GLenum kGlContextProfileMask = 0x9126;
constexpr GLenum kGlContextCoreProfileBit = 0x00000001;

using CreateContextAttribs = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using SwapInterval = BOOL(WINAPI*)(int);

// The class is registered once per binary under a name unique to it, and
// unregistered when the last surface that used it goes.
std::mutex g_class_mutex;
int g_class_users = 0;

const std::wstring& class_name_wide() {
    static const std::wstring name = [] {
        const std::string narrow = pulp::platform::binary_unique_name("PulpGlSurface");
        std::wstring wide;
        for (char c : narrow)
            wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        return wide;
    }();
    return name;
}

HINSTANCE this_module() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&this_module), &module);
    return module;
}

// What the window procedure hands messages to: the surface that owns the
// window, found through the window's user data.
struct MessageSink {
    virtual ~MessageSink() = default;
    // True when the message was handled; `result` is then the procedure's
    // return value.
    virtual bool on_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result) = 0;
};

LRESULT CALLBACK child_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (auto* sink = reinterpret_cast<MessageSink*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
        LRESULT result = 0;
        if (sink->on_message(message, wparam, lparam, result))
            return result;
    }
    switch (message) {
    case WM_ERASEBKGND:
        return 1; // the client paints every pixel
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

bool acquire_class() {
    std::lock_guard<std::mutex> lock(g_class_mutex);
    if (g_class_users == 0) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &child_proc;
        wc.hInstance = this_module();
        wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        wc.lpszClassName = class_name_wide().c_str();
        if (RegisterClassExW(&wc) == 0)
            return false;
    }
    ++g_class_users;
    return true;
}

void release_class() {
    std::lock_guard<std::mutex> lock(g_class_mutex);
    if (g_class_users > 0 && --g_class_users == 0)
        UnregisterClassW(class_name_wide().c_str(), this_module());
}

std::string gl_string(GLenum name) {
    const auto* s = reinterpret_cast<const char*>(glGetString(name));
    return s != nullptr ? std::string(s) : std::string();
}

// An extension entry point from the context current on this thread, or null
// when the driver lacks it (including its non-null sentinel values).
template <typename Fn> Fn wgl_extension(const char* name) {
    const auto address = reinterpret_cast<std::intptr_t>(wglGetProcAddress(name));
    return is_usable_wgl_proc(address) ? reinterpret_cast<Fn>(address) : nullptr;
}

double window_scale(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto get_dpi = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    const UINT dpi = get_dpi != nullptr && hwnd != nullptr ? get_dpi(hwnd) : 96u;
    return dpi > 0 ? dpi / 96.0 : 1.0;
}

struct WglWindow final : GlSurface::Impl, MessageSink {
    HWND hwnd = nullptr;
    HDC hdc = nullptr;
    HGLRC context = nullptr;
    bool class_held = false;
    // Samples per pixel of the window's pixel format. ChoosePixelFormat only
    // offers single-sampled formats, so this stays 0 and a request that asks
    // for multisampling is refused rather than silently given none.
    int window_samples = 0;
    // Input state, UI thread.
    HWND previous_focus = nullptr;
    bool holding_focus = false;
    bool pointer_inside = false;
    struct KeyPress {
        bool client_first = false;
        bool forwarded_down = false; // the host saw the key-down, so it gets the key-up
    };
    // Keyed by virtual key and scan identity: Windows reports both Shift, Ctrl
    // or Alt keys as the same generic virtual key, so the side needs the scan.
    std::unordered_map<std::uint64_t, KeyPress> keys_down;
    Win32TakenKeys taken_keys; // taken key-downs whose characters are dropped
    DWORD last_press_time = 0;
    unsigned last_press_button = 0;
    int last_press_x = 0;
    int last_press_y = 0;
    int click_count = 0;

    ~WglWindow() override {
        destroy_window();
    }

    bool offscreen() const override {
        return false;
    }

    void* native_window() const override {
        return hwnd;
    }

    bool attached() const override {
        return hwnd != nullptr;
    }

    double current_scale() const {
        if (scale_override > 0.0)
            return scale_override;
        if (options.scale > 0.0)
            return options.scale;
        return window_scale(hwnd);
    }

    void emit(SurfaceEventKind kind) {
        if (!listener)
            return;
        SurfaceEvent event;
        event.kind = kind;
        {
            std::lock_guard<std::mutex> lock(geometry_mutex);
            event.geometry = geometry;
        }
        listener(event);
    }

    void apply_scale() override {
        if (hwnd == nullptr)
            return;
        update_geometry();
        emit(SurfaceEventKind::geometry_changed);
    }

    void update_geometry() {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        const double scale = current_scale();
        const auto px_w = static_cast<std::uint32_t>(rect.right - rect.left);
        const auto px_h = static_cast<std::uint32_t>(rect.bottom - rect.top);
        std::lock_guard<std::mutex> lock(geometry_mutex);
        ++geometry.generation;
        geometry.scale = scale;
        geometry.width = px_w / scale;
        geometry.height = px_h / scale;
        geometry.drawable_width = px_w;
        geometry.drawable_height = px_h;
        geometry.in_window = true;
    }

    bool attach(void* parent_handle) override {
        HWND parent = static_cast<HWND>(parent_handle);
        if (!IsWindow(parent))
            return false;
        if (!class_held) {
            if (!acquire_class())
                return false;
            class_held = true;
        }
        RECT rect{};
        GetClientRect(parent, &rect);
        hwnd = CreateWindowExW(0, class_name_wide().c_str(), L"",
                               WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0,
                               rect.right - rect.left, rect.bottom - rect.top, parent, nullptr,
                               this_module(), nullptr);
        if (hwnd == nullptr)
            return false;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(static_cast<MessageSink*>(this)));
        keys_down.clear();
        taken_keys.clear();
        click_count = 0;
        pointer_inside = false;
        hdc = GetDC(hwnd);
        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof pfd;
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cAlphaBits = 8;
        pfd.cDepthBits = 24;
        pfd.cStencilBits = 8;
        pfd.iLayerType = PFD_MAIN_PLANE;
        const int format = ChoosePixelFormat(hdc, &pfd);
        if (format == 0 || !SetPixelFormat(hdc, format, &pfd)) {
            detach_window();
            return false;
        }
        update_geometry();
        apply_keyboard_focus();
        return true;
    }

    // ── input ───────────────────────────────────────────────────────────────

    static bool key_state(int vk) {
        return (GetKeyState(vk) & 0x8000) != 0;
    }

    static std::uint32_t current_modifiers() {
        return modifiers_from_win32_keys(
            key_state(VK_SHIFT), key_state(VK_CONTROL), key_state(VK_MENU),
            key_state(VK_LWIN) || key_state(VK_RWIN), (GetKeyState(VK_CAPITAL) & 1) != 0);
    }

    PointerEvent pointer_at(LPARAM lparam, unsigned mk) const {
        PointerEvent e;
        const double scale = current_scale();
        e.x = GET_X_LPARAM(lparam) / scale;
        e.y = GET_Y_LPARAM(lparam) / scale;
        e.buttons_held = buttons_held_from_win32_mk(mk);
        e.modifiers = current_modifiers();
        e.timestamp = GetMessageTime() / 1000.0;
        return e;
    }

    void track_leave() {
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof track;
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd;
        TrackMouseEvent(&track);
    }

    void on_button(UINT message, WPARAM wparam, LPARAM lparam) {
        const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                          message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN;
        unsigned button = 1;
        if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP)
            button = 3; // X numbering: 1 left, 2 middle, 3 right, 8 and up extra
        else if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP)
            button = 2;
        else if (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP)
            button = GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? 8 : 9;
        if (down) {
            // Keep receiving the pointer after it leaves the window.
            SetCapture(hwnd);
        } else if ((GET_KEYSTATE_WPARAM(wparam) &
                    (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)) == 0) {
            ReleaseCapture();
        }
        if (!input.pointer)
            return;
        PointerEvent e = pointer_at(lparam, GET_KEYSTATE_WPARAM(wparam));
        e.kind = down ? PointerKind::down : PointerKind::up;
        // The button numbering is X's (1 left, 2 middle, 3 right, 8 and up extra).
        e.button = pointer_button_for_x11(button, e.button_number);
        if (down) {
            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            const DWORD now = GetMessageTime();
            const bool repeat =
                button == last_press_button && now - last_press_time <= GetDoubleClickTime() &&
                std::abs(x - last_press_x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                std::abs(y - last_press_y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
            click_count = repeat ? click_count + 1 : 1;
            last_press_time = now;
            last_press_button = button;
            last_press_x = x;
            last_press_y = y;
        }
        e.click_count = click_count;
        input.pointer(e);
    }

    void on_move(WPARAM wparam, LPARAM lparam) {
        if (!pointer_inside) {
            pointer_inside = true;
            track_leave();
            if (input.pointer) {
                PointerEvent enter = pointer_at(lparam, static_cast<unsigned>(wparam));
                enter.kind = PointerKind::enter;
                input.pointer(enter);
            }
        }
        if (!input.pointer)
            return;
        PointerEvent e = pointer_at(lparam, static_cast<unsigned>(wparam));
        if (e.buttons_held == 0) {
            e.kind = PointerKind::move;
        } else {
            e.kind = PointerKind::drag;
            for (int n = 0; n < 32; ++n)
                if (e.buttons_held & (1u << n)) {
                    e.button_number = n;
                    e.button = n == 0   ? PointerButton::left
                               : n == 1 ? PointerButton::right
                               : n == 2 ? PointerButton::middle
                                        : PointerButton::other;
                    break;
                }
        }
        input.pointer(e);
    }

    void on_leave() {
        pointer_inside = false;
        if (!input.pointer)
            return;
        PointerEvent e;
        e.kind = PointerKind::leave;
        e.timestamp = GetMessageTime() / 1000.0;
        input.pointer(e);
    }

    void on_wheel(WPARAM wparam, LPARAM lparam, bool horizontal) {
        if (!input.wheel) {
            // No wheel handler: the host's scroll view keeps working.
            HWND parent = GetParent(hwnd);
            if (parent != nullptr)
                PostMessageW(parent, horizontal ? WM_MOUSEHWHEEL : WM_MOUSEWHEEL, wparam, lparam);
            return;
        }
        POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd, &p); // wheel messages carry screen coordinates
        const double scale = current_scale();
        WheelEvent e;
        e.x = p.x / scale;
        e.y = p.y / scale;
        const double lines = win32_wheel_lines(GET_WHEEL_DELTA_WPARAM(wparam), horizontal);
        (horizontal ? e.dx : e.dy) = lines;
        e.raw_dx = e.dx;
        e.raw_dy = e.dy;
        e.precise = false;
        e.modifiers = current_modifiers();
        e.timestamp = GetMessageTime() / 1000.0;
        input.wheel(e);
    }

    void on_key(UINT message, WPARAM wparam, LPARAM lparam) {
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        const unsigned vk = static_cast<unsigned>(wparam);
        KeyEvent key;
        key.kind = down ? KeyKind::down : KeyKind::up;
        key.key = key_for_win32_vk(vk, (lparam & (1 << 24)) != 0,
                                    static_cast<std::uint32_t>((lparam >> 16) & 0xff));
        key.native_code = static_cast<std::uint16_t>(vk);
        key.modifiers = current_modifiers();
        key.timestamp = GetMessageTime() / 1000.0;
        const std::uint64_t press_id =
            (static_cast<std::uint64_t>(Win32TakenKeys::id_from_lparam(
                 static_cast<std::uint64_t>(lparam)))
             << 32) |
            vk;
        const auto known = keys_down.find(press_id);
        key.repeat = down && known != keys_down.end();
        // Whether the client goes first is decided at the first press and kept
        // for its repeats and its release.
        bool client_first;
        bool was_forwarded = false;
        if (known != keys_down.end()) {
            client_first = known->second.client_first;
            was_forwarded = known->second.forwarded_down;
        } else {
            client_first = down && route_key(key, keyboard_focus) == KeyRoute::client_first;
        }
        bool taken = false;
        if (client_first) {
            taken = input.key && input.key(key);
            // A key the client took is not also typed. The characters it makes
            // are matched by scan code, so a taken key that makes none leaves
            // the next typed character alone.
            if (taken && down)
                taken_keys.taken(Win32TakenKeys::id_from_lparam(static_cast<std::uint64_t>(lparam)));
        }
        if (!down)
            taken_keys.released(Win32TakenKeys::id_from_lparam(static_cast<std::uint64_t>(lparam)));
        // Whether the host sees a key is decided with its first press and kept
        // for its repeats and its release, so it never gets an unmatched repeat
        // or key-up. A client-first key in a text field is typed, not sent.
        bool forward;
        if (known != keys_down.end())
            forward = was_forwarded;
        else if (down)
            forward = !taken && !(client_first && keyboard_focus == KeyboardFocus::text);
        else
            forward = true; // a release whose press this window never saw
        if (down) {
            if (known == keys_down.end()) {
                KeyPress& press = keys_down[press_id];
                press.client_first = client_first;
                press.forwarded_down = forward;
            }
        } else if (known != keys_down.end()) {
            keys_down.erase(press_id);
        }
        if (forward) {
            HWND parent = GetParent(hwnd);
            if (parent != nullptr)
                PostMessageW(parent, message, wparam, lparam);
        }
    }

    void on_char(WPARAM wparam, LPARAM lparam) {
        if (taken_keys.suppresses_char(
                Win32TakenKeys::id_from_lparam(static_cast<std::uint64_t>(lparam))))
            return;
        if (keyboard_focus != KeyboardFocus::text || !input.text)
            return;
        // UTF-16 to UTF-8; a surrogate pair arrives as two WM_CHAR messages.
        const unsigned unit = static_cast<unsigned>(wparam);
        if (unit < 0x20 || unit == 0x7f)
            return;
        std::uint32_t code_point = unit;
        if (unit >= 0xd800 && unit <= 0xdbff) {
            high_surrogate = unit;
            return;
        }
        if (unit >= 0xdc00 && unit <= 0xdfff) {
            if (high_surrogate == 0)
                return;
            code_point = 0x10000 + ((high_surrogate - 0xd800) << 10) + (unit - 0xdc00);
        }
        high_surrogate = 0;
        std::string out;
        if (code_point < 0x80) {
            out.push_back(static_cast<char>(code_point));
        } else if (code_point < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
        } else if (code_point < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
        }
        input.text(out);
    }
    unsigned high_surrogate = 0;

    bool on_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result) override {
        result = 0;
        switch (message) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            on_button(message, wparam, lparam);
            return true;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            on_button(message, wparam, lparam);
            result = TRUE;
            return true;
        case WM_MOUSEMOVE:
            on_move(wparam, lparam);
            return true;
        case WM_MOUSELEAVE:
            on_leave();
            return true;
        case WM_MOUSEWHEEL:
            on_wheel(wparam, lparam, false);
            return true;
        case WM_MOUSEHWHEEL:
            on_wheel(wparam, lparam, true);
            return true;
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            on_key(message, wparam, lparam);
            return true;
        case WM_CHAR:
            on_char(wparam, lparam);
            return true;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            // A release can go to another window: forget the taken keys.
            taken_keys.clear();
            if (input.focus) {
                FocusEvent focus;
                focus.focused = message == WM_SETFOCUS;
                focus.reason = FocusReason::responder;
                input.focus(focus);
            }
            return false; // the default procedure still runs
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT) {
                SetCursor(cursor_handle());
                result = TRUE;
                return true;
            }
            return false;
        default:
            return false;
        }
    }

    // ── keyboard focus and cursor ───────────────────────────────────────────

    HCURSOR cursor_handle() const {
        return LoadCursorW(nullptr, MAKEINTRESOURCEW(win32_cursor_resource(cursor)));
    }

    void apply_cursor() override {
        if (hwnd == nullptr)
            return;
        POINT p{};
        if (GetCursorPos(&p) && WindowFromPoint(p) == hwnd)
            SetCursor(cursor_handle());
    }

    void apply_keyboard_focus() override {
        if (hwnd == nullptr)
            return;
        if (keyboard_focus != KeyboardFocus::none) {
            if (!holding_focus) {
                previous_focus = GetFocus();
                holding_focus = true;
            }
            SetFocus(hwnd);
        } else {
            release_keyboard();
        }
    }

    // Give the keyboard back to whoever had it.
    void release_keyboard() {
        if (!holding_focus)
            return;
        holding_focus = false;
        if (hwnd != nullptr && GetFocus() == hwnd) {
            HWND back =
                previous_focus != nullptr && IsWindow(previous_focus) && previous_focus != hwnd
                    ? previous_focus
                    : GetParent(hwnd);
            SetFocus(back);
        }
    }

    // Read the version, renderer and limits of the context current on this
    // thread.
    static GlContextReport read_report(GlProfile asked) {
        GlContextReport report;
        report.version = gl_string(GL_VERSION);
        report.renderer = gl_string(GL_RENDERER);
        report.vendor = gl_string(GL_VENDOR);
        std::sscanf(report.version.c_str(), "%d.%d", &report.major, &report.minor);
        report.profile = GlProfile::any;
        if (report.major > 3 || (report.major == 3 && report.minor >= 2)) {
            GLint mask = 0;
            glGetIntegerv(kGlContextProfileMask, &mask);
            report.profile =
                (mask & kGlContextCoreProfileBit) != 0 ? GlProfile::core : GlProfile::compatibility;
        } else if (asked == GlProfile::compatibility && report.major >= 3) {
            report.profile = GlProfile::compatibility;
        }
        GLint max_texture = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
        report.max_texture_size = max_texture;
        report.npot_textures = report.major >= 2;
        report.software = is_software_renderer(report.renderer);
        return report;
    }

    // Keep `candidate` (current on this thread) if it honours request `index`.
    bool accept(HGLRC candidate, const GlSurfaceOptions& options, std::size_t index,
                GlContextReport& out) {
        GlContextReport report = read_report(options.requests[index].profile);
        if (!first_honoured_request({options.requests[index]}, report.major, report.minor,
                                    report.profile, window_samples))
            return false;
        report.request_index = index;
        context = candidate;
        out = report;
        if (auto swap_interval = wgl_extension<SwapInterval>("wglSwapIntervalEXT"))
            swap_interval(options.swap_interval);
        return true;
    }

    bool create_context(const GlSurfaceOptions& options, GlContextReport& out) override {
        if (hdc == nullptr)
            return false;
        HGLRC previous_context = wglGetCurrentContext();
        HDC previous_dc = wglGetCurrentDC();

        HGLRC legacy = wglCreateContext(hdc);
        if (legacy == nullptr || !wglMakeCurrent(hdc, legacy)) {
            if (legacy != nullptr)
                wglDeleteContext(legacy);
            wglMakeCurrent(previous_dc, previous_context);
            return false;
        }
        const auto create_attribs =
            wgl_extension<CreateContextAttribs>("wglCreateContextAttribsARB");

        for (std::size_t i = 0; i < options.requests.size(); ++i) {
            const GlContextRequest& request = options.requests[i];
            if (request.major < 3 || create_attribs == nullptr) {
                // OpenGL 2.1 and earlier: the legacy context is the candidate.
                wglMakeCurrent(hdc, legacy);
                if (accept(legacy, options, i, out))
                    return true;
                continue;
            }
            // `any` tries core and then compatibility, so a core-only driver
            // still honours it.
            for (const GlProfile attempt : context_profile_attempts(request.profile)) {
                const bool core = attempt == GlProfile::core;
                const int attribs[] = {
                    kWglContextMajorVersion,
                    request.major,
                    kWglContextMinorVersion,
                    request.minor,
                    kWglContextProfileMask,
                    core ? kWglContextCoreProfileBit : kWglContextCompatibilityProfileBit,
                    kWglContextFlags,
                    request.profile == GlProfile::core ? kWglContextForwardCompatibleBit : 0,
                    0};
                HGLRC modern = create_attribs(hdc, nullptr, attribs);
                if (modern == nullptr)
                    continue;
                if (wglMakeCurrent(hdc, modern) && accept(modern, options, i, out)) {
                    wglDeleteContext(legacy);
                    return true;
                }
                wglMakeCurrent(hdc, legacy);
                wglDeleteContext(modern);
            }
        }
        wglMakeCurrent(previous_dc, previous_context);
        wglDeleteContext(legacy);
        return false;
    }

    bool make_current() override {
        return context != nullptr && hdc != nullptr && wglMakeCurrent(hdc, context) == TRUE;
    }

    void release_current() override {
        if (context != nullptr && wglGetCurrentContext() == context)
            wglMakeCurrent(nullptr, nullptr);
    }

    bool swap() override {
        return hdc != nullptr && wglGetCurrentContext() == context && SwapBuffers(hdc) == TRUE;
    }

    bool set_interval(int interval) override {
        auto swap_interval = wgl_extension<SwapInterval>("wglSwapIntervalEXT");
        return swap_interval != nullptr && swap_interval(interval) == TRUE;
    }

    void* proc(const char* name) const override {
        // wglGetProcAddress returns small sentinel values, not null, for
        // entry points it does not know; OpenGL 1.1 ones come from the DLL.
        auto p = reinterpret_cast<std::intptr_t>(wglGetProcAddress(name));
        if (!is_usable_wgl_proc(p))
            return reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"opengl32.dll"), name));
        return reinterpret_cast<void*>(p);
    }

    void destroy_context() override {
        // The client released the context before the release handler
        // returned, so it is current nowhere.
        if (context != nullptr)
            wglDeleteContext(context);
        context = nullptr;
    }

    void detach_window() override {
        taken_keys.clear();
        if (hwnd != nullptr) {
            release_keyboard();
            // No message reaches this object once the window starts to go.
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (GetCapture() == hwnd)
                ReleaseCapture();
            if (hdc != nullptr)
                ReleaseDC(hwnd, hdc);
            DestroyWindow(hwnd);
        }
        hwnd = nullptr;
        hdc = nullptr;
        std::lock_guard<std::mutex> lock(geometry_mutex);
        ++geometry.generation;
        geometry.in_window = false;
    }

    void forget_window() override {
        // The parent, and with it the child window and its DC, are already
        // gone or going: drop the handles without touching them.
        holding_focus = false;
        taken_keys.clear();
        hwnd = nullptr;
        hdc = nullptr;
    }

    void destroy_window() override {
        detach_window();
        if (class_held) {
            release_class();
            class_held = false;
        }
    }
};

} // namespace

std::unique_ptr<GlSurface::Impl> make_windowed_gl_impl(const GlSurfaceOptions& options) {
    auto impl = std::make_unique<WglWindow>();
    impl->options = options;
    return impl;
}

std::unique_ptr<GlSurface::Impl> make_offscreen_gl_impl(std::uint32_t, std::uint32_t,
                                                        const GlSurfaceOptions&) {
    // Offscreen GL on Windows needs a hidden window plus framebuffer objects,
    // which the GDI software renderer lacks. Not offered yet.
    return nullptr;
}

std::string windowed_gl_class_name() {
    // The name is ASCII (a base name and a hex token).
    std::string narrow;
    for (wchar_t c : class_name_wide())
        narrow.push_back(static_cast<char>(c));
    return narrow;
}

} // namespace pulp::native
