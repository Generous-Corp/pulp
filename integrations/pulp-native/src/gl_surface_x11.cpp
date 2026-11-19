// GlSurface on X11: a child window of the host's window with a GLX context.
//
// Two connections to the X server, so the two threads never share one:
//   - `ui`, opened on attach, owns the child window, follows the host window
//     and receives input. It is used on the UI thread only.
//   - `rdpy`, opened on the client's render thread on its first make_current(),
//     owns the context. It is used by the render thread, and by the UI thread
//     only after the release handler has returned (teardown).
// Xlib cannot be told to lock before a plug-in loads (XInitThreads has to be
// the first Xlib call in the process), so separate connections are the only
// safe arrangement.
//
// Xlib's default error handler ends the process. A window that vanishes under
// us (the host closed it first), or a context request the server cannot honour
// (the normal fall-through of the request list), raises an X error, and that
// must never take the host down. ErrorRegistry swallows the errors of our two
// connections and hands every other connection's errors to the handler that
// was there before.
//
// No thread of its own: pointer, key and resize events are read on the UI
// thread by dispatch_events(), which the editor calls from its idle tick or
// when event_fd() is readable.

#include <pulp/platform/binary_unique_name.hpp>

#include "gl_surface_impl.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// The X headers define macros (None, Bool, KeyPress, ...); keep them after
// every Pulp header.
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

namespace pulp::native {

namespace {

// The protocol values x11_translate.cpp relies on, against the real headers.
static_assert(ShiftMask == 1u << 0 && LockMask == 1u << 1 && ControlMask == 1u << 2 &&
              Mod1Mask == 1u << 3 && Mod4Mask == 1u << 6);
static_assert(Button1Mask == 1u << 8 && Button2Mask == 1u << 9 && Button3Mask == 1u << 10);
static_assert(XK_a == 0x61 && XK_Return == 0xff0d && XK_Escape == 0xff1b && XK_F1 == 0xffbe &&
              XK_KP_0 == 0xffb0 && XK_KP_Home == 0xff95 && XK_Shift_L == 0xffe1 &&
              XK_Super_L == 0xffeb);

// GLX_ARB_create_context and GLX_ARB_create_context_profile.
constexpr int kGlxContextMajorVersion = 0x2091;
constexpr int kGlxContextMinorVersion = 0x2092;
constexpr int kGlxContextFlags = 0x2094;
constexpr int kGlxContextProfileMask = 0x9126;
constexpr int kGlxContextCoreProfileBit = 0x0001;
constexpr int kGlxContextCompatibilityProfileBit = 0x0002;
constexpr int kGlxContextForwardCompatibleBit = 0x0002;
constexpr GLenum kGlContextProfileMask = 0x9126;
constexpr GLint kGlContextCoreProfileBit = 0x00000001;

using CreateContextAttribs = GLXContext (*)(Display*, GLXFBConfig, GLXContext, int, const int*);
using SwapIntervalExt = void (*)(Display*, GLXDrawable, int);
using SwapIntervalIntFn = int (*)(int);

// ── X error containment ─────────────────────────────────────────────────────

class ErrorRegistry {
  public:
    static ErrorRegistry& instance() {
        // Never destroyed: a handler can run during process shutdown.
        static ErrorRegistry* registry = new ErrorRegistry;
        return *registry;
    }

    // Start containing the errors of `display`.
    void add(Display* display) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (users_++ == 0) {
            XErrorHandler previous = XSetErrorHandler(&ErrorRegistry::on_error);
            if (previous != &ErrorRegistry::on_error)
                previous_ = previous;
        }
        entries_.push_back({display, 0});
    }

