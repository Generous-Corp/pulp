// test_au_v2_cocoa_ui.mm — AU v2 Cocoa editor-view advertisement.
//
// Regression test for the bug ChainerSynth surfaced: no AU v2 adapter
// advertised kAudioUnitProperty_CocoaUI, so hosts (Logic, auval) never learned
// the plugin had a custom editor and showed their own generic param view. The
// fix wires a cross-TU filler hook (g_cocoa_view_info_filler) that the
// per-target Cocoa view module registers at static-init, and gives the factory
// class a per-plugin-unique name (PULP_AU_COCOA_VIEW_CLASS) so two Pulp AUs in
// one host don't collide on a process-global ObjC class name.
//
// This test compiles au_v2_cocoa_view.mm with PULP_AU_GUI + a test-unique
// class name and asserts the registration + the advertised view info, without
// constructing a real AudioComponentInstance.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#if defined(__APPLE__) && !TARGET_OS_IPHONE

#import <Foundation/Foundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AUCocoaUIView.h>

#include <pulp/format/au_v2_adapter.hpp>

TEST_CASE("AU v2 advertises a unique, resolvable Cocoa view factory",
          "[auv2][cocoa][editor]") {
    using namespace pulp::format::au;

    // The Cocoa view module's static-init registrar installs the filler when
    // linked (it's compiled into this test target with PULP_AU_GUI).
    REQUIRE(g_cocoa_view_info_filler != nullptr);

    AudioUnitCocoaViewInfo info{};
    REQUIRE(g_cocoa_view_info_filler(&info));

    // Bundle URL handed to the host (+1 retained; host owns/releases).
    REQUIRE(info.mCocoaAUViewBundleLocation != nullptr);

    // Advertised class name == the per-target unique name (set via the
    // PULP_AU_COCOA_VIEW_CLASS define on this test target below).
    REQUIRE(info.mCocoaAUViewClass[0] != nullptr);
    NSString* className = (__bridge NSString*)info.mCocoaAUViewClass[0];
    REQUIRE([className isEqualToString:@"PulpAUCocoaViewFactory_Test"]);

    // The advertised class resolves and conforms to AUCocoaUIBase — i.e. the
    // name we hand the host is the class we actually registered.
    Class cls = NSClassFromString(className);
    REQUIRE(cls != nil);
    REQUIRE([cls conformsToProtocol:@protocol(AUCocoaUIBase)]);

    // Release the +1 CF objects the filler handed us (the host would).
    CFRelease(info.mCocoaAUViewBundleLocation);
    CFRelease(info.mCocoaAUViewClass[0]);
}

TEST_CASE("AU v2 editor pins the design viewport per the shared resize contract",
          "[auv2][cocoa][editor][resize]") {
    // Regression coverage for the Logic-clipping bug: the AU v2 Cocoa view
    // never called set_design_viewport, so a DAW resize clipped the fixed-size
    // tree while VST3/AUv3 scaled it. The pin decision is the shared
    // should_pin_design_viewport() predicate (also used by PulpPlugView), so
    // the formats cannot drift again; assert its full three-way contract here.
    // The set_design_viewport call itself cannot be exercised headlessly —
    // uiViewForAudioUnit: refuses to build an editor in CI/test environments
    // (editor_launch_blocked_by_environment) — so the windowed behavior rides
    // the VST3 resize tests (test_vst3_editor.cpp) as the cross-format
    // contract, plus manual Logic/auval verification.
    using pulp::format::ViewSize;
    using pulp::format::should_pin_design_viewport;

    // Not resizable (min==0): pin at preferred (letterbox scaling).
    ViewSize fixed{};
    fixed.preferred_width = 640;
    fixed.preferred_height = 400;
    CHECK(should_pin_design_viewport(fixed));

    // Resizable + aspect-locked: pin viewport + aspect (design-import path).
    ViewSize locked = fixed;
    locked.min_width = 640;
    locked.min_height = 400;
    locked.aspect_ratio = 1.6;
    CHECK(should_pin_design_viewport(locked));

    // Resizable + aspect==0: free drag — NO pin, the root reflows via Yoga.
    ViewSize free_drag = locked;
    free_drag.aspect_ratio = 0.0;
    CHECK_FALSE(should_pin_design_viewport(free_drag));
    CHECK(native_view_aspect_ratio(free_drag) == 0.0);

    // Non-resizable legacy editors never had to spell out an aspect ratio;
    // adapters derived it from the preferred/pinned viewport.
    ViewSize legacy_fixed{};
    legacy_fixed.preferred_width = 640;
    legacy_fixed.preferred_height = 400;
    CHECK(should_pin_design_viewport(legacy_fixed));
    CHECK(native_view_aspect_ratio(legacy_fixed)
          == Catch::Approx(1.6).margin(0.0001));

    // Responsive + aspect-locked: constrain the host gesture but never apply
    // a fixed paint transform. This is Spectr's native resize contract.
    ViewSize responsive = locked;
    responsive.design_width = 1320;
    responsive.design_height = 860;
    responsive.viewport_policy = pulp::format::ViewportPolicy::Responsive;
    CHECK_FALSE(should_pin_design_viewport(responsive));
    CHECK(pulp::format::should_lock_view_aspect(responsive));
    CHECK(native_view_aspect_ratio(responsive)
          == Catch::Approx(responsive.aspect_ratio).margin(0.0001));

    // Degenerate preferred size: never pin (avoids a 0-aspect viewport).
    ViewSize degenerate{};
    degenerate.preferred_width = 0;
    degenerate.preferred_height = 0;
    CHECK_FALSE(should_pin_design_viewport(degenerate));
}

