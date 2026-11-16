// test_rmlui_native_surface.mm: RmlUi on a pulp-native surface. The document
// loads and renders through the caller's renderer, pointer, key and text input
// reach RmlUi elements, the context follows the surface size, and RmlUi is
// never entered by two threads at once. A counting Rml::RenderInterface stands
// in for a real renderer. Skips, saying so, without a Metal device. Compiled
// with ARC.

#include <catch2/catch_test_macros.hpp>

#include <pulp/rmlui_native/rmlui_surface.hpp>

#include <vellum/rmlui/assets.hpp>
#include <vellum/rmlui/native.hpp>

#include <RmlUi/Core.h>

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace pulp::rmlui_native;

/// Forwards every message to a real MTLDevice under a distinct identity, so a
/// test can tell "the device it passed" from the system default.
@interface PulpTestDeviceProxy : NSProxy
- (instancetype)initWithDevice:(id<MTLDevice>)device;
@end
@implementation PulpTestDeviceProxy {
    id<MTLDevice> _device;
}
- (instancetype)initWithDevice:(id<MTLDevice>)device {
    _device = device;
    return self;
}
- (NSMethodSignature*)methodSignatureForSelector:(SEL)sel {
    return [(NSObject*)_device methodSignatureForSelector:sel];
}
- (void)forwardInvocation:(NSInvocation*)invocation {
    [invocation invokeWithTarget:_device];
}
- (BOOL)conformsToProtocol:(Protocol*)protocol {
    return [(NSObject*)_device conformsToProtocol:protocol];
}
@end

namespace {

// Lengths are in dp: the surface sets RmlUi's density ratio to the backing
// scale, so a dp is one logical point and lines up with pointer positions.
constexpr const char* kDocument = R"(<rml>
<head>
<style>
body { width: 100%; height: 100%; margin: 0; font-family: Arial; font-size: 14dp; }
#button { position: absolute; left: 0; top: 0; width: 100dp; height: 50dp;
          background-color: #ff0000; cursor: pointer; }
#button:hover { background-color: #00ff00; }
#field { display: block; position: absolute; left: 0; top: 100dp; width: 200dp; height: 30dp;
         background-color: #333333; }
</style>
</head>
<body>
<div id="button"></div>
<input id="field" type="text" value="ab"/>
</body>
</rml>)";

class MemoryAssets final : public vellum::rmlui::AssetProvider {
  public:
    vellum::rmlui::Status load(std::string_view path, vellum::rmlui::Asset& out) override {
        out.path = std::string(path);
        if (path == "probe.rml") {
            out.bytes = kDocument;
            return vellum::rmlui::Status::ok;
        }
        if (path == "font.ttf") {
            // A system face, so a text field has a font to measure with.
            NSData* data =
                [NSData dataWithContentsOfFile:@"/System/Library/Fonts/Supplemental/Arial.ttf"];
            if (!data)
                return vellum::rmlui::Status::missing_asset;
            out.bytes.assign(static_cast<const char*>(data.bytes), data.length);
            return vellum::rmlui::Status::ok;
        }
        return vellum::rmlui::Status::missing_asset;
    }
};

/// Counts what RmlUi asks of a renderer, and how many threads are inside it.
class CountingRenderer final : public Rml::RenderInterface {
  public:
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>,
                                                Rml::Span<const int>) override {
        enter();
        compiled.fetch_add(1);
        leave();
        return static_cast<Rml::CompiledGeometryHandle>(compiled.load());
    }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {
        enter();
        rendered.fetch_add(1);
        // Long enough that two unserialized threads would overlap here.
        std::this_thread::sleep_for(std::chrono::microseconds(draw_delay_us.load()));
        // A test can park the render thread inside a draw, lock held.
        if (hold_draws.load()) {
            parked.store(true);
            while (hold_draws.load())
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            parked.store(false);
        }
        leave();
    }
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i&, const Rml::String&) override {
        return 0;
    }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override {
        return 1;
    }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}

    static inline std::atomic<int> inside{0};
    static inline std::atomic<int> max_inside{0};
    std::atomic<int> compiled{0};
    std::atomic<int> rendered{0};
    std::atomic<int> draw_delay_us{200};
    std::atomic<bool> hold_draws{false};
    std::atomic<bool> parked{false};

  private:
    static void enter() {
        const int now = inside.fetch_add(1) + 1;
        int seen = max_inside.load();
        while (now > seen && !max_inside.compare_exchange_weak(seen, now)) {
        }
    }
    static void leave() {
        inside.fetch_sub(1);
    }
};