    // Stop containing `display`'s errors. The last one restores the handler
    // that was in place, unless someone installed another on top.
    void remove(Display* display) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [&](const auto& e) { return e.first == display; }),
                       entries_.end());
        if (users_ > 0 && --users_ == 0) {
            XErrorHandler current = XSetErrorHandler(previous_);
            if (current != &ErrorRegistry::on_error)
                XSetErrorHandler(current);
        }
    }

    // Errors on `display` since the last call; resets the count.
    int take(Display* display) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& e : entries_)
            if (e.first == display) {
                const int count = e.second;
                e.second = 0;
                return count;
            }
        return 0;
    }

  private:
    static int on_error(Display* display, XErrorEvent* event) {
        XErrorHandler chain = nullptr;
        {
            ErrorRegistry& self = instance();
            std::lock_guard<std::mutex> lock(self.mutex_);
            for (auto& e : self.entries_)
                if (e.first == display) {
                    ++e.second;
                    return 0;
                }
            chain = self.previous_;
        }
        return chain != nullptr ? chain(display, event) : 0;
    }

    std::mutex mutex_;
    std::vector<std::pair<Display*, int>> entries_;
    int users_ = 0;
    XErrorHandler previous_ = nullptr;
};

// Round-trip to the server so every error our requests raised has arrived,
// then report whether there was one.
bool sync_failed(Display* display) {
    XSync(display, False);
    return ErrorRegistry::instance().take(display) != 0;
}

std::string gl_string(GLenum name) {
    const auto* s = reinterpret_cast<const char*>(glGetString(name));
    return s != nullptr ? std::string(s) : std::string();
}

template <typename Fn> Fn glx_extension(const char* name) {
    return reinterpret_cast<Fn>(glXGetProcAddressARB(reinterpret_cast<const GLubyte*>(name)));
}

void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

struct X11Window final : GlSurface::Impl {
    // UI-thread side.
    Display* ui = nullptr;
    std::string display_name;
    int screen = 0;
    Window parent = 0;
    Window child = 0;
    Colormap colormap = 0;
    int fbconfig_id = 0;
    unsigned pixel_w = 0;
    unsigned pixel_h = 0;
    double detected_scale = 1.0;
    bool parent_gone = false;
    OnScreenState visibility;
    // Keyboard.
    Window previous_focus = 0;
    bool holding_focus = false;
    std::unordered_map<unsigned, bool> keys_down; // keycode -> client went first
    // Multi-click counting.
    Time last_press_time = 0;
    unsigned last_press_button = 0;
    int last_press_x = 0;
    int last_press_y = 0;
    int click_count = 0;
    // Render-thread side.
    Display* rdpy = nullptr;
    GLXContext context = nullptr;
    GLXWindow drawable = 0;

    ~X11Window() override {
        destroy_context();
        detach_window();
    }

    bool offscreen() const override {
        return false;
    }

    void* native_window() const override {
        return child != 0 ? reinterpret_cast<void*>(static_cast<std::uintptr_t>(child)) : nullptr;
    }

    // Attached means the connection is open: a window the host destroyed under
    // us still has resources to release through detach().
    bool attached() const override {
        return ui != nullptr;
    }

    int event_fd() const override {
        return ui != nullptr ? ConnectionNumber(ui) : -1;
    }

    // ── geometry ────────────────────────────────────────────────────────────

    double current_scale() const {
        if (scale_override > 0.0)
            return scale_override;
        if (options.scale > 0.0)
            return options.scale;
        return detected_scale;
    }

    void update_geometry() {
        const double scale = current_scale();
        std::lock_guard<std::mutex> lock(geometry_mutex);
        ++geometry.generation;
        geometry.scale = scale;
        geometry.width = pixel_w / scale;
        geometry.height = pixel_h / scale;
        geometry.drawable_width = pixel_w;
        geometry.drawable_height = pixel_h;
        geometry.in_window = !parent_gone;
    }

    void clear_geometry() {
        std::lock_guard<std::mutex> lock(geometry_mutex);
        ++geometry.generation;
        geometry.width = 0.0;
        geometry.height = 0.0;
        geometry.scale = 0.0;
        geometry.drawable_width = 0;
        geometry.drawable_height = 0;
        geometry.in_window = false;
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
        event.visibility = visibility;
        listener(event);
    }

