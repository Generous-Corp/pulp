// RmlUi on a pulp-native Metal surface. Compiled with ARC.

#include <pulp/rmlui_native/rmlui_surface.hpp>

#include <vellum/rmlui/native.hpp>

#include <RmlUi/Core.h>

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <utility>

#if !__has_feature(objc_arc)
#error "rmlui_surface_mac.mm must be compiled with -fobjc-arc"
#endif

namespace pulp::rmlui_native {

using namespace ::pulp::native;
namespace KI = ::Rml::Input;

namespace {

std::atomic<std::size_t> g_live{0};

/// The surface whose pointer event RmlUi is handling, for SetMouseCursor.
/// Read and written with rmlui_mutex() held.
MetalSurface* g_pointer_surface = nullptr;

// The host side of RmlUi's system interface: a steady clock, the system
// pasteboard, and cursors on whichever surface is under the pointer.
class SurfaceSystemInterface final : public Rml::SystemInterface {
  public:
    double GetElapsedTime() override {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }
    void SetMouseCursor(const Rml::String& name) override {
        if (g_pointer_surface)
            g_pointer_surface->set_cursor(cursor_for_name(name));
    }
    void SetClipboardText(const Rml::String& text) override {
        NSPasteboard* board = [NSPasteboard generalPasteboard];
        [board clearContents];
        [board setString:@(text.c_str()) forType:NSPasteboardTypeString];
    }
    void GetClipboardText(Rml::String& text) override {
        NSString* s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        text = s ? Rml::String(s.UTF8String) : Rml::String();
    }

  private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

SurfaceSystemInterface& system_interface() {
    static SurfaceSystemInterface instance;
    return instance;
}

int mouse_button(const PointerEvent& e) {
    switch (e.button) {
    case PointerButton::left:
        return 0;
    case PointerButton::right:
        return 1;
    case PointerButton::middle:
        return 2;
    case PointerButton::other:
        return e.button_number;
    case PointerButton::none:
        break;
    }
    return -1;
}

bool takes_text(Rml::Element* element) {
    if (!element)
        return false;
    const auto& tag = element->GetTagName();
    if (tag == "textarea")
        return true;
    if (tag != "input")
        return false;
    const auto type = element->GetAttribute<Rml::String>("type", "text");
    return type == "text" || type == "password" || type == "search" || type == "email" ||
           type == "url" || type == "number";
}

} // namespace

std::mutex& rmlui_mutex() {
    static std::mutex m;
    return m;
}

struct RmlUiSurface::Impl {
    vellum::rmlui::AssetProvider& assets;
    Rml::RenderInterface& renderer;
    RendererHooks hooks;
    RmlUiSurfaceOptions options;
    std::unique_ptr<MetalSurface> surface;
    std::unique_ptr<FramePump> pump;
    std::unique_ptr<vellum::rmlui::NativeContext> context;
    std::atomic<std::uint64_t> frames{0};
    std::uint32_t width_px = 0;
    std::uint32_t height_px = 0;
    double scale = 1.0;
    bool text_focus = false;

    Impl(vellum::rmlui::AssetProvider& a, Rml::RenderInterface& r, RendererHooks h,
         RmlUiSurfaceOptions o)
        : assets(a), renderer(r), hooks(std::move(h)), options(std::move(o)) {}

    // Main thread, rmlui_mutex() held: the context follows the surface size.
    void sync_size_locked() {
        const auto g = surface->geometry();
        const double s = g.scale > 0.0 ? g.scale : 1.0;
        const auto w = static_cast<std::uint32_t>(std::lround(g.width * s));
        const auto h = static_cast<std::uint32_t>(std::lround(g.height * s));
        if (w == 0 || h == 0 || (w == width_px && h == height_px && s == scale))
            return;
        width_px = w;
        height_px = h;
        scale = s;
        context->set_dimensions(w, h);
        if (auto* ctx = context->context())
            ctx->SetDensityIndependentPixelRatio(static_cast<float>(s));
    }

    // Main thread, rmlui_mutex() held: hold the host's keyboard only while an
    // RmlUi text field has focus.
    void sync_text_focus_locked() {
        auto* ctx = context ? context->context() : nullptr;
        const bool wants = ctx && takes_text(ctx->GetFocusElement());
        if (wants != text_focus) {
            text_focus = wants;
            surface->set_text_focus(wants);
        }
    }

