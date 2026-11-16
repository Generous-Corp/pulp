// test_rmlui_native_surface.mm: RmlUi on a pulp-native surface. The document
// loads and renders through the caller's renderer, pointer, key and text input
// reach RmlUi elements, the context follows the surface size, and RmlUi is
// never entered by two threads at once. A counting Rml::RenderInterface stands
// in for a real renderer. Skips, saying so, without a Metal device. Compiled
// with ARC.

#include <catch2/catch_test_macros.hpp>

#include <pulp/rmlui_native/editor_keys.hpp>
#include <pulp/rmlui_native/rmlui_surface.hpp>

#include "../integrations/rmlui-native/src/input_drain.hpp"

#include <vellum/rmlui/assets.hpp>
#include <vellum/rmlui/native.hpp>

#include <RmlUi/Core.h>

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <map>
#include <memory>
#include <mutex>
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
#pxbox { position: absolute; left: 0; top: 160px; width: 100px; height: 10px;
         background-color: #0000ff; }
#dpbox { position: absolute; left: 0; top: 175dp; width: 100dp; height: 10dp;
         background-color: #0000ff; }
#field { display: block; position: absolute; left: 0; top: 100dp; width: 200dp; height: 30dp;
         background-color: #333333; }
</style>
</head>
<body>
<div id="button"></div>
<div id="pxbox"></div><div id="dpbox"></div>
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