    void apply_scale() override {
        if (ui == nullptr)
            return;
        update_geometry();
        emit(SurfaceEventKind::geometry_changed);
    }

    // ── attach and detach ───────────────────────────────────────────────────

    // The framebuffer configuration for the window: double buffered RGBA with
    // depth and stencil, falling back to fewer buffers on a server that offers
    // no such visual. Null when there is none.
    GLXFBConfig choose_fbconfig() {
        for (int stencil : {8, 0}) {
            const int attribs[] = {GLX_X_RENDERABLE,
                                   True,
                                   GLX_DRAWABLE_TYPE,
                                   GLX_WINDOW_BIT,
                                   GLX_RENDER_TYPE,
                                   GLX_RGBA_BIT,
                                   GLX_X_VISUAL_TYPE,
                                   GLX_TRUE_COLOR,
                                   GLX_RED_SIZE,
                                   8,
                                   GLX_GREEN_SIZE,
                                   8,
                                   GLX_BLUE_SIZE,
                                   8,
                                   GLX_DEPTH_SIZE,
                                   stencil > 0 ? 24 : 16,
                                   GLX_STENCIL_SIZE,
                                   stencil,
                                   GLX_DOUBLEBUFFER,
                                   True,
                                   None};
            int count = 0;
            GLXFBConfig* configs = glXChooseFBConfig(ui, screen, attribs, &count);
            if (configs == nullptr)
                continue;
            GLXFBConfig chosen = nullptr;
            for (int i = 0; i < count && chosen == nullptr; ++i) {
                XVisualInfo* visual = glXGetVisualFromFBConfig(ui, configs[i]);
                if (visual != nullptr) {
                    chosen = configs[i];
                    XFree(visual);
                }
            }
            XFree(configs);
            if (chosen != nullptr)
                return chosen;
        }
        return nullptr;
    }

    bool attach(void* parent_handle) override {
        const Window requested =
            static_cast<Window>(reinterpret_cast<std::uintptr_t>(parent_handle));
        ui = XOpenDisplay(nullptr);
        if (ui == nullptr)
            return false;
        const char* name = DisplayString(ui);
        display_name = name != nullptr ? name : "";
        ErrorRegistry::instance().add(ui);
        parent_gone = false;
        keys_down.clear();
        click_count = 0;
        int glx_event_base = 0;
        int glx_error_base = 0;
        if (!glXQueryExtension(ui, &glx_event_base, &glx_error_base)) {
            detach_window();
            return false;
        }

        XWindowAttributes parent_attributes{};
        if (XGetWindowAttributes(ui, requested, &parent_attributes) == 0 || sync_failed(ui)) {
            detach_window();
            return false;
        }
        parent = requested;
        screen = XScreenNumberOfScreen(parent_attributes.screen);

        GLXFBConfig config = choose_fbconfig();
        if (config == nullptr) {
            detach_window();
            return false;
        }
        glXGetFBConfigAttrib(ui, config, GLX_FBCONFIG_ID, &fbconfig_id);
        XVisualInfo* visual = glXGetVisualFromFBConfig(ui, config);
        if (visual == nullptr) {
            detach_window();
            return false;
        }

        // An explicit colormap and border pixel: without them the window
        // takes its parent's, which does not match this visual's depth.
        colormap = XCreateColormap(ui, parent_attributes.root, visual->visual, AllocNone);
        XSetWindowAttributes window_attributes{};
        window_attributes.colormap = colormap;
        window_attributes.border_pixel = 0;
        window_attributes.background_pixmap = None; // the client paints every pixel
        window_attributes.event_mask = ExposureMask | StructureNotifyMask | VisibilityChangeMask |
                                       ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                                       EnterWindowMask | LeaveWindowMask | KeyPressMask |
                                       KeyReleaseMask | FocusChangeMask;
        pixel_w = static_cast<unsigned>(std::max(1, parent_attributes.width));
        pixel_h = static_cast<unsigned>(std::max(1, parent_attributes.height));
        child = XCreateWindow(
            ui, parent, 0, 0, pixel_w, pixel_h, 0, visual->depth, InputOutput, visual->visual,
            CWColormap | CWBorderPixel | CWBackPixmap | CWEventMask, &window_attributes);
        XFree(visual);
        if (child == 0) {
            detach_window();
            return false;
        }
        // The per-binary class, so two plug-ins' windows are told apart.
        const std::string class_name = windowed_gl_class_name();
        XClassHint hint;
        std::vector<char> class_buf(class_name.begin(), class_name.end());
        class_buf.push_back('\0');
        hint.res_name = class_buf.data();
        hint.res_class = class_buf.data();
        XSetClassHint(ui, child, &hint);
        // Follow the host window: its resizes arrive as ConfigureNotify.
        XSelectInput(ui, parent, StructureNotifyMask);
        XMapWindow(ui, child);

        int xkb_supported = 0;
        XkbSetDetectableAutoRepeat(ui, True, &xkb_supported);

        const char* resources = XResourceManagerString(ui);
        const double from_dpi = resources != nullptr ? scale_from_xft_dpi(resources) : 0.0;
        detected_scale = from_dpi > 0.0 ? from_dpi : 1.0;

        if (sync_failed(ui)) {
            detach_window();
            return false;
        }
        visibility.in_window = true;
        visibility.window_visible = true;
        update_geometry();
        apply_keyboard_focus();
        return true;
    }