// ── Real editor lifetime ────────────────────────────────────────────────
//
// The factory builds a real PluginViewHost and returns its NSView. The host
// app's reference must decide the editor's lifetime: closing the editor
// (removing and dropping the view) has to deallocate the view, destroy the
// host, and fire Processor::on_view_closed. A view that the host itself kept
// alive leaked every editor (a GPU editor's Dawn device and surfaces with it)
// and never told the processor it closed.

#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#import <objc/message.h>

#include <pulp/canvas/canvas.hpp>
#include <pulp/format/au_v2_common.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/runtime/alive_token.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace {

class LifetimeProcessor : public pulp::format::Processor {
  public:
    int opened = 0;
    int closed = 0;
    pulp::format::PluginDescriptor descriptor() const override {
        return {"Lifetime", "Acme", "com.acme.lifetime", "1.0.0",
                pulp::format::PluginCategory::Effect};
    }
    void define_parameters(pulp::state::StateStore& s) override {
        s.add_parameter({1, "Gain", "dB", {-60.0f, 12.0f, 0.0f}});
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}
    std::unique_ptr<pulp::view::View> create_view() override {
        auto root = std::make_unique<pulp::view::View>();
        root->add_child(std::make_unique<pulp::view::View>());
        return root;
    }
    void on_view_opened(pulp::view::View&) override {
        ++opened;
    }
    void on_view_closed(pulp::view::View&) override;
    // Whether the last close ran while the editor view was still whole.
    bool closed_before_view_teardown = false;
};

// The AU instance the factory talks to: it only answers the private editor
// context property, which is all uiViewForAudioUnit: reads.
struct FakeEditorAU {
    AudioComponentPlugInInterface iface{};
    pulp::format::au::PulpEditorContext ctx{};
};
FakeEditorAU* g_fake_au_ctx_source = nullptr;

OSStatus fake_get_property(void* self, AudioUnitPropertyID id, AudioUnitScope, AudioUnitElement,
                           void* out, UInt32* io_size) {
    auto* au = static_cast<FakeEditorAU*>(self);
    if (id != pulp::format::au::kPulpEditorContextProperty || !out || !io_size ||
        *io_size < sizeof(au->ctx))
        return kAudioUnitErr_InvalidProperty;
    *static_cast<pulp::format::au::PulpEditorContext*>(out) = au->ctx;
    *io_size = sizeof(au->ctx);
    return noErr;
}
OSStatus fake_open(void* self, AudioComponentInstance) {
    static_cast<FakeEditorAU*>(self)->ctx = g_fake_au_ctx_source->ctx;
    return noErr;
}
OSStatus fake_close(void* self) {
    delete static_cast<FakeEditorAU*>(self);
    return noErr;
}
AudioComponentMethod fake_lookup(SInt16 selector) {
    if (selector == kAudioUnitGetPropertySelect)
        return reinterpret_cast<AudioComponentMethod>(&fake_get_property);
    return nullptr;
}
AudioComponentPlugInInterface* fake_factory(const AudioComponentDescription*) {
    auto* au = new FakeEditorAU; // iface is the first member: self == au
    au->iface.Open = fake_open;
    au->iface.Close = fake_close;
    au->iface.Lookup = fake_lookup;
    return &au->iface;
}

int g_view_deallocs = 0;