long long now_ns();

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
            // unbounded-wait: allow DrawCleanup releases this latch on bounded assertion failure.
            while (hold_draws.load())
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            parked.store(false);
        }
        leave();
    }
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {
        mark_release();
        released_geometry.fetch_add(1);
    }
    Rml::TextureHandle LoadTexture(Rml::Vector2i&, const Rml::String&) override {
        return 0;
    }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override {
        return 1;
    }
    void ReleaseTexture(Rml::TextureHandle) override {
        mark_release();
        released_textures.fetch_add(1);
    }
    // When the first release since it was last cleared happened.
    void mark_release() {
        long long none = 0;
        first_release_ns.compare_exchange_strong(none, now_ns());
    }
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}

    static inline std::atomic<int> inside{0};
    static inline std::atomic<int> max_inside{0};
    std::atomic<int> compiled{0};
    std::atomic<int> rendered{0};
    std::atomic<int> released_geometry{0};
    std::atomic<int> released_textures{0};
    std::atomic<long long> first_release_ns{0};
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
    std::atomic<int> trimmed{0};
    /// Set to hold the next frame's command buffer on the GPU until `gate` is
    /// signalled with 1.
    std::atomic<bool> gate_next{false};
    std::atomic<bool> gated{false};
    id<MTLSharedEvent> gate = nil;
    id<MTLCommandBuffer> gated_cb = nil;
    /// The gated command buffer's status when the trim hook ran, or -1.
    std::atomic<int> trim_saw_status{-1};
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
            if (gate_next.exchange(false)) {
                id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)t.command_buffer;
                gate = [cb.device newSharedEvent];
                [cb encodeWaitForEvent:gate value:1];
                gated_cb = cb;
                gated.store(true);
            }
            // Stored before the count moves, so a reader that sees the count
            // sees the value.
            if (begun.load() == 0)
                compiled_at_first_begin.store(renderer.compiled.load());
            begun.fetch_add(1);
            begin_ns.store(now_ns());
        };
        hooks.end_frame = [this] {
            const long long hold = now_ns() - begin_ns.load();
            long long seen = max_hold_ns.load();
            while (hold > seen && !max_hold_ns.compare_exchange_weak(seen, hold)) {
            }
            ended.fetch_add(1);
        };
        hooks.trim = [this] {
            if (gated.load())
                trim_saw_status.store(static_cast<int>(gated_cb.status));
            trimmed.fetch_add(1);
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
    /// A pointer event at a document position in dp. Events are in window
    /// points and reach RmlUi in device pixels, and a dp is `density_ratio()`
    /// device pixels: with an SDK that lacks the setter dp equals px, so on a 2x
    /// window the same document position is fewer points.
    NSEvent* doc_mouse(NSEventType type, double x, double y) const {
        const double to_points = ui->density_ratio() / window.backingScaleFactor;
        return mouse(type, x * to_points, y * to_points);
    }
    /// idle() only try-locks, so a frame in flight leaves a resize for a later
    /// tick. Tick until the context has the wanted size, within a deadline.
    bool sync_context_size(Rml::Vector2i want, double seconds = 5.0) {
        bool synced = false;
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (!synced && std::chrono::steady_clock::now() < until) {
            ui->idle();
            ui->with_context([&](Rml::Context& ctx) { synced = ctx.GetDimensions() == want; });
            if (!synced)
                [[NSRunLoop currentRunLoop]
                    runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
        }
        return synced;
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

// Declared after listeners and before parking a draw: unwinding must release
// render threads and stop the requester before detaching editors or listeners.
template <class EditorType> struct DrawCleanup {
    std::vector<EditorType*> editors;
    std::atomic<bool> stop{false};
    std::thread requester;

    ~DrawCleanup() {
        stop.store(true);
        for (auto* editor : editors)
            editor->renderer.hold_draws.store(false);
        if (requester.joinable())
            requester.join();
        for (auto* editor : editors)
            editor->ui->detach();
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
        [e.view() mouseDown:e.doc_mouse(NSEventTypeLeftMouseDown, 190, 115)];
        [e.view() mouseUp:e.doc_mouse(NSEventTypeLeftMouseUp, 190, 115)];
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
        const Rml::Vector2i want(static_cast<int>(400 * scale), static_cast<int>(260 * scale));
        REQUIRE(e.sync_context_size(want));
        Rml::Vector2i size;
        e.ui->with_context([&](Rml::Context& ctx) { size = ctx.GetDimensions(); });
        REQUIRE(size.x == want.x);
        REQUIRE(size.y == want.y);
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

namespace {

std::string field_value(Editor& e) {
    std::string value;
    e.ui->with_context([&](Rml::Context& ctx) {
        value = ctx.GetDocument(0)->GetElementById("field")->GetAttribute<Rml::String>("value", "");
    });
    return value;
}

void focus_field(Editor& e) {
    [e.view() mouseDown:e.mouse(NSEventTypeLeftMouseDown, 190, 115)];
    [e.view() mouseUp:e.mouse(NSEventTypeLeftMouseUp, 190, 115)];
    // The click may have been queued behind a frame.
    e.ui->flush_input();
}

pulp::format::EditorKeyEvent key_event(pulp::format::EditorKey key, char32_t character = 0,
                                       std::uint8_t modifiers = 0, bool repeat = false) {
    pulp::format::EditorKeyEvent event;
    event.key = key;
    event.character = character;
    event.modifiers = modifiers;
    event.is_repeat = repeat;
    return event;
}

} // namespace

TEST_CASE("an offered key is taken only while a text field holds focus",
          "[native-surface][rmlui][keys]") {
    REQUIRE_METAL();
    using pulp::format::EditorKey;
    using pulp::format::KeyDisposition;
    @autoreleasepool {
        Editor e;
        REQUIRE(e.attach());
        REQUIRE(e.wait_for([&] { return e.ui->frames() > 0; }));

        // No field focused: Space is the host's (transport), and RmlUi is not
        // touched.
        REQUIRE_FALSE(e.ui->text_field_focused());
        const auto host_press = key_event(EditorKey::space, U' ');
        REQUIRE(offer_editor_key(*e.ui, host_press) == KeyDisposition::forward);
        REQUIRE(field_value(e) == "ab");

        focus_field(e);
        REQUIRE(e.ui->text_field_focused());
        // A repeat keeps the host's original ownership after focus changes.
        REQUIRE(offer_editor_key(*e.ui, key_event(EditorKey::space, U' ', 0, true)) ==
                KeyDisposition::forward);
        auto host_release = host_press;
        host_release.is_down = false;
        REQUIRE(offer_editor_key(*e.ui, host_release) == KeyDisposition::forward);
        // Space is typed into the field and the host does not see it.
        REQUIRE(offer_editor_key(*e.ui, key_event(EditorKey::space, U' ')) ==
                KeyDisposition::consumed);
        REQUIRE(offer_editor_key(*e.ui, key_event(EditorKey::none, U'\u00e9')) ==
                KeyDisposition::consumed);
        REQUIRE(field_value(e) == "ab \xc3\xa9");
        // An editing key reaches RmlUi as a key.
        REQUIRE(offer_editor_key(*e.ui, key_event(EditorKey::backspace)) ==
                KeyDisposition::consumed);
        REQUIRE(field_value(e) == "ab ");
        // A command shortcut is the host's, and types nothing.
        REQUIRE(offer_editor_key(*e.ui, key_event(EditorKey::none, U's',
                                                  pulp::format::editor_modifier_command)) ==
                KeyDisposition::forward);
        REQUIRE(field_value(e) == "ab ");
        // A release types nothing.
        auto release = key_event(EditorKey::space, U' ');
        release.is_down = false;
        // The release of a key the field took is the field's too.
        REQUIRE(offer_editor_key(*e.ui, release) == KeyDisposition::consumed);
        // A release whose press the host took stays the host's, even when a
        // field gained focus in between.
        e.ui->with_context(
            [&](Rml::Context& ctx) { ctx.GetDocument(0)->GetElementById("field")->Blur(); });
        REQUIRE_FALSE(e.ui->text_field_focused());
        REQUIRE(offer_editor_key(*e.ui, key_event(EditorKey::space, U' ')) ==
                KeyDisposition::forward);
        focus_field(e);
        REQUIRE(e.ui->text_field_focused());
        REQUIRE(offer_editor_key(*e.ui, release) == KeyDisposition::forward);
        // A field-owned printable key inserts its character before the
        // release, and that release leaves the value unchanged.
        // (RmlUi does not add a second consecutive space to a field's value, so
        // clear the trailing one first.)
        REQUIRE(offer_editor_key(*e.ui, key_event(EditorKey::backspace)) ==
                KeyDisposition::consumed);
        REQUIRE(field_value(e) == "ab");
        auto backspace_up = key_event(EditorKey::backspace);
        backspace_up.is_down = false;
        REQUIRE(offer_editor_key(*e.ui, backspace_up) == KeyDisposition::consumed);
        auto final_space = key_event(EditorKey::space, U' ');
        REQUIRE(offer_editor_key(*e.ui, final_space) == KeyDisposition::consumed);
        REQUIRE(field_value(e) == "ab ");
        e.ui->with_context(
            [&](Rml::Context& ctx) { ctx.GetDocument(0)->GetElementById("field")->Blur(); });
        final_space.is_down = false;
        REQUIRE(offer_editor_key(*e.ui, final_space) == KeyDisposition::consumed);
        REQUIRE(field_value(e) == "ab ");

        // A field-owned editing key keeps its ownership through repeats and a
        // blur, too.
        focus_field(e);
        REQUIRE(offer_editor_key(*e.ui, key_event(EditorKey::left)) == KeyDisposition::consumed);
        REQUIRE(offer_editor_key(*e.ui, key_event(EditorKey::left, 0, 0, true)) ==
                KeyDisposition::consumed);
        e.ui->with_context(
            [&](Rml::Context& ctx) { ctx.GetDocument(0)->GetElementById("field")->Blur(); });
        auto left_release = key_event(EditorKey::left);
        left_release.is_down = false;
        REQUIRE(offer_editor_key(*e.ui, left_release) == KeyDisposition::consumed);

        // Printable contract keys have no physical Key value. Their character
        // identity keeps overlapping presses paired with the right releases.
        focus_field(e);
        REQUIRE(offer_editor_key(*e.ui, key_event(EditorKey::none, U'x')) ==
                KeyDisposition::consumed);
        REQUIRE(offer_editor_key(*e.ui, key_event(EditorKey::none, U'y')) ==
                KeyDisposition::consumed);
        // (Both are typed; a blur may trim the value's trailing space, so the
        // check is on the typed tail.)
        const std::string typed = field_value(e);
        REQUIRE(typed.size() >= 2);
        REQUIRE(typed.substr(typed.size() - 2) == "xy");
        auto x_release = key_event(EditorKey::none, U'x');
        x_release.is_down = false;
        auto y_release = key_event(EditorKey::none, U'y');
        y_release.is_down = false;
        REQUIRE(offer_editor_key(*e.ui, x_release) == KeyDisposition::consumed);
        REQUIRE(offer_editor_key(*e.ui, y_release) == KeyDisposition::consumed);
        REQUIRE(field_value(e) == typed);
        e.ui->detach();
    }
}

TEST_CASE("a key press keeps its owner across modifier changes and a lost release",
          "[native-surface][rmlui][keys]") {
    REQUIRE_METAL();
    using pulp::format::EditorKey;
    using pulp::format::KeyDisposition;
    @autoreleasepool {
        Editor e;
        REQUIRE(e.attach());
        REQUIRE(e.wait_for([&] { return e.ui->frames() > 0; }));
        auto blur = [&] {
            e.ui->with_context(
                [&](Rml::Context& ctx) { ctx.GetDocument(0)->GetElementById("field")->Blur(); });
        };
        // The same physical key (code 0 + 1) with a different composed
        // character, as Shift changes it.
        auto key_a = [](char32_t c, bool down, bool repeat, std::uint8_t mods) {
            auto event = key_event(EditorKey::none, c, mods, repeat);
            event.is_down = down;
            event.physical_key = 1;
            return event;
        };

        // A host-owned 'a' gains a field and Shift before its repeat and
        // release: both stay the host's.
        REQUIRE(offer_editor_key(*e.ui, key_a(U'a', true, false, 0)) == KeyDisposition::forward);
        focus_field(e);
        REQUIRE(e.ui->text_field_focused());
        REQUIRE(
            offer_editor_key(*e.ui, key_a(U'A', true, true, pulp::format::editor_modifier_shift)) ==
            KeyDisposition::forward);
        REQUIRE(offer_editor_key(*e.ui,
                                 key_a(U'A', false, false, pulp::format::editor_modifier_shift)) ==
                KeyDisposition::forward);
        REQUIRE(field_value(e) == "ab");

        // A field-owned shifted 'A' released without Shift is still the field's.
        REQUIRE(offer_editor_key(*e.ui,
                                 key_a(U'A', true, false, pulp::format::editor_modifier_shift)) ==
                KeyDisposition::consumed);
        REQUIRE(field_value(e) == "abA");
        blur();
        REQUIRE(offer_editor_key(*e.ui, key_a(U'a', false, false, 0)) == KeyDisposition::consumed);
        REQUIRE(field_value(e) == "abA");

        // A consumed Space whose release was lost to window deactivation must
        // not own the next, host-routed Space press.
        focus_field(e);
        auto space = key_event(EditorKey::space, U' ');
        REQUIRE(offer_editor_key(*e.ui, space) == KeyDisposition::consumed);
        e.ui->set_window_focused(false);
        e.ui->set_window_focused(true);
        blur();
        REQUIRE_FALSE(e.ui->text_field_focused());
        REQUIRE(offer_editor_key(*e.ui, space) == KeyDisposition::forward);
        auto space_repeat = key_event(EditorKey::space, U' ', 0, true);
        REQUIRE(offer_editor_key(*e.ui, space_repeat) == KeyDisposition::forward);
        auto space_up = space;
        space_up.is_down = false;
        REQUIRE(offer_editor_key(*e.ui, space_up) == KeyDisposition::forward);
        // A fresh press replaces a stale record with no deactivation in between.
        focus_field(e);
        REQUIRE(offer_editor_key(*e.ui, space) == KeyDisposition::consumed);
        blur();
        REQUIRE(offer_editor_key(*e.ui, space) == KeyDisposition::forward);
        REQUIRE(offer_editor_key(*e.ui, space_up) == KeyDisposition::forward);
        e.ui->detach();
    }
}

// A window whose backing scale a test can set, standing in for a display with
// another scale.
@interface PulpRmlUiScaleWindow : NSWindow
@property(nonatomic) CGFloat testScale;
@end
@implementation PulpRmlUiScaleWindow
- (CGFloat)backingScaleFactor {
    return self.testScale > 0 ? self.testScale : [super backingScaleFactor];
}
@end

TEST_CASE("px lengths are device pixels and dp lengths follow the density ratio",
          "[native-surface][rmlui][scale]") {
    REQUIRE_METAL();
    @autoreleasepool {
        [NSApplication sharedApplication];
        auto* window = [[PulpRmlUiScaleWindow alloc] initWithContentRect:NSMakeRect(0, 0, 300, 200)
                                                               styleMask:NSWindowStyleMaskBorderless
                                                                 backing:NSBackingStoreBuffered
                                                                   defer:NO];
        window.releasedWhenClosed = NO;
        window.testScale = 1.0;
        MemoryAssets assets;
        CountingRenderer renderer;
        RmlUiSurfaceOptions options;
        options.document = "probe.rml";
        options.font_faces = {"font.ttf"};
        RmlUiSurface ui(assets, renderer, {}, options);
        REQUIRE(ui.attach((__bridge void*)window.contentView));
        NSView* view = (__bridge NSView*)ui.surface()->view();
        // Hit testing needs a laid-out document, which the first frame does.
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (ui.frames() == 0 && std::chrono::steady_clock::now() < until)
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        REQUIRE(ui.frames() > 0);

        auto measure = [&](double scale, float& px_width, float& dp_width, bool& hover_px,
                           bool& hover_dp) {
            window.testScale = scale;
            [view viewDidChangeBackingProperties];
            // idle() only try-locks, so a frame in flight leaves the resize for
            // a later tick. Tick until the context has the new size, then run
            // its update under the lock so the dp layout is current before any
            // measurement or pointer event.
            const Rml::Vector2i want(static_cast<int>(std::lround(300 * scale)),
                                     static_cast<int>(std::lround(200 * scale)));
            bool synced = false;
            const auto sync_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!synced && std::chrono::steady_clock::now() < sync_until) {
                ui.idle();
                ui.with_context([&](Rml::Context& ctx) {
                    synced = ctx.GetDimensions() == want;
                    if (synced)
                        ctx.Update();
                });
                if (!synced)
                    [[NSRunLoop currentRunLoop]
                        runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
            }
            REQUIRE(synced);
            // Pointer positions are in points and reach RmlUi in device pixels.
            // 60 points is 60*scale device pixels: outside a 100px box at 2x.
            auto mouse = [&](double y) {
                return [NSEvent mouseEventWithType:NSEventTypeMouseMoved
                                          location:NSMakePoint(60, 200 - y)
                                     modifierFlags:0
                                         timestamp:1.0
                                      windowNumber:window.windowNumber
                                           context:nil
                                       eventNumber:0
                                        clickCount:1
                                          pressure:1.0];
            };
            ui.with_context([&](Rml::Context& ctx) {
                px_width = ctx.GetDocument(0)->GetElementById("pxbox")->GetClientWidth();
                dp_width = ctx.GetDocument(0)->GetElementById("dpbox")->GetClientWidth();
            });
            [view mouseMoved:mouse(162 / scale)];
            ui.with_context([&](Rml::Context& ctx) {
                hover_px = ctx.GetDocument(0)->GetElementById("pxbox")->IsPseudoClassSet("hover");
            });
            // 177 in the context's dp-scaled layout: inside #dpbox, which starts
            // at 175dp (175 * density_ratio device pixels), in points.
            [view mouseMoved:mouse(177 * ui.density_ratio() / scale)];
            ui.with_context([&](Rml::Context& ctx) {
                hover_dp = ctx.GetDocument(0)->GetElementById("dpbox")->IsPseudoClassSet("hover");
            });
        };

        float px = 0, dp = 0;
        bool hover_px = false, hover_dp = false;
        // 1x: the two units agree.
        measure(1.0, px, dp, hover_px, hover_dp);
        REQUIRE(px == 100.0f);
        REQUIRE(dp == 100.0f);
        REQUIRE(hover_px);
        REQUIRE(hover_dp);
        REQUIRE(ui.density_ratio() == 1.0);

        // 2x: a px length stays 100 device pixels (50 points), so the pointer
        // at 60 points is outside it. A dp length is a point: 200 device pixels
        // with a density ratio, so the pointer is inside; without one it is
        // 100 device pixels and the document renders half size.
        measure(2.0, px, dp, hover_px, hover_dp);
        REQUIRE(px == 100.0f);
        REQUIRE_FALSE(hover_px);
        if (density_ratio_supported()) {
            REQUIRE(ui.density_ratio() == 2.0);
            REQUIRE(dp == 200.0f);
            REQUIRE(hover_dp);
        } else {
            REQUIRE(ui.density_ratio() == 1.0);
            REQUIRE(dp == 100.0f);
            REQUIRE_FALSE(hover_dp);
        }

        // Back to 1x: nothing stays scaled.
        measure(1.0, px, dp, hover_px, hover_dp);
        REQUIRE(px == 100.0f);
        REQUIRE(dp == 100.0f);
        ui.detach();
        [window close];
    }
}

TEST_CASE("an attach without a density ratio says so once, and one with it does not",
          "[native-surface][rmlui][scale]") {
    REQUIRE_METAL();
    @autoreleasepool {
        std::vector<std::string> messages;
        set_diagnostic_sink([&](const std::string& m) { messages.push_back(m); });
        // Declared after `messages`: restores the sink on an assertion unwind too,
        // before `messages` is destroyed.
        struct SinkReset {
            ~SinkReset() {
                set_diagnostic_sink({});
            }
        } sink_reset;
        for (int i = 0; i < 2; ++i) {
            Editor e;
            REQUIRE(e.attach());
            REQUIRE(e.ui->density_ratio() > 0.0);
            e.ui->detach();
            // Detached: no stale scale.
            REQUIRE(e.ui->density_ratio() == 1.0);
        }
        set_diagnostic_sink({});
        if (density_ratio_supported()) {
            REQUIRE(messages.empty());
        } else {
            REQUIRE(messages.size() == 1);
            REQUIRE(messages[0].find("density ratio") != std::string::npos);
            REQUIRE(messages[0].find("dp") != std::string::npos);
        }
    }
}

TEST_CASE("pointer input does not wait for a frame the render thread is drawing",
          "[native-surface][rmlui][threading][performance]") {
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

TEST_CASE("contract keys map to pulp-native keys", "[native-surface][rmlui][keys]") {
    using pulp::format::EditorKey;
    using pulp::native::Key;
    REQUIRE(native_key_for(EditorKey::space) == Key::space);
    REQUIRE(native_key_for(EditorKey::delete_forward) == Key::delete_forward);
    REQUIRE(native_key_for(EditorKey::page_down) == Key::page_down);
    REQUIRE(native_key_for(EditorKey::none) == Key::unknown);
    const auto mapped = native_key_event_for(
        key_event(EditorKey::none, 0x1F600,
                  pulp::format::editor_modifier_shift | pulp::format::editor_modifier_alt));
    REQUIRE(mapped.characters == "\xF0\x9F\x98\x80");
    REQUIRE(mapped.modifiers == (pulp::native::modifier_shift | pulp::native::modifier_option));

    // A lone surrogate types nothing.
    REQUIRE(native_key_event_for(key_event(EditorKey::none, 0xD83D)).characters.empty());
    REQUIRE(native_key_event_for(key_event(EditorKey::none, 0xDE00)).characters.empty());
}

TEST_CASE("losing window focus blurs the text field and gaining it restores it",
          "[native-surface][rmlui][keys]") {
    REQUIRE_METAL();
    @autoreleasepool {
        Editor e;
        REQUIRE(e.attach());
        REQUIRE(e.wait_for([&] { return e.ui->frames() > 0; }));
        focus_field(e);
        REQUIRE(e.ui->text_field_focused());
        REQUIRE(e.ui->surface()->text_focus());

        forward_editor_focus(*e.ui, false);
        REQUIRE_FALSE(e.ui->text_field_focused());
        REQUIRE_FALSE(e.ui->surface()->text_focus());
        bool blurred = false;
        e.ui->with_context([&](Rml::Context& ctx) {
            blurred = ctx.GetFocusElement() != ctx.GetDocument(0)->GetElementById("field");
        });
        REQUIRE(blurred);

        forward_editor_focus(*e.ui, true);
        REQUIRE(e.ui->text_field_focused());
        REQUIRE(e.ui->surface()->text_focus());

        // Losing focus twice in a row keeps what the first loss remembered.
        forward_editor_focus(*e.ui, false);
        forward_editor_focus(*e.ui, false);
        REQUIRE_FALSE(e.ui->text_field_focused());
        forward_editor_focus(*e.ui, true);
        REQUIRE(e.ui->text_field_focused());

        // With no field focused, gaining focus restores nothing.
        [e.view() mouseDown:e.mouse(NSEventTypeLeftMouseDown, 250, 180)];
        [e.view() mouseUp:e.mouse(NSEventTypeLeftMouseUp, 250, 180)];
        e.ui->flush_input();
        REQUIRE_FALSE(e.ui->text_field_focused());
        forward_editor_focus(*e.ui, false);
        forward_editor_focus(*e.ui, true);
        REQUIRE_FALSE(e.ui->text_field_focused());
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
        DrawCleanup<Editor> cleanup{{&a, &b}};
        cleanup.requester = std::thread([&] {
            while (!cleanup.stop.load()) {
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
        cleanup.stop.store(true);
        cleanup.requester.join();
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

TEST_CASE("queued input retries after another editor consumes the last drain notification",
          "[native-surface][rmlui][threading]") {
    REQUIRE_METAL();
    @autoreleasepool {
        Editor a;
        Editor b;
        REQUIRE(a.attach());
        REQUIRE(b.attach());
        REQUIRE(a.wait_for([&] { return a.ui->frames() > 0 && b.ui->frames() > 0; }));
        int applied = 0;
        struct Listener final : Rml::EventListener {
            int& count;
            explicit Listener(int& c) : count(c) {}
            void ProcessEvent(Rml::Event&) override {
                ++count;
            }
        } listener(applied);
        DrawCleanup<Editor> cleanup{{&a, &b}};
        a.ui->with_context([&](Rml::Context& ctx) {
            ctx.GetDocument(0)->GetElementById("button")->AddEventListener("mouseover", &listener);
        });
        [a.view() mouseMoved:a.mouse(NSEventTypeMouseMoved, 250, 180)];
        a.ui->flush_input();
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.3]];
        a.renderer.hold_draws.store(true);
        a.ui->request_frame();
        REQUIRE(a.wait_for([&] { return a.renderer.parked.load(); }));
        const auto queued_before = a.ui->queued_inputs();
        [a.view() mouseMoved:a.mouse(NSEventTypeMouseMoved, 20, 20)];
        REQUIRE(a.ui->queued_inputs() == queued_before + 1);
        a.renderer.hold_draws.store(false);
        // Do not pump the main queue: A must finish all requested frames before
        // B takes the mutex, so A cannot post a later notification to rescue it.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const auto finished = a.ui->frames();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        REQUIRE(a.ui->frames() == finished);
        REQUIRE(a.begun.load() == a.ended.load());
        b.renderer.hold_draws.store(true);
        b.ui->request_frame();
        // Wait without servicing A's main-queue callbacks.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!b.renderer.parked.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        REQUIRE(b.renderer.parked.load());
        // Consume A's callbacks with B holding the lock and no input of its
        // own. The failed attempts must leave a retry behind.
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        REQUIRE(applied == 0);
        REQUIRE(a.ui->frames() == finished);
        REQUIRE(b.ui->queued_inputs() == 0);
        b.renderer.hold_draws.store(false);
        REQUIRE(a.wait_for([&] { return applied == 1; }));
        REQUIRE(a.wait_for([&] { return a.ui->frames() > finished; }));
        a.ui->with_context([&](Rml::Context& ctx) {
            ctx.GetDocument(0)->GetElementById("button")->RemoveEventListener("mouseover",
                                                                              &listener);
        });
    }
}

TEST_CASE("parked draw cleanup releases both editors and joins during unwinding",
          "[native-surface][rmlui][threading]") {
    REQUIRE_METAL();
    @autoreleasepool {
        Editor a;
        Editor b;
        REQUIRE(a.attach());
        REQUIRE(b.attach());
        struct Unwind {};
        bool parked = false;
        std::atomic<bool> requester_exited{false};
        try {
            DrawCleanup<Editor> cleanup{{&a, &b}};
            a.renderer.hold_draws.store(true);
            b.renderer.hold_draws.store(true);
            cleanup.requester = std::thread([&] {
                while (!cleanup.stop.load()) {
                    a.ui->request_frame();
                    b.ui->request_frame();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                requester_exited.store(true);
            });
            parked =
                a.wait_for([&] { return a.renderer.parked.load() || b.renderer.parked.load(); });
            throw Unwind{};
        } catch (const Unwind&) {
        }
        REQUIRE(parked);
        REQUIRE(requester_exited.load());
        REQUIRE_FALSE(a.renderer.hold_draws.load());
        REQUIRE_FALSE(b.renderer.hold_draws.load());
        REQUIRE_FALSE(a.renderer.parked.load());
        REQUIRE_FALSE(b.renderer.parked.load());
        REQUIRE_FALSE(a.ui->attached());
        REQUIRE_FALSE(b.ui->attached());
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
            [e.view() mouseDown:e.doc_mouse(NSEventTypeLeftMouseDown, 190, 115)];
            [e.view() mouseUp:e.doc_mouse(NSEventTypeLeftMouseUp, 190, 115)];
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

TEST_CASE("a click queued behind a frame decides where an offered key goes",
          "[native-surface][rmlui][threading][keys]") {
    REQUIRE_METAL();
    using pulp::format::EditorKey;
    using pulp::format::KeyDisposition;
    @autoreleasepool {
        Editor e;
        REQUIRE(e.attach());
        REQUIRE(e.wait_for([&] { return e.ui->frames() > 0; }));
        e.renderer.draw_delay_us.store(6000);
        bool queued = false;
        for (int attempt = 0; attempt < 20 && !queued; ++attempt) {
            e.ui->request_frame();
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.008]];
            [e.view() mouseDown:e.mouse(NSEventTypeLeftMouseDown, 190, 115)];
            [e.view() mouseUp:e.mouse(NSEventTypeLeftMouseUp, 190, 115)];
            // The click is queued: the field is not focused yet.
            queued = !e.ui->text_field_focused();
            if (!queued) {
                [e.view() mouseDown:e.mouse(NSEventTypeLeftMouseDown, 250, 180)];
                [e.view() mouseUp:e.mouse(NSEventTypeLeftMouseUp, 250, 180)];
                e.ui->flush_input();
            }
        }
        REQUIRE(queued);
        // The user clicked the field, then pressed Space: it is typed into the
        // field, not handed to the host's transport.
        REQUIRE(offer_editor_key(*e.ui, key_event(EditorKey::space, U' ')) ==
                KeyDisposition::consumed);
        REQUIRE(e.ui->text_field_focused());
        REQUIRE(field_value(e) == "ab ");
        e.renderer.draw_delay_us.store(200);
        e.ui->detach();
    }
}

TEST_CASE("losing window focus blurs a text field that has no id",
          "[native-surface][rmlui][keys]") {
    REQUIRE_METAL();
    @autoreleasepool {
        Editor e;
        REQUIRE(e.attach());
        REQUIRE(e.wait_for([&] { return e.ui->frames() > 0; }));
        e.ui->with_context(
            [&](Rml::Context& ctx) { ctx.GetDocument(0)->GetElementById("field")->SetId(""); });
        e.ui->with_context([&](Rml::Context& ctx) {
            for (int i = 0; i < ctx.GetDocument(0)->GetNumChildren(); ++i)
                if (auto* el = ctx.GetDocument(0)->GetChild(i); el->GetTagName() == "input")
                    el->Focus();
        });
        REQUIRE(e.ui->text_field_focused());
        forward_editor_focus(*e.ui, false);
        REQUIRE_FALSE(e.ui->text_field_focused());
        REQUIRE_FALSE(e.ui->surface()->text_focus());
        // Nothing to restore by id.
        forward_editor_focus(*e.ui, true);
        REQUIRE_FALSE(e.ui->text_field_focused());
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
        DrawCleanup<Editor> cleanup{{&e}};
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
        DrawCleanup<Editor> cleanup{{&e}};
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

TEST_CASE("flush_input on a surface that is not attached does nothing",
          "[native-surface][rmlui][threading]") {
    REQUIRE_METAL();
    @autoreleasepool {
        MemoryAssets assets;
        CountingRenderer renderer;
        RmlUiSurfaceOptions options;
        options.document = "probe.rml";
        options.font_faces = {"font.ttf"};
        RmlUiSurface fresh(assets, renderer, {}, options);
        fresh.flush_input();
        fresh.idle();
        REQUIRE(fresh.frames() == 0);

        Editor e;
        REQUIRE(e.attach());
        REQUIRE(e.wait_for([&] { return e.ui->frames() > 0; }));
        e.ui->detach();
        const auto frames = e.ui->frames();
        e.ui->flush_input();
        e.ui->with_context([](Rml::Context&) {});
        REQUIRE(e.ui->frames() == frames);
    }
}

// These synchronization contracts do not need a drawable or a Metal device.
TEST_CASE("contended input drain survives the last notification", "[rmlui][threading][cpu]") {
    std::mutex mutex;
    std::promise<void> owns_lock;
    std::promise<void> release_lock;
    auto release = release_lock.get_future();
    auto owned = owns_lock.get_future();
    std::atomic<bool> owner_timed_out{false};
    auto other_editor = std::async(std::launch::async, [&] {
        std::lock_guard lock(mutex);
        owns_lock.set_value();
        owner_timed_out.store(release.wait_for(std::chrono::seconds(10)) !=
                              std::future_status::ready);
    });
    const bool acquired = owned.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    int pending = 3;
    int delivered = 0;
    bool retry_posted = false;
    auto attempt = [&] {
        return detail::try_drain_input(
            mutex, [&] { retry_posted = true; },
            [&] {
                delivered += pending;
                pending = 0;
                return true;
            });
    };
    // Consume the only notification while an unrelated editor holds the lock.
    // No fatal assertion until the other thread has been released and joined.
    const bool applied_while_busy = attempt();
    const int delivered_while_busy = delivered;
    release_lock.set_value();
    other_editor.get();
    REQUIRE(acquired);
    REQUIRE_FALSE(owner_timed_out.load());
    CHECK_FALSE(applied_while_busy);
    CHECK(delivered_while_busy == 0);
    REQUIRE(retry_posted);
    // The unrelated editor posts nothing. Only the saved retry can deliver.
    retry_posted = false;
    CHECK(attempt());
    CHECK(delivered == 3);
    CHECK(pending == 0);
    CHECK_FALSE(retry_posted);
}

TEST_CASE("parked cleanup releases holds before joining on exception", "[rmlui][threading][cpu]") {
    struct ParkedEditor {
        struct Renderer {
            std::atomic<bool> hold_draws{true};
        } renderer;
        struct Surface {
            bool detached = false;
            void detach() {
                detached = true;
            }
        } surface;
        Surface* ui = &surface;
    } a, b;
    std::atomic<bool> exited{false};
    std::atomic<bool> timed_out{false};
    struct Unwind {};
    bool requester_started = false;
    try {
        std::promise<void> started;
        auto ready = started.get_future();
        DrawCleanup<ParkedEditor> cleanup{{&a, &b}};
        cleanup.requester = std::thread([&] {
            started.set_value();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!cleanup.stop.load() || a.renderer.hold_draws.load() ||
                   b.renderer.hold_draws.load()) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    timed_out.store(true);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            exited.store(true);
        });
        requester_started = ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        throw Unwind{};
    } catch (const Unwind&) {
    }
    CHECK(requester_started);
    CHECK_FALSE(timed_out.load());
    CHECK(exited.load());
    CHECK_FALSE(a.renderer.hold_draws.load());
    CHECK_FALSE(b.renderer.hold_draws.load());
    CHECK(a.surface.detached);
    CHECK(b.surface.detached);
}

TEST_CASE("a hidden editor stops frames and trims GPU memory, and showing it draws a full frame",
          "[native-surface][rmlui][visibility]") {
    REQUIRE_METAL();
    @autoreleasepool {
        Editor e;
        REQUIRE(e.attach());
        REQUIRE(e.wait_for([&] { return e.ui->frames() > 0; }));
        const int compiled_before = e.renderer.compiled.load();
        const int rendered_before = e.renderer.rendered.load();
        REQUIRE(e.renderer.released_geometry.load() == 0);
        REQUIRE(e.trimmed.load() == 0);

        e.ui->visibility_changed(false);
        // RmlUi handed back its geometry and textures through the renderer,
        // and the renderer was told to drop its own caches.
        REQUIRE(e.renderer.released_geometry.load() > 0);
        REQUIRE(e.trimmed.load() == 1);

        // Frames stay off while hidden, whatever asks for one and whatever the
        // window reports.
        const auto frames = e.ui->frames();
        for (int i = 0; i < 10; ++i) {
            e.ui->request_frame();
            e.ui->idle();
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        }
        REQUIRE(e.ui->frames() == frames);

        // Showing draws a complete frame: the geometry RmlUi released is
        // compiled again and drawn.
        e.ui->visibility_changed(true);
        REQUIRE(e.wait_for([&] { return e.ui->frames() > frames; }));
        REQUIRE(e.renderer.compiled.load() > compiled_before);
        REQUIRE(e.renderer.rendered.load() > rendered_before);
        REQUIRE(e.trimmed.load() == 1);
        e.ui->detach();
    }
}

TEST_CASE("hiding waits for the last submitted frame before releasing GPU resources",
          "[native-surface][rmlui][visibility]") {
    REQUIRE_METAL();
    @autoreleasepool {
        Editor e;
        REQUIRE(e.attach());
        REQUIRE(e.wait_for([&] { return e.ui->frames() > 0; }));
        // The next frame's command buffer waits on an event, so it is still on
        // the GPU, committed, when the editor is hidden.
        e.gate_next.store(true);
        const auto before = e.ui->frames();
        e.ui->request_frame();
        REQUIRE(e.wait_for([&] { return e.gated.load() && e.ui->frames() > before; }));
        REQUIRE(e.gated_cb.status != MTLCommandBufferStatusCompleted);
        e.renderer.first_release_ns.store(0);
        std::atomic<long long> signalled_ns{0};
        id<MTLSharedEvent> gate = e.gate;
        // Always signalled, so a regression fails the assertions, not the run.
        std::thread release([&signalled_ns, gate] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            signalled_ns.store(now_ns());
            gate.signaledValue = 1;
        });
        e.ui->visibility_changed(false);
        release.join();
        REQUIRE(e.trimmed.load() == 1);
        // Nothing was released, and the renderer was not trimmed, while the
        // frame could still use it.
        REQUIRE(e.trim_saw_status.load() == static_cast<int>(MTLCommandBufferStatusCompleted));
        REQUIRE(e.renderer.first_release_ns.load() >= signalled_ns.load());
        e.ui->detach();
    }
}

TEST_CASE("a renderer serves one attached surface at a time",
          "[native-surface][rmlui][visibility]") {
    REQUIRE_METAL();
    @autoreleasepool {
        Editor a;
        Editor b;
        REQUIRE(a.attach());
        RmlUiSurfaceOptions options;
        options.document = "probe.rml";
        options.font_faces = {"font.ttf"};
        // Hiding a would release this surface's geometry and textures too.
        RmlUiSurface shared(a.assets, a.renderer, {}, options);
        REQUIRE_FALSE(shared.attach((__bridge void*)b.window.contentView));
        REQUIRE_FALSE(shared.attached());
        REQUIRE(a.wait_for([&] { return a.ui->frames() > 0; }));
        // Detaching frees the renderer; an attach that fails after taking it
        // (a missing document) gives it back.
        a.ui->detach();
        RmlUiSurface missing(a.assets, a.renderer, {}, {"missing.rml"});
        REQUIRE_FALSE(missing.attach((__bridge void*)b.window.contentView));
        REQUIRE(shared.attach((__bridge void*)b.window.contentView));
        REQUIRE_FALSE(a.attach());
        shared.detach();
        REQUIRE(a.attach());
        a.ui->detach();
    }
}