    void detach_window() override {
        if (ui == nullptr)
            return;
        release_keyboard();
        if (child != 0 && !parent_gone)
            XDestroyWindow(ui, child);
        if (parent != 0 && !parent_gone)
            XSelectInput(ui, parent, NoEventMask);
        if (colormap != 0)
            XFreeColormap(ui, colormap);
        sync_failed(ui); // errors from a window that is already gone
        ErrorRegistry::instance().remove(ui);
        XCloseDisplay(ui);
        ui = nullptr;
        display_name.clear();
        child = 0;
        parent = 0;
        colormap = 0;
        clear_geometry();
    }

    void forget_window() override {
        // The parent, and with it the child, is gone or going: no request
        // names either, only the connection is closed.
        parent_gone = true;
        detach_window();
    }

    void destroy_window() override {
        detach_window();
    }

    // ── context (render thread) ─────────────────────────────────────────────

    // The framebuffer configuration the window was made with, on the render
    // thread's own connection: identified by its server-side id.
    GLXFBConfig render_fbconfig() {
        const int attribs[] = {GLX_FBCONFIG_ID, fbconfig_id, None};
        int count = 0;
        GLXFBConfig* configs = glXChooseFBConfig(rdpy, screen, attribs, &count);
        if (configs == nullptr)
            return nullptr;
        GLXFBConfig chosen = count > 0 ? configs[0] : nullptr;
        XFree(configs);
        return chosen;
    }

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
    bool accept(GLXContext candidate, const GlSurfaceOptions& opts, std::size_t index,
                GlContextReport& out) {
        GlContextReport report = read_report(opts.requests[index].profile);
        // The window's visual is single-sampled, so a request for more is
        // refused rather than silently given none.
        if (!first_honoured_request({opts.requests[index]}, report.major, report.minor,
                                    report.profile, 0))
            return false;
        report.request_index = index;
        context = candidate;
        out = report;
        apply_interval(opts.swap_interval);
        return true;
    }

    bool apply_interval(int interval) {
        if (auto ext = glx_extension<SwapIntervalExt>("glXSwapIntervalEXT")) {
            ext(rdpy, drawable, interval);
            return !sync_failed(rdpy);
        }
        if (auto mesa = glx_extension<SwapIntervalIntFn>("glXSwapIntervalMESA"))
            return mesa(interval) == 0;
        if (auto sgi = glx_extension<SwapIntervalIntFn>("glXSwapIntervalSGI"))
            return sgi(interval) == 0;
        return false;
    }