// NSView's own -dealloc is wrapped for the test so it can tell whether the
// editor closed before the view began tearing itself down (the plug-in view
// class's -dealloc runs its hook, then calls into NSView's).
void* g_editor_view = nullptr;
bool g_editor_nsview_dealloc_entered = false;
IMP g_nsview_dealloc = nullptr;
void wrapped_nsview_dealloc(id self, SEL sel) {
    if ((__bridge void*)self == g_editor_view)
        g_editor_nsview_dealloc_entered = true;
    ((void (*)(id, SEL))g_nsview_dealloc)(self, sel);
}

void LifetimeProcessor::on_view_closed(pulp::view::View&) {
    ++closed;
    closed_before_view_teardown = !g_editor_nsview_dealloc_entered;
}

} // namespace

// Associated with each returned view: released (and counted) when the view
// itself deallocates.
@interface PulpTestDeallocSentinel : NSObject
@end
@implementation PulpTestDeallocSentinel
- (void)dealloc {
    ++g_view_deallocs;
    [super dealloc];
}
@end

namespace {

// The factory refuses to build an editor under CI/test environment flags;
// lift them for this test and put them back after.
class EditorEnvironment {
  public:
    EditorEnvironment() {
        for (const char* k : kKeys) {
            const char* v = std::getenv(k);
            saved_.push_back(v ? std::optional<std::string>(v) : std::nullopt);
            ::unsetenv(k);
        }
    }
    ~EditorEnvironment() {
        for (std::size_t i = 0; i < saved_.size(); ++i)
            if (saved_[i])
                ::setenv(kKeys[i], saved_[i]->c_str(), 1);
    }

  private:
    static constexpr const char* kKeys[] = {"CI", "PULP_TEST_MODE", "PULP_HEADLESS",
                                            "PULP_DISABLE_PLUGIN_EDITOR"};
    std::vector<std::optional<std::string>> saved_;
};

} // namespace

TEST_CASE("AU v2 editor views close and deallocate when the host drops them",
          "[auv2][cocoa][editor][lifetime]") {
    EditorEnvironment env;
    LifetimeProcessor processor;
    pulp::state::StateStore store;
    processor.set_state_store(&store);
    processor.define_parameters(store);
    pulp::runtime::AliveToken alive;

    FakeEditorAU source;
    source.ctx.processor = &processor;
    source.ctx.store = &store;
    source.ctx.owner_alive = alive.capture();
    g_fake_au_ctx_source = &source;

    AudioComponentDescription desc{kAudioUnitType_Effect, 'LfTm', 'Acme', 0, 0};
    AudioComponent comp = AudioComponentRegister(&desc, CFSTR("Acme: Lifetime"), 1, fake_factory);
    REQUIRE(comp != nullptr);
    AudioUnit au = nullptr;
    REQUIRE(AudioComponentInstanceNew(comp, &au) == noErr);

    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 480, 320)
                                                   styleMask:NSWindowStyleMaskTitled
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    if (!window || !window.contentView)
        SKIP("No Cocoa window available.");
    Class factory_class = NSClassFromString(@"PulpAUCocoaViewFactory_Test");
    REQUIRE(factory_class != nil);

    constexpr int kOpens = 3;
    g_view_deallocs = 0;
    Method nsview_dealloc =
        class_getInstanceMethod([NSView class], NSSelectorFromString(@"dealloc"));
    g_nsview_dealloc = method_setImplementation(nsview_dealloc, (IMP)wrapped_nsview_dealloc);
    for (int i = 0; i < kOpens; ++i) {
        @autoreleasepool {
            id<AUCocoaUIBase> factory = [[factory_class alloc] init];
            NSView* view = [factory uiViewForAudioUnit:au withSize:NSMakeSize(480, 320)];
            [factory release];
            REQUIRE(view != nil);
            PulpTestDeallocSentinel* sentinel = [[PulpTestDeallocSentinel alloc] init];
            static char kSentinelKey;
            objc_setAssociatedObject(view, &kSentinelKey, sentinel,
                                     OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            [sentinel release];
            g_editor_view = (__bridge void*)view;
            g_editor_nsview_dealloc_entered = false;
            [window.contentView addSubview:view];
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
            REQUIRE(processor.opened == i + 1);
            [view removeFromSuperview]; // the host closes its editor window
        }
        @autoreleasepool {
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        }
        INFO("open " << i);
        CHECK(g_view_deallocs == i + 1);
        CHECK(processor.closed == i + 1);
        // The editor closed while its view was still whole: the host's
        // teardown messages that view.
        CHECK(processor.closed_before_view_teardown);
    }

    [window close];
    method_setImplementation(nsview_dealloc, g_nsview_dealloc);
    g_editor_view = nullptr;
    AudioComponentInstanceDispose(au);
    g_fake_au_ctx_source = nullptr;
}