    // Render thread.
    void render_frame() {
        @autoreleasepool {
            auto drawable = surface->acquire_drawable();
            if (!drawable)
                return;
            id<MTLCommandBuffer> cb =
                [(__bridge id<MTLCommandQueue>)surface->command_queue() commandBuffer];
            {
                std::lock_guard lock(rmlui_mutex());
                if (!context || !context->is_open())
                    return;
                FrameTarget target{(__bridge void*)cb, drawable.texture(), drawable.width(),
                                   drawable.height(), scale};
                if (hooks.begin_frame)
                    hooks.begin_frame(target);
                context->update();
                context->render();
                if (hooks.end_frame)
                    hooks.end_frame();
            }
            drawable.present((__bridge void*)cb);
            [cb commit];
            frames.fetch_add(1);
        }
    }

    void on_pointer(const PointerEvent& e) {
        {
            std::lock_guard lock(rmlui_mutex());
            auto* ctx = context ? context->context() : nullptr;
            if (!ctx)
                return;
            vellum::rmlui::NativeContext::Scope scope(*context);
            g_pointer_surface = surface.get();
            const int mods = rml_modifiers_for(e.modifiers);
            const int x = static_cast<int>(std::lround(e.x * scale));
            const int y = static_cast<int>(std::lround(e.y * scale));
            switch (e.kind) {
            case PointerKind::move:
            case PointerKind::drag:
            case PointerKind::enter:
                ctx->ProcessMouseMove(x, y, mods);
                break;
            case PointerKind::down:
                ctx->ProcessMouseMove(x, y, mods);
                if (const int b = mouse_button(e); b >= 0)
                    ctx->ProcessMouseButtonDown(b, mods);
                break;
            case PointerKind::up:
                ctx->ProcessMouseMove(x, y, mods);
                if (const int b = mouse_button(e); b >= 0)
                    ctx->ProcessMouseButtonUp(b, mods);
                break;
            case PointerKind::leave:
                ctx->ProcessMouseLeave();
                break;
            }
            g_pointer_surface = nullptr;
            sync_text_focus_locked();
        }
        pump->request_frame();
    }

    void on_wheel(const WheelEvent& e) {
        {
            std::lock_guard lock(rmlui_mutex());
            auto* ctx = context ? context->context() : nullptr;
            if (!ctx)
                return;
            vellum::rmlui::NativeContext::Scope scope(*context);
            // RmlUi's delta is in lines, positive scrolling the content up;
            // AppKit's is positive moving the content down, in points for a
            // trackpad.
            const double line = e.precise ? 16.0 : 1.0;
            ctx->ProcessMouseWheel(
                Rml::Vector2f(static_cast<float>(-e.dx / line), static_cast<float>(-e.dy / line)),
                rml_modifiers_for(e.modifiers));
        }
        pump->request_frame();
    }

    bool on_key(const KeyEvent& e) {
        bool consumed = false;
        {
            std::lock_guard lock(rmlui_mutex());
            auto* ctx = context ? context->context() : nullptr;
            const int key = rml_key_for(e.key);
            if (!ctx || key == KI::KI_UNKNOWN)
                return false;
            vellum::rmlui::NativeContext::Scope scope(*context);
            const auto ki = static_cast<KI::KeyIdentifier>(key);
            const int mods = rml_modifiers_for(e.modifiers);
            // ProcessKey* returns true when the event propagated unconsumed.
            consumed = e.kind == KeyKind::down ? !ctx->ProcessKeyDown(ki, mods)
                                               : !ctx->ProcessKeyUp(ki, mods);
            sync_text_focus_locked();
        }
        pump->request_frame();
        return consumed;
    }

    void on_text(const std::string& text) {
        {
            std::lock_guard lock(rmlui_mutex());
            auto* ctx = context ? context->context() : nullptr;
            if (!ctx)
                return;
            vellum::rmlui::NativeContext::Scope scope(*context);
            ctx->ProcessTextInput(Rml::String(text));
        }
        pump->request_frame();
    }