long long now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

double percentile_ms(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[static_cast<std::size_t>(p * (v.size() - 1))];
}

struct Editor {
    NSWindow* window = nil;
    MemoryAssets assets;
    CountingRenderer renderer;
    std::atomic<int> begun{0};
    /// What the renderer had compiled when the first frame began.
    std::atomic<int> compiled_at_first_begin{-1};
    std::atomic<int> ended{0};
    /// Longest span between a frame's begin and end hooks, which run with the
    /// RmlUi lock held: how long a frame keeps everyone else out.
    std::atomic<long long> max_hold_ns{0};
    std::atomic<long long> begin_ns{0};
    FrameTarget last_target;
    std::unique_ptr<RmlUiSurface> ui;

    explicit Editor(NSRect frame = NSMakeRect(0, 0, 300, 200)) {
        [NSApplication sharedApplication];
        window = [[NSWindow alloc] initWithContentRect:frame
                                             styleMask:NSWindowStyleMaskBorderless
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
        window.releasedWhenClosed = NO;
        RendererHooks hooks;
        hooks.begin_frame = [this](const FrameTarget& t) {
            last_target = t;
            if (begun.fetch_add(1) == 0)
                compiled_at_first_begin.store(renderer.compiled.load());
            begin_ns.store(now_ns());
        };
        hooks.end_frame = [this] {
            const long long hold = now_ns() - begin_ns.load();
            long long seen = max_hold_ns.load();
            while (hold > seen && !max_hold_ns.compare_exchange_weak(seen, hold)) {
            }
            ended.fetch_add(1);
        };
        RmlUiSurfaceOptions options;
        options.document = "probe.rml";
        options.font_faces = {"font.ttf"};
        options.rate_cap_hz = 120.0;
        ui = std::make_unique<RmlUiSurface>(assets, renderer, hooks, options);
    }
    ~Editor() {
        ui.reset();
        [window close];
    }
    bool attach() {
        return ui->attach((__bridge void*)window.contentView);
    }
    NSView* view() {
        return (__bridge NSView*)ui->surface()->view();
    }
    NSEvent* mouse(NSEventType type, double x, double y) const {
        return [NSEvent mouseEventWithType:type
                                  location:NSMakePoint(x, window.contentView.bounds.size.height - y)
                             modifierFlags:0
                                 timestamp:1.0
                              windowNumber:window.windowNumber
                                   context:nil
                               eventNumber:0
                                clickCount:1
                                  pressure:1.0];
    }
    template <class Pred> bool wait_for(Pred done, double seconds = 5.0) {
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (!done()) {
            if (std::chrono::steady_clock::now() > until)
                return false;
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        }
        return true;
    }
};

bool metal_available() {
    return MTLCreateSystemDefaultDevice() != nil;
}

#define REQUIRE_METAL()                                                                            \
    do {                                                                                           \
        if (!metal_available())                                                                    \
            SKIP("no Metal device on this host; RmlUi on the native surface was not exercised");   \
    } while (0)

} // namespace

TEST_CASE("the document loads and renders through the caller's renderer",
          "[native-surface][rmlui]") {
    REQUIRE_METAL();
    @autoreleasepool {
        Editor e;
        const auto instances = vellum::rmlui::NativeContext::open_instances();
        REQUIRE(e.attach());
        REQUIRE(vellum::rmlui::NativeContext::open_instances() == instances + 1);
        REQUIRE(e.wait_for([&] { return e.ui->frames() > 0; }));
        REQUIRE(e.renderer.compiled.load() > 0);
        REQUIRE(e.renderer.rendered.load() > 0);
        REQUIRE(e.begun.load() == e.ended.load());
        REQUIRE(e.last_target.texture != nullptr);
        REQUIRE(e.last_target.command_buffer != nullptr);
        REQUIRE(e.last_target.width ==
                static_cast<std::uint32_t>(300 * e.window.backingScaleFactor));
        e.ui->detach();
        REQUIRE(vellum::rmlui::NativeContext::open_instances() == instances);
    }
}