    bool create_context(const GlSurfaceOptions& opts, GlContextReport& out) override {
        if (ui == nullptr || child == 0)
            return false;
        rdpy = XOpenDisplay(display_name.c_str());
        if (rdpy == nullptr)
            return false;
        ErrorRegistry::instance().add(rdpy);
        GLXFBConfig config = render_fbconfig();
        if (config == nullptr) {
            drop_render_connection();
            return false;
        }
        // FBConfig-created contexts render to a GLXWindow wrapping the X
        // child, owned by this connection independently of the UI window.
        drawable = glXCreateWindow(rdpy, config, child, nullptr);
        const bool drawable_failed = sync_failed(rdpy);
        if (drawable_failed || drawable == 0) {
            drop_render_connection();
            return false;
        }
        const auto create_attribs =
            glx_extension<CreateContextAttribs>("glXCreateContextAttribsARB");

        for (std::size_t i = 0; i < opts.requests.size(); ++i) {
            const GlContextRequest& request = opts.requests[i];
            if (request.major < 3 || create_attribs == nullptr) {
                // OpenGL 2.1 and earlier: a legacy context is the candidate.
                GLXContext legacy = glXCreateNewContext(rdpy, config, GLX_RGBA_TYPE, nullptr, True);
                if (legacy != nullptr && !sync_failed(rdpy) &&
                    glXMakeCurrent(rdpy, drawable, legacy) && accept(legacy, opts, i, out))
                    return true;
                discard(legacy);
                continue;
            }
            // `any` tries core and then compatibility, so a core-only driver
            // still honours it.
            for (const GlProfile attempt : context_profile_attempts(request.profile)) {
                const bool core = attempt == GlProfile::core;
                const int attribs[] = {
                    kGlxContextMajorVersion,
                    request.major,
                    kGlxContextMinorVersion,
                    request.minor,
                    kGlxContextProfileMask,
                    core ? kGlxContextCoreProfileBit : kGlxContextCompatibilityProfileBit,
                    kGlxContextFlags,
                    request.profile == GlProfile::core ? kGlxContextForwardCompatibleBit : 0,
                    0};
                // A request the server cannot honour raises an X error (the
                // expected fall-through), not a null context alone.
                GLXContext modern = create_attribs(rdpy, config, nullptr, True, attribs);
                if (sync_failed(rdpy) || modern == nullptr) {
                    discard(modern);
                    continue;
                }
                if (glXMakeCurrent(rdpy, drawable, modern) && accept(modern, opts, i, out))
                    return true;
                discard(modern);
            }
        }
        drop_render_connection();
        return false;
    }

    // Unbind and free a context that was not kept.
    void discard(GLXContext candidate) {
        if (candidate == nullptr)
            return;
        glXMakeCurrent(rdpy, None, nullptr);
        glXDestroyContext(rdpy, candidate);
        sync_failed(rdpy);
    }

    bool make_current() override {
        if (context == nullptr || rdpy == nullptr || child == 0)
            return false;
        return glXMakeCurrent(rdpy, drawable, context) == True;
    }

    void release_current() override {
        if (context != nullptr && glXGetCurrentContext() == context)
            glXMakeCurrent(rdpy, None, nullptr);
    }

    bool swap() override {
        if (rdpy == nullptr || glXGetCurrentContext() != context)
            return false;
        glXSwapBuffers(rdpy, drawable);
        return true;
    }

    bool set_interval(int interval) override {
        return rdpy != nullptr && glXGetCurrentContext() == context && apply_interval(interval);
    }

    void* proc(const char* name) const override {
        return reinterpret_cast<void*>(
            glXGetProcAddressARB(reinterpret_cast<const GLubyte*>(name)));
    }

    void drop_render_connection() {
        if (rdpy == nullptr)
            return;
        if (drawable != 0) {
            glXDestroyWindow(rdpy, drawable);
            drawable = 0;
        }
        sync_failed(rdpy);
        ErrorRegistry::instance().remove(rdpy);
        XCloseDisplay(rdpy);
        rdpy = nullptr;
        drawable = 0;
    }