    void teardown(bool orderly) {
        if (surface) {
            if (orderly)
                surface->detach(); // stops the pump through the release handler
            else
                surface->invalidate();
        }
        if (pump)
            pump->stop();
        {
            std::lock_guard lock(rmlui_mutex());
            if (context)
                context->close();
            context.reset();
        }
        pump.reset();
        surface.reset();
        text_focus = false;
        width_px = height_px = 0;
    }
};

RmlUiSurface::RmlUiSurface(vellum::rmlui::AssetProvider& assets, Rml::RenderInterface& renderer,
                           RendererHooks hooks, RmlUiSurfaceOptions options)
    : impl_(std::make_unique<Impl>(assets, renderer, std::move(hooks), std::move(options))) {
    g_live.fetch_add(1);
}

RmlUiSurface::~RmlUiSurface() {
    impl_->teardown(true);
    g_live.fetch_sub(1);
}

bool RmlUiSurface::attach(void* parent) {
    auto& d = *impl_;
    if (!parent || d.surface)
        return false;
    MetalSurfaceOptions surface_options;
    surface_options.device = d.options.device;
    surface_options.opaque = d.options.opaque;
    d.surface = MetalSurface::create(surface_options);
    if (!d.surface)
        return false;
    d.pump = std::make_unique<FramePump>(
        [impl = impl_.get()](const FrameTick&) { impl->render_frame(); },
        FramePumpOptions{d.options.rate_cap_hz, false, d.options.thread});
    d.surface->set_release_handler([impl = impl_.get()] {
        if (impl->pump)
            impl->pump->stop();
    });
    InputHandler input;
    input.pointer = [impl = impl_.get()](const PointerEvent& e) { impl->on_pointer(e); };
    input.wheel = [impl = impl_.get()](const WheelEvent& e) { impl->on_wheel(e); };
    input.key = [impl = impl_.get()](const KeyEvent& e) { return impl->on_key(e); };
    input.text = [impl = impl_.get()](const std::string& t) { impl->on_text(t); };
    d.surface->set_input_handler(std::move(input));
    if (!d.surface->attach(parent)) {
        impl_->teardown(false);
        return false;
    }
    {
        std::lock_guard lock(rmlui_mutex());
        // Native captures the system interface when its first instance opens.
        if (vellum::rmlui::NativeContext::open_instances() == 0)
            Rml::SetSystemInterface(&system_interface());
        d.context = std::make_unique<vellum::rmlui::NativeContext>(d.assets, d.renderer);
        const auto g = d.surface->geometry();
        const double s = g.scale > 0.0 ? g.scale : 1.0;
        const auto w =
            std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(g.width * s)));
        const auto h =
            std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(g.height * s)));
        bool ok = d.context->open(w, h) == vellum::rmlui::Status::ok;
        if (ok) {
            vellum::rmlui::NativeContext::Scope scope(*d.context);
            for (std::size_t i = 0; i < d.options.font_faces.size() && ok; ++i)
                ok = Rml::LoadFontFace(d.options.font_faces[i], i == 0);
        }
        if (!ok || d.context->load_document(d.options.document) != vellum::rmlui::Status::ok) {
            d.context.reset();
        } else {
            d.width_px = w;
            d.height_px = h;
            d.scale = s;
            d.context->context()->SetDensityIndependentPixelRatio(static_cast<float>(s));
        }
    }
    if (!d.context) {
        impl_->teardown(true);
        return false;
    }
    d.pump->start();
    d.pump->request_frame();
    return true;
}

void RmlUiSurface::detach() {
    impl_->teardown(true);
}

void RmlUiSurface::invalidate() {
    impl_->teardown(false);
}

bool RmlUiSurface::attached() const noexcept {
    return impl_->surface && impl_->surface->attached();
}

void RmlUiSurface::idle() {
    auto& d = *impl_;
    if (!d.surface || !d.pump)
        return;
    d.surface->poll_visibility();
    d.pump->set_on_screen(d.surface->on_screen().on_screen());
    bool resized = false;
    {
        std::lock_guard lock(rmlui_mutex());
        if (d.context) {
            const auto before = std::make_pair(d.width_px, d.height_px);
            d.sync_size_locked();
            resized = before != std::make_pair(d.width_px, d.height_px);
        }
    }
    if (resized)
        d.pump->request_frame();
}

void RmlUiSurface::request_frame() {
    if (impl_->pump)
        impl_->pump->request_frame();
}

bool RmlUiSurface::with_context(const std::function<void(Rml::Context&)>& fn) {
    std::lock_guard lock(rmlui_mutex());
    auto& d = *impl_;
    auto* ctx = d.context ? d.context->context() : nullptr;
    if (!ctx)
        return false;
    vellum::rmlui::NativeContext::Scope scope(*d.context);
    fn(*ctx);
    // The caller may have moved focus (Element::Focus()).
    d.sync_text_focus_locked();
    return true;
}

MetalSurface* RmlUiSurface::surface() noexcept {
    return impl_->surface.get();
}

std::uint64_t RmlUiSurface::frames() const noexcept {
    return impl_->frames.load();
}

std::size_t RmlUiSurface::live() noexcept {
    return g_live.load();
}

// ── Translation tables ───────────────────────────────────────────────────────