TEST_CASE("the document's first frame is prepared before any frame is drawn",
          "[native-surface][rmlui][first-frame]") {
    REQUIRE_METAL();
#if defined(PULP_VELLUM_RMLUI_HAS_WARM)
    @autoreleasepool {
        Editor e;
        REQUIRE(e.attach());
        REQUIRE(e.wait_for([&] { return e.begun.load() > 0; }));
        // begin_frame runs before RmlUi updates, so geometry compiled by then
        // was compiled while the document was prepared, not by the first
        // frame's own update.
        REQUIRE(e.compiled_at_first_begin.load() > 0);
        e.ui->detach();
    }
#else
    SKIP("the installed Vellum RmlUi SDK has no NativeContext::warm()");
#endif
}

TEST_CASE("a missing document or font fails attach and leaves nothing behind",
          "[native-surface][rmlui]") {
    REQUIRE_METAL();
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 100, 100)
                                                       styleMask:NSWindowStyleMaskBorderless
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.releasedWhenClosed = NO;
        MemoryAssets assets;
        CountingRenderer renderer;
        const auto instances = vellum::rmlui::NativeContext::open_instances();
        RmlUiSurface ui(assets, renderer, {}, {"missing.rml"});
        REQUIRE_FALSE(ui.attach((__bridge void*)window.contentView));
        RmlUiSurfaceOptions no_font{"probe.rml"};
        no_font.font_faces = {"missing.ttf"};
        RmlUiSurface ui_no_font(assets, renderer, {}, no_font);
        REQUIRE_FALSE(ui_no_font.attach((__bridge void*)window.contentView));
        REQUIRE(window.contentView.subviews.count == 0);
        REQUIRE(ui.surface() == nullptr);
        REQUIRE(window.contentView.subviews.count == 0);
        REQUIRE(vellum::rmlui::NativeContext::open_instances() == instances);
        [window close];
    }
}

TEST_CASE("pointer input reaches RmlUi elements in device pixels", "[native-surface][rmlui]") {
    REQUIRE_METAL();
    @autoreleasepool {
        Editor e;
        REQUIRE(e.attach());
        // Hit testing needs a laid-out document, which the first frame does.
        REQUIRE(e.wait_for([&] { return e.ui->frames() > 0; }));
        int clicks = 0;
        struct Listener final : Rml::EventListener {
            int& count;
            explicit Listener(int& c) : count(c) {}
            void ProcessEvent(Rml::Event&) override {
                ++count;
            }
        } listener(clicks);
        REQUIRE(e.ui->with_context([&](Rml::Context& ctx) {
            ctx.GetDocument(0)->GetElementById("button")->AddEventListener("click", &listener);
        }));
        [e.view() mouseMoved:e.mouse(NSEventTypeMouseMoved, 20, 20)];
        bool hovered = false;
        e.ui->with_context([&](Rml::Context& ctx) {
            hovered = ctx.GetDocument(0)->GetElementById("button")->IsPseudoClassSet("hover");
        });
        REQUIRE(hovered);
        [e.view() mouseDown:e.mouse(NSEventTypeLeftMouseDown, 20, 20)];
        [e.view() mouseUp:e.mouse(NSEventTypeLeftMouseUp, 20, 20)];
        // A frame in flight queues the event; flush applies it.
        e.ui->flush_input();
        REQUIRE(clicks == 1);
        // Outside the button: no hover, no click.
        [e.view() mouseMoved:e.mouse(NSEventTypeMouseMoved, 250, 180)];
        e.ui->with_context([&](Rml::Context& ctx) {
            hovered = ctx.GetDocument(0)->GetElementById("button")->IsPseudoClassSet("hover");
        });
        REQUIRE_FALSE(hovered);
        e.ui->with_context([&](Rml::Context& ctx) {
            ctx.GetDocument(0)->GetElementById("button")->RemoveEventListener("click", &listener);
        });
        e.ui->detach();
    }
}