    void destroy_context() override {
        // The client released the context before the release handler returned,
        // so it is current nowhere.
        if (context != nullptr && rdpy != nullptr) {
            glXDestroyContext(rdpy, context);
            sync_failed(rdpy);
        }
        context = nullptr;
        drop_render_connection();
    }

    // ── keyboard ────────────────────────────────────────────────────────────

    void apply_keyboard_focus() override {
        if (ui == nullptr || child == 0 || parent_gone)
            return;
        if (keyboard_focus != KeyboardFocus::none) {
            if (!holding_focus) {
                int revert = 0;
                XGetInputFocus(ui, &previous_focus, &revert);
                holding_focus = true;
            }
            XSetInputFocus(ui, child, RevertToParent, CurrentTime);
        } else {
            release_keyboard();
        }
        sync_failed(ui);
    }

    // Give the keyboard back to whoever had it.
    void release_keyboard() {
        if (!holding_focus)
            return;
        holding_focus = false;
        if (ui == nullptr || parent_gone)
            return;
        Window current = 0;
        int revert = 0;
        XGetInputFocus(ui, &current, &revert);
        // Only restore focus if the child still holds it.
        if (current == child)
            XSetInputFocus(ui, previous_focus != child ? previous_focus : PointerRoot,
                           RevertToParent, CurrentTime);
    }

    // Hand an event the client declined to the host window.
    void forward_to_host(XEvent event, long mask) {
        if (ui == nullptr || parent_gone || parent == 0)
            return;
        event.xany.window = parent;
        event.xany.send_event = True;
        XSendEvent(ui, parent, False, mask, &event);
    }

    // ── events ──────────────────────────────────────────────────────────────

    void dispatch_events() override {
        while (ui != nullptr) {
            XEvent event;
            // Reading may deliver the errors of earlier requests; the registry
            // contains them. The handlers run after, so they can call back
            // into the surface.
            if (XPending(ui) <= 0)
                break;
            XNextEvent(ui, &event);
            handle(event);
        }
    }

    void handle(const XEvent& event) {
        switch (event.type) {
        case ConfigureNotify:
            if (event.xconfigure.window == parent)
                on_parent_resized(static_cast<unsigned>(std::max(1, event.xconfigure.width)),
                                  static_cast<unsigned>(std::max(1, event.xconfigure.height)));
            else if (event.xconfigure.window == child)
                on_child_resized(static_cast<unsigned>(std::max(1, event.xconfigure.width)),
                                 static_cast<unsigned>(std::max(1, event.xconfigure.height)));
            break;
        case DestroyNotify:
            if (event.xdestroywindow.window == parent || event.xdestroywindow.window == child)
                on_window_destroyed();
            break;
        case VisibilityNotify:
            if (event.xvisibility.window == child)
                on_visibility(event.xvisibility.state);
            break;
        case MapNotify:
            if (event.xmap.window == child)
                on_map(true);
            break;
        case UnmapNotify:
            if (event.xunmap.window == child)
                on_map(false);
            break;
        case ButtonPress:
        case ButtonRelease:
            if (event.xbutton.window == child)
                on_button(event.xbutton);
            break;
        case MotionNotify:
            if (event.xmotion.window == child)
                on_motion(event.xmotion);
            break;
        case EnterNotify:
        case LeaveNotify:
            if (event.xcrossing.window == child && event.xcrossing.mode == NotifyNormal)
                on_crossing(event.xcrossing);
            break;
        case FocusIn:
        case FocusOut:
            // A key released while the child lacked focus sent its release
            // elsewhere. Forget such keys when focus returns, so the next
            // press is a new press, not a repeat with a stale routing. A
            // release after focus left is still kept (it goes where the
            // press went).
            if (event.type == FocusIn && event.xfocus.window == child)
                keys_down.clear();
            if (event.xfocus.window == child && event.xfocus.mode != NotifyGrab &&
                event.xfocus.mode != NotifyUngrab && input.focus) {
                FocusEvent focus;
                focus.focused = event.type == FocusIn;
                focus.reason = FocusReason::responder;
                input.focus(focus);
            }
            break;
        case KeyPress:
        case KeyRelease:
            if (event.xkey.window == child)
                on_key(event);
            break;
        default:
            break;
        }
    }