int rml_key_for(Key key) noexcept {
    const auto k = static_cast<int>(key);
    auto in = [k](Key first, Key last) {
        return k >= static_cast<int>(first) && k <= static_cast<int>(last);
    };
    if (in(Key::a, Key::z))
        return KI::KI_A + (k - static_cast<int>(Key::a));
    if (in(Key::digit_0, Key::digit_9))
        return KI::KI_0 + (k - static_cast<int>(Key::digit_0));
    if (in(Key::f1, Key::f24))
        return KI::KI_F1 + (k - static_cast<int>(Key::f1));
    if (in(Key::keypad_0, Key::keypad_9))
        return KI::KI_NUMPAD0 + (k - static_cast<int>(Key::keypad_0));
    switch (key) {
    case Key::space:
        return KI::KI_SPACE;
    case Key::enter:
        return KI::KI_RETURN;
    case Key::tab:
        return KI::KI_TAB;
    case Key::backspace:
        return KI::KI_BACK;
    case Key::escape:
        return KI::KI_ESCAPE;
    case Key::delete_forward:
        return KI::KI_DELETE;
    case Key::left:
        return KI::KI_LEFT;
    case Key::right:
        return KI::KI_RIGHT;
    case Key::up:
        return KI::KI_UP;
    case Key::down:
        return KI::KI_DOWN;
    case Key::home:
        return KI::KI_HOME;
    case Key::end:
        return KI::KI_END;
    case Key::page_up:
        return KI::KI_PRIOR;
    case Key::page_down:
        return KI::KI_NEXT;
    case Key::help:
        return KI::KI_HELP;
    case Key::keypad_decimal:
        return KI::KI_DECIMAL;
    case Key::keypad_plus:
        return KI::KI_ADD;
    case Key::keypad_minus:
        return KI::KI_SUBTRACT;
    case Key::keypad_multiply:
        return KI::KI_MULTIPLY;
    case Key::keypad_divide:
        return KI::KI_DIVIDE;
    case Key::keypad_equals:
        return KI::KI_OEM_NEC_EQUAL;
    case Key::keypad_enter:
        return KI::KI_NUMPADENTER;
    case Key::keypad_clear:
        return KI::KI_CLEAR;
    case Key::minus:
        return KI::KI_OEM_MINUS;
    case Key::equal:
        return KI::KI_OEM_PLUS;
    case Key::left_bracket:
        return KI::KI_OEM_4;
    case Key::right_bracket:
        return KI::KI_OEM_6;
    case Key::backslash:
        return KI::KI_OEM_5;
    case Key::semicolon:
        return KI::KI_OEM_1;
    case Key::quote:
        return KI::KI_OEM_7;
    case Key::grave:
        return KI::KI_OEM_3;
    case Key::comma:
        return KI::KI_OEM_COMMA;
    case Key::period:
        return KI::KI_OEM_PERIOD;
    case Key::slash:
        return KI::KI_OEM_2;
    case Key::shift_left:
        return KI::KI_LSHIFT;
    case Key::shift_right:
        return KI::KI_RSHIFT;
    case Key::control_left:
        return KI::KI_LCONTROL;
    case Key::control_right:
        return KI::KI_RCONTROL;
    case Key::option_left:
        return KI::KI_LMENU;
    case Key::option_right:
        return KI::KI_RMENU;
    case Key::command_left:
        return KI::KI_LMETA;
    case Key::command_right:
        return KI::KI_RMETA;
    case Key::caps_lock:
        return KI::KI_CAPITAL;
    case Key::context_menu:
        return KI::KI_APPS;
    case Key::volume_up:
        return KI::KI_VOLUME_UP;
    case Key::volume_down:
        return KI::KI_VOLUME_DOWN;
    case Key::mute:
        return KI::KI_VOLUME_MUTE;
    default:
        return KI::KI_UNKNOWN;
    }
}

int rml_modifiers_for(std::uint32_t m) noexcept {
    int out = 0;
    if (m & modifier_control)
        out |= KI::KM_CTRL;
    if (m & modifier_shift)
        out |= KI::KM_SHIFT;
    if (m & modifier_option)
        out |= KI::KM_ALT;
    if (m & modifier_command)
        out |= KI::KM_META;
    if (m & modifier_caps_lock)
        out |= KI::KM_CAPSLOCK;
    return out;
}

Cursor cursor_for_name(const std::string& name) noexcept {
    if (name == "pointer")
        return Cursor::pointing_hand;
    if (name == "text")
        return Cursor::ibeam;
    if (name == "move" || name == "grab")
        return Cursor::open_hand;
    if (name == "grabbing")
        return Cursor::closed_hand;
    if (name == "crosshair")
        return Cursor::crosshair;
    if (name == "wait" || name == "progress")
        return Cursor::busy;
    if (name == "ew-resize" || name == "col-resize")
        return Cursor::resize_horizontal;
    if (name == "ns-resize" || name == "row-resize")
        return Cursor::resize_vertical;
    if (name == "nesw-resize")
        return Cursor::resize_diagonal_nesw;
    if (name == "nwse-resize")
        return Cursor::resize_diagonal_nwse;
    if (name == "not-allowed")
        return Cursor::not_allowed;
    return Cursor::arrow;
}

} // namespace pulp::rmlui_native