TEST_CASE("a focused RmlUi text field takes the keyboard and receives text",
          "[native-surface][rmlui]") {
    REQUIRE_METAL();
    @autoreleasepool {
        Editor e;
        REQUIRE(e.attach());
        REQUIRE(e.wait_for([&] { return e.ui->frames() > 0; }));
        // The field resolved a face, so it lays out text rather than drawing
        // nothing.
        Rml::FontFaceHandle face = 0;
        e.ui->with_context([&](Rml::Context& ctx) {
            face = ctx.GetDocument(0)->GetElementById("field")->GetFontFaceHandle();
        });
        REQUIRE(face != 0);
        REQUIRE_FALSE(e.ui->surface()->text_focus());
        // Past the end of the text, so the caret lands after "ab".
        [e.view() mouseDown:e.mouse(NSEventTypeLeftMouseDown, 190, 115)];
        [e.view() mouseUp:e.mouse(NSEventTypeLeftMouseUp, 190, 115)];
        e.ui->flush_input();
        REQUIRE(e.ui->surface()->text_focus());
        id<NSTextInputClient> client = (id<NSTextInputClient>)e.view();
        [client insertText:@"cé" replacementRange:NSMakeRange(NSNotFound, 0)];
        std::string value;
        e.ui->with_context([&](Rml::Context& ctx) {
            value =
                ctx.GetDocument(0)->GetElementById("field")->GetAttribute<Rml::String>("value", "");
        });
        REQUIRE(value == "abcé");
        // A click elsewhere blurs the field and gives the keyboard back.
        [e.view() mouseDown:e.mouse(NSEventTypeLeftMouseDown, 250, 180)];
        [e.view() mouseUp:e.mouse(NSEventTypeLeftMouseUp, 250, 180)];
        e.ui->flush_input();
        REQUIRE_FALSE(e.ui->surface()->text_focus());
        e.ui->detach();
    }
}

TEST_CASE("the RmlUi context follows the surface size", "[native-surface][rmlui]") {
    REQUIRE_METAL();
    @autoreleasepool {
        Editor e;
        REQUIRE(e.attach());
        const double scale = e.window.backingScaleFactor;
        [e.window setContentSize:NSMakeSize(400, 260)];
        e.ui->idle();
        Rml::Vector2i size;
        e.ui->with_context([&](Rml::Context& ctx) { size = ctx.GetDimensions(); });
        REQUIRE(size.x == static_cast<int>(400 * scale));
        REQUIRE(size.y == static_cast<int>(260 * scale));
        e.ui->detach();
    }
}

TEST_CASE("two editors never enter RmlUi at the same time", "[native-surface][rmlui]") {
    REQUIRE_METAL();
    @autoreleasepool {
        CountingRenderer::inside.store(0);
        CountingRenderer::max_inside.store(0);
        Editor a(NSMakeRect(0, 0, 200, 150));
        Editor b(NSMakeRect(0, 0, 200, 150));
        REQUIRE(a.attach());
        REQUIRE(b.attach());
        // Keep both render threads busy at once.
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
        while (std::chrono::steady_clock::now() < until) {
            a.ui->request_frame();
            b.ui->request_frame();
            [a.view() mouseMoved:a.mouse(NSEventTypeMouseMoved, 20, 20)];
            [b.view() mouseMoved:b.mouse(NSEventTypeMouseMoved, 60, 30)];
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
        }
        REQUIRE(a.ui->frames() > 1);
        REQUIRE(b.ui->frames() > 1);
        REQUIRE(CountingRenderer::max_inside.load() == 1);
        a.ui->detach();
        b.ui->detach();
    }
}

TEST_CASE("open and close cycles leave no RmlUi instance or surface behind",
          "[native-surface][rmlui][lifecycle]") {
    REQUIRE_METAL();
    const auto instances = vellum::rmlui::NativeContext::open_instances();
    const auto surfaces = pulp::native::MetalSurface::live_surfaces();
    for (int i = 0; i < 10; ++i) {
        @autoreleasepool {
            Editor e;
            REQUIRE(e.attach());
            REQUIRE(e.wait_for([&] { return e.ui->frames() > 0; }));
            if (i % 2 == 0)
                e.ui->detach();
            else
                e.ui->invalidate();
        }
        REQUIRE(vellum::rmlui::NativeContext::open_instances() == instances);
        REQUIRE(pulp::native::MetalSurface::live_surfaces() == surfaces);
    }
}