    void on_parent_resized(unsigned w, unsigned h) {
        if (child == 0 || (w == pixel_w && h == pixel_h))
            return;
        // The child fills the parent; its own ConfigureNotify then finds the
        // size already recorded.
        pixel_w = w;
        pixel_h = h;
        XResizeWindow(ui, child, w, h);
        update_geometry();
        emit(SurfaceEventKind::geometry_changed);
    }

    void on_child_resized(unsigned w, unsigned h) {
        if (w == pixel_w && h == pixel_h)
            return;
        pixel_w = w;
        pixel_h = h;
        update_geometry();
        emit(SurfaceEventKind::geometry_changed);
    }

    void on_window_destroyed() {
        if (parent_gone)
            return;
        parent_gone = true;
        visibility = {};
        clear_geometry();
        emit(SurfaceEventKind::window_changed);
    }

    void on_visibility(int state) {
        OnScreenState next = visibility;
        next.in_window = !parent_gone;
        next.occluded = state == VisibilityFullyObscured;
        if (next == visibility)
            return;
        visibility = next;
        emit(SurfaceEventKind::visibility_changed);
    }

    void on_map(bool mapped) {
        OnScreenState next = visibility;
        next.in_window = !parent_gone;
        next.window_visible = mapped;
        next.minimized = !mapped;
        if (next == visibility)
            return;
        visibility = next;
        emit(SurfaceEventKind::visibility_changed);
    }

    PointerEvent pointer_at(int x, int y, unsigned state, Time time) const {
        PointerEvent e;
        const double scale = current_scale();
        e.x = x / scale;
        e.y = y / scale;
        e.modifiers = modifiers_from_x11_state(state);
        e.buttons_held = buttons_held_from_x11_state(state);
        e.timestamp = time / 1000.0;
        return e;
    }

    void on_button(const XButtonEvent& button) {
        double dx = 0.0;
        double dy = 0.0;
        if (wheel_delta_for_x11_button(button.button, dx, dy)) {
            if (button.type != ButtonPress)
                return;
            if (!input.wheel) {
                // No wheel handler: the host's scroll view keeps working.
                XEvent copy{};
                copy.xbutton = button;
                forward_to_host(copy, ButtonPressMask);
                return;
            }
            WheelEvent wheel;
            const double scale = current_scale();
            wheel.x = button.x / scale;
            wheel.y = button.y / scale;
            wheel.dx = dx;
            wheel.dy = dy;
            wheel.precise = false;
            wheel.raw_dx = dx;
            wheel.raw_dy = dy;
            wheel.modifiers = modifiers_from_x11_state(button.state);
            wheel.timestamp = button.time / 1000.0;
            input.wheel(wheel);
            return;
        }
        if (!input.pointer)
            return;
        PointerEvent e = pointer_at(button.x, button.y, button.state, button.time);
        e.kind = button.type == ButtonPress ? PointerKind::down : PointerKind::up;
        e.button = pointer_button_for_x11(button.button, e.button_number);
        const std::uint32_t bit =
            e.button_number >= 0 && e.button_number < 32 ? 1u << e.button_number : 0u;
        // The state is from before the event: add the press, remove the release.
        e.buttons_held =
            e.kind == PointerKind::down ? (e.buttons_held | bit) : (e.buttons_held & ~bit);
        if (e.kind == PointerKind::down) {
            const bool repeat = button.button == last_press_button &&
                                button.time - last_press_time <= kDoubleClickMs &&
                                std::abs(button.x - last_press_x) <= kDoubleClickSlop &&
                                std::abs(button.y - last_press_y) <= kDoubleClickSlop;
            click_count = repeat ? click_count + 1 : 1;
            last_press_time = button.time;
            last_press_button = button.button;
            last_press_x = button.x;
            last_press_y = button.y;
        }
        e.click_count = click_count;
        input.pointer(e);
    }

