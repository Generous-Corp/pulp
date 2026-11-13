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

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

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

LRESULT CALLBACK child_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
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

struct WglWindow final : GlSurface::Impl {
    HWND hwnd = nullptr;
    HDC hdc = nullptr;
    HGLRC context = nullptr;
    bool class_held = false;
    // Samples per pixel of the window's pixel format. ChoosePixelFormat only
    // offers single-sampled formats, so this stays 0 and a request that asks
    // for multisampling is refused rather than silently given none.
    int window_samples = 0;

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

    void update_geometry() {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        const double scale = window_scale(hwnd);
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
        return true;
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
        if (hwnd != nullptr) {
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