TEST_CASE("keys, modifiers and cursor names translate to RmlUi's", "[native-surface][rmlui]") {
    using pulp::native::Key;
    REQUIRE(rml_key_for(Key::a) == Rml::Input::KI_A);
    REQUIRE(rml_key_for(Key::z) == Rml::Input::KI_Z);
    REQUIRE(rml_key_for(Key::digit_7) == Rml::Input::KI_7);
    REQUIRE(rml_key_for(Key::f13) == Rml::Input::KI_F13);
    REQUIRE(rml_key_for(Key::keypad_3) == Rml::Input::KI_NUMPAD3);
    REQUIRE(rml_key_for(Key::enter) == Rml::Input::KI_RETURN);
    REQUIRE(rml_key_for(Key::backspace) == Rml::Input::KI_BACK);
    REQUIRE(rml_key_for(Key::shift_right) == Rml::Input::KI_RSHIFT);
    REQUIRE(rml_key_for(Key::unknown) == Rml::Input::KI_UNKNOWN);
    REQUIRE(rml_modifiers_for(pulp::native::modifier_command | pulp::native::modifier_shift) ==
            (Rml::Input::KM_META | Rml::Input::KM_SHIFT));
    REQUIRE(cursor_for_name("pointer") == pulp::native::Cursor::pointing_hand);
    REQUIRE(cursor_for_name("text") == pulp::native::Cursor::ibeam);
    REQUIRE(cursor_for_name("no-such-cursor") == pulp::native::Cursor::arrow);
}

TEST_CASE("the surface uses the device the caller's renderer was built on",
          "[native-surface][rmlui]") {
    REQUIRE_METAL();
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 100, 100)
                                                       styleMask:NSWindowStyleMaskBorderless
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.releasedWhenClosed = NO;
        id<MTLDevice> device = (id<MTLDevice>)[[PulpTestDeviceProxy alloc]
            initWithDevice:MTLCreateSystemDefaultDevice()];
        MemoryAssets assets;
        CountingRenderer renderer;
        RmlUiSurfaceOptions options{"probe.rml"};
        options.device = (__bridge void*)device;
        options.opaque = false;
        RmlUiSurface ui(assets, renderer, {}, options);
        REQUIRE(ui.attach((__bridge void*)window.contentView));
        // A proxy has its own identity, so this fails if the surface fell back
        // to the system default device instead of the one passed.
        REQUIRE(ui.surface()->device() == (__bridge void*)device);
        NSView* view = (__bridge NSView*)ui.surface()->view();
        REQUIRE_FALSE(view.layer.opaque);
        ui.detach();
        [window close];
    }
}

TEST_CASE("pointer input does not wait for a frame the render thread is drawing",
          "[native-surface][rmlui][threading]") {
    REQUIRE_METAL();
    @autoreleasepool {
        Editor e;
        REQUIRE(e.attach());
        REQUIRE(e.wait_for([&] { return e.ui->frames() > 0; }));
        // Each frame holds the RmlUi lock for several draws of 6 ms.
        e.renderer.draw_delay_us.store(6000);
        e.max_hold_ns.store(0);
        std::vector<double> pointer_ms;
        std::vector<double> with_context_ms;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        int i = 0;
        while (std::chrono::steady_clock::now() < until) {
            e.ui->request_frame();
            // Let the render thread take the lock and start drawing, so the
            // event arrives while a frame is in flight.
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.008]];
            // Alternate: the same kind of in-flight frame, once for an event
            // and once for with_context(), which waits for the lock by design.
            if (i++ % 2 == 0) {
                const double x = 10 + (i % 200);
                const auto t0 = std::chrono::steady_clock::now();
                [e.view() mouseMoved:e.mouse(NSEventTypeMouseMoved, x, 20)];
                pointer_ms.push_back(
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                        .count());
            } else {
                const auto t0 = std::chrono::steady_clock::now();
                e.ui->with_context([](Rml::Context&) {});
                with_context_ms.push_back(
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                        .count());
            }
        }
        const double hold_ms = static_cast<double>(e.max_hold_ns.load()) / 1e6;
        const double p95 = percentile_ms(pointer_ms, 0.95);
        const double with_context_max = percentile_ms(with_context_ms, 1.0);
        INFO("longest frame hold " << hold_ms << " ms, pointer p95 " << p95
                                   << " ms, with_context max " << with_context_max << " ms");
        // The instrument can see a stall: a frame held the lock long enough
        // that a caller waiting on it (with_context blocks by design) lost
        // several milliseconds.
        // Thresholds follow the measured hold rather than a wall-clock number,
        // so a loaded host moves both sides together.
        REQUIRE(hold_ms > 10.0);
        REQUIRE(with_context_max > 0.25 * hold_ms);
        // Pointer input is not such a caller.
        REQUIRE(p95 < 0.1 * hold_ms);
        e.renderer.draw_delay_us.store(200);
        e.ui->detach();
    }
}