    void on_motion(const XMotionEvent& motion) {
        if (!input.pointer)
            return;
        PointerEvent e = pointer_at(motion.x, motion.y, motion.state, motion.time);
        if (e.buttons_held == 0) {
            e.kind = PointerKind::move;
        } else {
            e.kind = PointerKind::drag;
            // The lowest button held names the drag.
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

    void on_crossing(const XCrossingEvent& crossing) {
        if (!input.pointer)
            return;
        PointerEvent e = pointer_at(crossing.x, crossing.y, crossing.state, crossing.time);
        e.kind = crossing.type == EnterNotify ? PointerKind::enter : PointerKind::leave;
        input.pointer(e);
    }

    // The text a key press produced, UTF-8: ASCII and Latin-1 only (no input
    // method, and a plug-in cannot set the locale).
    static std::string key_characters(XKeyEvent key) {
        if (key.state & (ControlMask | Mod1Mask | Mod4Mask))
            return {};
        char buffer[16];
        KeySym symbol = NoSymbol;
        const int n = XLookupString(&key, buffer, sizeof buffer, &symbol, nullptr);
        std::string out;
        if (n > 0 && static_cast<unsigned char>(buffer[0]) >= 0x20 &&
            static_cast<unsigned char>(buffer[0]) < 0x7f)
            out.assign(buffer, static_cast<std::size_t>(n));
        else if (symbol >= 0xa0 && symbol <= 0xff)
            append_utf8(out, static_cast<std::uint32_t>(symbol));
        return out;
    }

    void on_key(const XEvent& event) {
        const XKeyEvent& xk = event.xkey;
        const bool down = xk.type == KeyPress;
        KeyEvent key;
        key.kind = down ? KeyKind::down : KeyKind::up;
        key.key = key_for_x11_keysym(static_cast<std::uint32_t>(
            XkbKeycodeToKeysym(ui, static_cast<KeyCode>(xk.keycode), 0, 0)));
        key.native_code = static_cast<std::uint16_t>(xk.keycode);
        key.modifiers = modifiers_from_x11_state(xk.state);
        key.timestamp = xk.time / 1000.0;
        // Whether the client goes first is decided at the first press and kept
        // for its repeats and its release, even when focus moves while the key
        // is held.
        const auto known = keys_down.find(xk.keycode);
        key.repeat = down && known != keys_down.end();
        bool client_first;
        if (known != keys_down.end()) {
            client_first = known->second;
            if (!down)
                keys_down.erase(known);
        } else {
            client_first = down && route_key(key, keyboard_focus) == KeyRoute::client_first;
            if (down)
                keys_down[xk.keycode] = client_first;
        }
        if (down)
            key.characters = key_characters(xk);

        bool taken = false;
        if (client_first) {
            taken = input.key && input.key(key);
            if (!taken && down && keyboard_focus == KeyboardFocus::text &&
                !key.characters.empty() && input.text) {
                input.text(key.characters);
                taken = true;
            }
        }
        if (!taken)
            forward_to_host(event, down ? KeyPressMask : KeyReleaseMask);
    }

    static constexpr Time kDoubleClickMs = 400;
    static constexpr int kDoubleClickSlop = 4;
};

} // namespace

std::unique_ptr<GlSurface::Impl> make_windowed_gl_impl(const GlSurfaceOptions& options) {
    auto impl = std::make_unique<X11Window>();
    impl->options = options;
    return impl;
}

std::unique_ptr<GlSurface::Impl> make_offscreen_gl_impl(std::uint32_t, std::uint32_t,
                                                        const GlSurfaceOptions&) {
    // Offscreen GL on X11 needs a pbuffer or a hidden window; not offered yet.
    return nullptr;
}

std::string windowed_gl_class_name() {
    return pulp::platform::binary_unique_name("PulpGlSurface");
}

} // namespace pulp::native