namespace {

// A GPU-hosted editor, so the factory returns the GPU host's view.
class GpuHideProcessor : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {"GpuHide", "Acme", "com.acme.gpuhide", "1.0.0",
                pulp::format::PluginCategory::Effect};
    }
    void define_parameters(pulp::state::StateStore& s) override {
        s.add_parameter({1, "Gain", "dB", {-60.0f, 12.0f, 0.0f}});
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}
    std::unique_ptr<pulp::view::View> create_view() override {
        auto root = std::make_unique<pulp::view::View>();
        root->set_requires_gpu_host(true);
        // A blurred panel paints through a layer, so each frame leaves a
        // purgeable scratch target for the hide to return.
        auto panel = std::make_unique<pulp::view::View>();
        panel->set_background_color(pulp::canvas::Color::rgba8(90, 140, 220));
        panel->set_filter_blur(4.0f);
        panel->flex().preferred_width = 200;
        panel->flex().preferred_height = 120;
        root->add_child(std::move(panel));
        return root;
    }
};

} // namespace

TEST_CASE("AU v2 GPU editor view releases its GPU caches when the host hides it",
          "[auv2][cocoa][editor][gpu-trim]") {
    EditorEnvironment env;
    GpuHideProcessor processor;
    pulp::state::StateStore store;
    processor.set_state_store(&store);
    processor.define_parameters(store);
    pulp::runtime::AliveToken alive;

    FakeEditorAU source;
    source.ctx.processor = &processor;
    source.ctx.store = &store;
    source.ctx.owner_alive = alive.capture();
    g_fake_au_ctx_source = &source;

    AudioComponentDescription desc{kAudioUnitType_Effect, 'GpHd', 'Acme', 0, 0};
    AudioComponent comp = AudioComponentRegister(&desc, CFSTR("Acme: GpuHide"), 1, fake_factory);
    REQUIRE(comp != nullptr);
    AudioUnit au = nullptr;
    REQUIRE(AudioComponentInstanceNew(comp, &au) == noErr);

    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 480, 320)
                                                   styleMask:NSWindowStyleMaskTitled
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    if (!window || !window.contentView) {
        AudioComponentInstanceDispose(au);
        g_fake_au_ctx_source = nullptr;
        SKIP("No Cocoa window available.");
    }
    Class factory_class = NSClassFromString(@"PulpAUCocoaViewFactory_Test");
    REQUIRE(factory_class != nil);

    bool gpu_editor = false;
    @autoreleasepool {
        id<AUCocoaUIBase> factory = [[factory_class alloc] init];
        NSView* view = [factory uiViewForAudioUnit:au withSize:NSMakeSize(480, 320)];
        [factory release];
        REQUIRE(view != nil);
        [window.contentView addSubview:view];
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
        gpu_editor = [view isKindOfClass:NSClassFromString(@"PulpGpuPluginView")];
        if (gpu_editor) {
            // An AU v2 editor has no view controller: -viewDidHide on this view
            // is the only hide signal it gets, so its host must have wired it.
            CHECK([view valueForKey:@"onHide"] != nil);
            // Test-only accessor on the factory class: the host behind the view.
            auto* host = static_cast<pulp::view::PluginViewHost*>(
                ((void* (*)(id, SEL, NSView*))objc_msgSend)(
                    factory_class, @selector(pulpEditorHostForTesting:), view));
            REQUIRE(host != nullptr);
            REQUIRE(!host->capture_back_buffer_png().empty());
            const auto before = host->gpu_cache_usage();
            INFO("before hide: budgeted " << before.budgeted << " purgeable " << before.purgeable);
            REQUIRE(before.purgeable > 0);
            view.hidden = YES;
            const auto hidden = host->gpu_cache_usage();
            INFO("hidden: budgeted " << hidden.budgeted << " purgeable " << hidden.purgeable);
            CHECK(hidden.purgeable == 0);
            CHECK(hidden.budgeted < before.budgeted);
            view.hidden = NO;
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        }
        [view removeFromSuperview];
    }
    @autoreleasepool {
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
    }
    [window close];
    AudioComponentInstanceDispose(au);
    g_fake_au_ctx_source = nullptr;
    if (!gpu_editor)
        SKIP("CPU editor (no GPU adapter).");
}

#else

TEST_CASE("AU v2 Cocoa view advertisement is mac-only", "[auv2][cocoa]") {
    SUCCEED("Not macOS — AU v2 Cocoa view test is a no-op.");
}

#endif