TEST_CASE("input queued behind a frame is applied in order and never lost",
          "[native-surface][rmlui][threading]") {
    REQUIRE_METAL();
    @autoreleasepool {
        CountingRenderer::inside.store(0);
        CountingRenderer::max_inside.store(0);
        Editor a(NSMakeRect(0, 0, 300, 200));
        Editor b(NSMakeRect(0, 0, 300, 200));
        REQUIRE(a.attach());
        REQUIRE(b.attach());
        REQUIRE(a.wait_for([&] { return a.ui->frames() > 0 && b.ui->frames() > 0; }));
        a.renderer.draw_delay_us.store(500);
        b.renderer.draw_delay_us.store(500);

        int clicks_a = 0;
        int clicks_b = 0;
        struct Listener final : Rml::EventListener {
            int& count;
            explicit Listener(int& c) : count(c) {}
            void ProcessEvent(Rml::Event&) override {
                ++count;
            }
        } listener_a(clicks_a), listener_b(clicks_b);
        a.ui->with_context([&](Rml::Context& ctx) {
            ctx.GetDocument(0)->GetElementById("button")->AddEventListener("click", &listener_a);
        });
        b.ui->with_context([&](Rml::Context& ctx) {
            ctx.GetDocument(0)->GetElementById("button")->AddEventListener("click", &listener_b);
        });

        // Park a render thread inside a draw, lock held (one lock serves both
        // editors), so every event below arrives behind a frame, then let them go. Frames are also
        // requested from another thread, as a host driving two editors would.
        std::atomic<bool> stop{false};
        std::thread requester([&] {
            while (!stop.load()) {
                a.ui->request_frame();
                b.ui->request_frame();
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        });
        int sent = 0;
        for (int round = 0; round < 15; ++round) {
            a.renderer.hold_draws.store(true);
            b.renderer.hold_draws.store(true);
            a.ui->request_frame();
            b.ui->request_frame();
            REQUIRE(
                a.wait_for([&] { return a.renderer.parked.load() || b.renderer.parked.load(); }));
            for (Editor* e : {&a, &b}) {
                [e->view() mouseMoved:e->mouse(NSEventTypeMouseMoved, 20, 20)];
                [e->view() mouseDown:e->mouse(NSEventTypeLeftMouseDown, 20, 20)];
                [e->view() mouseUp:e->mouse(NSEventTypeLeftMouseUp, 20, 20)];
                [e->view() mouseMoved:e->mouse(NSEventTypeMouseMoved, 250, 180)];
            }
            ++sent;
            a.renderer.hold_draws.store(false);
            b.renderer.hold_draws.store(false);
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        }
        stop.store(true);
        requester.join();
        a.ui->flush_input();
        b.ui->flush_input();
        REQUIRE(sent == 15);
        // Something really queued behind a frame, so the claim is about the
        // queue and not about uncontended delivery.
        REQUIRE(a.ui->queued_inputs() + b.ui->queued_inputs() > 0);
        // Every click arrived: only moves and wheel deltas ever merge.
        REQUIRE(clicks_a == sent);
        REQUIRE(clicks_b == sent);
        REQUIRE(CountingRenderer::max_inside.load() == 1);
        a.ui->with_context([&](Rml::Context& ctx) {
            ctx.GetDocument(0)->GetElementById("button")->RemoveEventListener("click", &listener_a);
        });
        b.ui->with_context([&](Rml::Context& ctx) {
            ctx.GetDocument(0)->GetElementById("button")->RemoveEventListener("click", &listener_b);
        });
        a.ui->detach();
        b.ui->detach();
    }
}

TEST_CASE("a click queued behind a frame lands before the text typed after it",
          "[native-surface][rmlui][threading]") {
    REQUIRE_METAL();
    @autoreleasepool {
        Editor e;
        REQUIRE(e.attach());
        REQUIRE(e.wait_for([&] { return e.ui->frames() > 0; }));
        e.renderer.draw_delay_us.store(6000);
        bool queued = false;
        for (int attempt = 0; attempt < 20 && !queued; ++attempt) {
            e.ui->request_frame();
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.008]];
            const auto t0 = std::chrono::steady_clock::now();
            [e.view() mouseDown:e.mouse(NSEventTypeLeftMouseDown, 190, 115)];
            [e.view() mouseUp:e.mouse(NSEventTypeLeftMouseUp, 190, 115)];
            // Returned without waiting: the frame was in flight, so the click
            // is queued and the field is not focused yet.
            queued =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                        .count() < 3.0 &&
                !e.ui->surface()->text_focus();
            if (!queued) {
                // Raced the frame: reset and try again.
                [e.view() mouseDown:e.mouse(NSEventTypeLeftMouseDown, 250, 180)];
                [e.view() mouseUp:e.mouse(NSEventTypeLeftMouseUp, 250, 180)];
                e.ui->flush_input();
            }
        }
        REQUIRE(queued);
        id<NSTextInputClient> client = (id<NSTextInputClient>)e.view();
        [client insertText:@"Z" replacementRange:NSMakeRange(NSNotFound, 0)];
        std::string value;
        e.ui->with_context([&](Rml::Context& ctx) {
            value =
                ctx.GetDocument(0)->GetElementById("field")->GetAttribute<Rml::String>("value", "");
        });
        REQUIRE(value == "abZ");
        e.renderer.draw_delay_us.store(200);
        e.ui->detach();
    }
}

