// test_rmlui_native_surface.mm — RmlUi on a pulp-native surface: the document
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
        std::this_thread::sleep_for(std::chrono::microseconds(200));
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

struct Editor {
    NSWindow* window = nil;
    MemoryAssets assets;
    CountingRenderer renderer;
    std::atomic<int> begun{0};
    std::atomic<int> ended{0};
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
            begun.fetch_add(1);
        };
        hooks.end_frame = [this] { ended.fetch_add(1); };
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