TEST_CASE("input applied after a frame is drawn without another request",
          "[native-surface][rmlui][threading]") {
    REQUIRE_METAL();
    @autoreleasepool {
        Editor e;
        REQUIRE(e.attach());
        REQUIRE(e.wait_for([&] { return e.ui->frames() > 0; }));
        std::atomic<long long> applied_ns{0};
        struct Listener final : Rml::EventListener {
            std::atomic<long long>& at;
            explicit Listener(std::atomic<long long>& a) : at(a) {}
            void ProcessEvent(Rml::Event&) override {
                at.store(now_ns());
            }
        } listener(applied_ns);
        e.ui->with_context([&](Rml::Context& ctx) {
            ctx.GetDocument(0)->GetElementById("button")->AddEventListener("mouseover", &listener);
        });
        // Away from the button, and let every frame that asks for settle.
        [e.view() mouseMoved:e.mouse(NSEventTypeMouseMoved, 250, 180)];
        e.ui->flush_input();
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.3]];

        // Park the render thread inside a draw, lock held, and send exactly
        // one event: it queues, and nothing else asks for a frame.
        e.renderer.hold_draws.store(true);
        e.ui->request_frame();
        REQUIRE(e.wait_for([&] { return e.renderer.parked.load(); }));
        const auto queued_before = e.ui->queued_inputs();
        [e.view() mouseMoved:e.mouse(NSEventTypeMouseMoved, 20, 20)];
        REQUIRE(e.ui->queued_inputs() == queued_before + 1);
        REQUIRE(applied_ns.load() == 0);
        e.renderer.hold_draws.store(false);
        // Keep the main thread from draining while the frames that were
        // already asked for run and finish: they draw without the event.
        const long long released_ns = now_ns();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        REQUIRE(applied_ns.load() == 0);
        REQUIRE(e.begin_ns.load() > released_ns);
        REQUIRE(e.wait_for([&] { return applied_ns.load() != 0; }));
        // Now the drain runs. It must ask for another frame, because every
        // frame so far drew without the event: one begins after it.
        REQUIRE(e.wait_for([&] { return e.begin_ns.load() > applied_ns.load(); }, 2.0));
        e.ui->with_context([&](Rml::Context& ctx) {
            ctx.GetDocument(0)->GetElementById("button")->RemoveEventListener("mouseover",
                                                                              &listener);
        });
        e.ui->detach();
    }
}

TEST_CASE("a size change idle() could not apply is applied by the next with_context",
          "[native-surface][rmlui][threading]") {
    REQUIRE_METAL();
    @autoreleasepool {
        Editor e;
        REQUIRE(e.attach());
        REQUIRE(e.wait_for([&] { return e.ui->frames() > 0; }));
        const double scale = e.window.backingScaleFactor;
        // Park the render thread inside a draw, lock held, then resize and
        // tick: the tick finds the lock busy and skips the size.
        e.renderer.hold_draws.store(true);
        e.ui->request_frame();
        REQUIRE(e.wait_for([&] { return e.renderer.parked.load(); }));
        [e.window setContentSize:NSMakeSize(420, 280)];
        e.ui->idle();
        e.renderer.hold_draws.store(false);
        Rml::Vector2i size;
        e.ui->with_context([&](Rml::Context& ctx) { size = ctx.GetDimensions(); });
        REQUIRE(size.x == static_cast<int>(420 * scale));
        REQUIRE(size.y == static_cast<int>(280 * scale));
        e.ui->detach();
    }
}
